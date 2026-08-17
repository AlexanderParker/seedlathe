#include "Seedlathe.h"
#include "IPlug_include_in_plug_src.h"

#include "sl/InstrumentGen.h"

#include <algorithm>
#include <array>
#include <pmmintrin.h>
#include <xmmintrin.h>

namespace {
// zyn's demo maps MIDI note 60 to zyn note 0 (middle C).
constexpr int kMidiMiddleC = 60;
constexpr int kMaxVoices = 64;
} // namespace

Seedlathe::Seedlathe(const InstanceInfo& info)
: iplug::Plugin(info, MakeConfig(sl::kNumParams, kNumPresets))
{
  // Stepped integers so hosts quantise them exactly. See SeedlatheParams.h for
  // why the seed is split across two of them.
  GetParam(sl::kSeedHi)->InitInt("Seed Hi", 0, 0, 65535);
  GetParam(sl::kSeedLo)->InitInt("Seed Lo", 13, 0, 65535);
  GetParam(sl::kVolume)->InitDouble("Volume", 100., 0., 500., 0.1, "%");
  GetParam(sl::kOctave)->InitInt("Octave", 0, -3, 3);
  GetParam(sl::kVoices)->InitInt("Voices", 32, 1, kMaxVoices);

#if IPLUG_EDITOR
  mMakeGraphicsFunc = [&]() {
    return MakeGraphics(*this, PLUG_WIDTH, PLUG_HEIGHT, PLUG_FPS,
                        GetScaleForScreen(PLUG_WIDTH, PLUG_HEIGHT));
  };

  mLayoutFunc = [&](IGraphics* pGraphics) {
    pGraphics->AttachPanelBackground(IColor(255, 24, 26, 30));
    pGraphics->EnableMouseOver(true);
    pGraphics->LoadFont("Roboto-Regular", ROBOTO_FN);

    const IRECT b = pGraphics->GetBounds().GetPadded(-16.f);
    const IRECT top = b.GetFromTop(150.f);
    pGraphics->AttachControl(new IVNumberBoxControl(
        top.GetGridCell(0, 1, 4).GetCentredInside(120.f, 60.f), sl::kSeedHi, nullptr, "Seed Hi"));
    pGraphics->AttachControl(new IVNumberBoxControl(
        top.GetGridCell(1, 1, 4).GetCentredInside(120.f, 60.f), sl::kSeedLo, nullptr, "Seed Lo"));
    pGraphics->AttachControl(new IVKnobControl(
        top.GetGridCell(2, 1, 4).GetCentredInside(90.f), sl::kVolume, "Volume"));
    pGraphics->AttachControl(new IVKnobControl(
        top.GetGridCell(3, 1, 4).GetCentredInside(90.f), sl::kOctave, "Octave"));
    pGraphics->AttachControl(new IVKeyboardControl(b.GetFromBottom(160.f)), kCtrlTagKeyboard);
  };
#endif
}

#if IPLUG_DSP

void Seedlathe::RebuildInstrument()
{
  // iPlug2 fires OnParamChange while the plugin is still being constructed,
  // before OnReset has run, so there is a window where the rack has no pools
  // and no sample rate. Nothing to rebuild against yet; OnReset does it.
  if (!mPrepared)
    return;

  const auto seed = sl::seedFrom(GetParam(sl::kSeedHi)->Int(),
                                 GetParam(sl::kSeedLo)->Int());
  if (seed == mCurrentSeed)
    return;

  const int next = 1 - mLive.load(std::memory_order_relaxed);
  const sl::Instrument candidate = sl::generateInstrument(seed);

  // Impulse generation and its FFTs happen here, on the message thread, and
  // RackPool guarantees the rack it builds into is unreachable from audio.
  // A refusal means every rack is still sounding; the next parameter change
  // retries, so dragging the control simply coalesces.
  if (!mRacks.rebuild(candidate, mPool))
    return;

  mInstruments[next] = candidate;
  mLive.store(next, std::memory_order_release);
  mCurrentSeed = seed;
}

void Seedlathe::OnReset()
{
  const double sr = GetSampleRate();

  mRacks.prepare(sr, kNumRacks);
  mPreparedVoices = GetParam(sl::kVoices)->Int();
  mPool.prepare(sr, mPreparedVoices);
  mComp.prepare(sr);
  mComp.setParams(-12.0, 6.0, 8.0, 0.003, 0.15);   // zyn's Z.init settings

  // Generous headroom: a host may hand ProcessBlock more frames than the
  // reported block size, and growing the buffer there would allocate.
  const size_t blockCap = std::max<size_t>(static_cast<size_t>(GetBlockSize()), 1024) * 4;
  mLeft.assign(blockCap, 0.f);
  mRight.assign(blockCap, 0.f);

  mPrepared = true;
  mCurrentSeed = 0xFFFFFFFFu;
  RebuildInstrument();
}

void Seedlathe::OnParamChange(int paramIdx)
{
  if (paramIdx == sl::kSeedHi || paramIdx == sl::kSeedLo)
    RebuildInstrument();
}

void Seedlathe::ProcessMidiMsg(const IMidiMsg& msg)
{
  const int octave = GetParam(sl::kOctave)->Int();
  const auto status = msg.StatusMsg();

  if (status == IMidiMsg::kNoteOn && msg.Velocity() > 0)
  {
    // zyn's gain: 0.5 * volume * velocity, with 0.5 applied inside the voice.
    const double gain = (msg.Velocity() / 127.0) * (GetParam(sl::kVolume)->Value() / 100.0);
    const int note = msg.NoteNumber() - kMidiMiddleC + octave * 12;
    mPool.noteOn(mRacks.liveRack(),
                 mInstruments[mLive.load(std::memory_order_acquire)],
                 note, gain, true);
  }
  else if (status == IMidiMsg::kNoteOff ||
           (status == IMidiMsg::kNoteOn && msg.Velocity() == 0))
  {
    mPool.noteOff(msg.NoteNumber() - kMidiMiddleC + octave * 12);
  }
  else if (status == IMidiMsg::kControlChange &&
           msg.ControlChangeIdx() == IMidiMsg::kAllNotesOff)
  {
    mPool.allNotesOff();
  }
}

void Seedlathe::ProcessBlock(sample** inputs, sample** outputs, int nFrames)
{
  // Flush denormals for the duration of the callback. Decaying reverb and
  // delay tails run down toward 1e-38, where the CPU falls back to microcode
  // and costs orders of magnitude more per operation. Measured at only ~1.1x
  // here, but it is free and it is what every host expects a plugin to do.
  const unsigned mxcsr = _mm_getcsr();
  _mm_setcsr(mxcsr | 0x8040);   // FTZ | DAZ

  const int nChans = NOutChansConnected();

  if (static_cast<int>(mLeft.size()) < nFrames)
  {
    // Should not happen: OnReset sizes to the host block. Bail rather than
    // allocate on the audio thread.
    for (int c = 0; c < nChans; ++c)
      for (int s = 0; s < nFrames; ++s) outputs[c][s] = 0.;
    _mm_setcsr(mxcsr);
    return;
  }

  mRacks.audioBlockStarted(mPool);
  mPool.render(mLeft.data(), mRight.data(), nFrames,
               mRacks.all(), mRacks.allCount());

  for (int s = 0; s < nFrames; ++s)
  {
    double l = 0.0, r = 0.0;
    mComp.process(mLeft[s], mRight[s], l, r);   // Z.masterGain is unity
    if (nChans > 0) outputs[0][s] = l;
    if (nChans > 1) outputs[1][s] = r;
  }

  _mm_setcsr(mxcsr);
}

#endif
