#include "Seedlathe.h"
#include "IPlug_include_in_plug_src.h"

#include "sl/FactoryPresets.h"
#include "sl/InstrumentGen.h"

#include <algorithm>
#include <cstdio>
#include <pmmintrin.h>
#include <vector>
#include <xmmintrin.h>

using seedlathe::ListControl;
using seedlathe::SeedBoxControl;
using seedlathe::TabBarControl;
using seedlathe::TypeName;

namespace {
// zyn's demo maps MIDI note 60 to zyn note 0 (middle C).
constexpr int kMidiMiddleC = 60;
constexpr int kMaxVoices = 64;

const IColor kBg(255, 22, 24, 28);
const IColor kPanel(255, 30, 33, 39);
const IColor kTextCol(255, 222, 228, 236);
const IColor kDim(255, 130, 140, 155);
const IColor kAccent(255, 120, 190, 255);

IVStyle DarkStyle() {
  // Every slot has to be set, not just the obvious ones. kPR in particular is
  // what IVTabbedPagesControl fills its page area with, so leaving it at the
  // default painted the whole tab body light blue.
  return DEFAULT_STYLE
      .WithColor(kBG, kPanel)                        // widget background
      .WithColor(kFG, IColor(255, 52, 58, 68))       // widget fill
      .WithColor(kPR, IColor(255, 26, 29, 34))       // pressed, and tab page body
      .WithColor(kFR, IColor(255, 70, 78, 90))       // frame
      .WithColor(kON, kAccent)
      .WithColor(kOFF, IColor(255, 44, 49, 58))
      .WithColor(kHL, IColor(40, 255, 255, 255))     // mouse-over wash
      .WithColor(kSH, IColor(0, 0, 0, 0))
      .WithColor(kX1, kAccent)
      .WithLabelText(IText(12.f, kDim))
      .WithValueText(IText(12.f, kTextCol))
      .WithDrawShadows(false);
}
} // namespace

Seedlathe::Seedlathe(const InstanceInfo& info)
: iplug::Plugin(info, MakeConfig(sl::kNumParams, kNumPresets))
{
  // Stepped integers so hosts quantise them exactly. See SeedlatheParams.h for
  // why the seed is split across two of them.
  GetParam(sl::kSeedHi)->InitInt("Seed Hi", 56506, 0, 65535);
  GetParam(sl::kSeedLo)->InitInt("Seed Lo", 7024, 0, 65535);
  GetParam(sl::kVolume)->InitDouble("Volume", 100., 0., 500., 0.1, "%");
  GetParam(sl::kOctave)->InitInt("Octave", 0, -3, 3);
  GetParam(sl::kVoices)->InitInt("Voices", 32, 1, kMaxVoices);
  GetParam(sl::kTypeFilter)->InitEnum("Type", 0, 11, "", IParam::kFlagsNone, "",
                                      "Any", "Pad", "Lead", "Bass", "Key", "Pluck",
                                      "Bell", "String", "Drum", "Perc", "FX");

#if IPLUG_EDITOR
  mMakeGraphicsFunc = [&]() {
    return MakeGraphics(*this, PLUG_WIDTH, PLUG_HEIGHT, PLUG_FPS,
                        GetScaleForScreen(PLUG_WIDTH, PLUG_HEIGHT));
  };

  mLayoutFunc = [&](IGraphics* g) {
    const IVStyle style = DarkStyle();

    g->AttachPanelBackground(kBg);
    g->EnableMouseOver(true);
    g->LoadFont("Roboto-Regular", ROBOTO_FN);

    const IRECT all = g->GetBounds();
    const IRECT top = all.GetFromTop(96.f).GetPadded(-10.f);
    const IRECT keys = all.GetFromBottom(150.f).GetPadded(-10.f);
    const IRECT mid = all.GetReducedFromTop(96.f).GetReducedFromBottom(150.f).GetPadded(-10.f);

    // ---- top bar -------------------------------------------------------
    g->AttachControl(new SeedBoxControl(top.GetFromLeft(200.f),
                                        [this](uint32_t s) { SetSeed(s); }, style),
                     kCtrlTagSeedBox);

    const IRECT btns = top.GetReducedFromLeft(208.f).GetFromLeft(300.f);
    g->AttachControl(new IVButtonControl(
        btns.GetGridCell(0, 1, 3).GetPadded(-3.f),
        [this](IControl*) { RollRandomSeed(); }, "Random", style));
    g->AttachControl(new IVButtonControl(
        btns.GetGridCell(1, 1, 3).GetPadded(-3.f),
        [this](IControl*) {
          if (mSearch.running()) mSearch.cancel();
          else mSearch.start(mInstruments[mLive.load(std::memory_order_acquire)], 0.0);
        }, "Find Similar", style));
    g->AttachControl(new IVButtonControl(
        btns.GetGridCell(2, 1, 3).GetPadded(-3.f),
        [this](IControl*) { mPool.allNotesOff(); }, "Panic", style));

    const IRECT knobs = top.GetFromRight(280.f);
    g->AttachControl(new IVKnobControl(knobs.GetGridCell(0, 1, 3).GetPadded(-4.f),
                                       sl::kVolume, "Volume", style));
    g->AttachControl(new IVKnobControl(knobs.GetGridCell(1, 1, 3).GetPadded(-4.f),
                                       sl::kOctave, "Octave", style));
    g->AttachControl(new IVKnobControl(knobs.GetGridCell(2, 1, 3).GetPadded(-4.f),
                                       sl::kTypeFilter, "Roll type", style));

    // ---- tabs ----------------------------------------------------------
    // iPlug2's IVTabbedPagesControl keys its pages on const char*, so the tab
    // order is whatever the linker produced, and AddPage is private. A small
    // tab bar plus iPlug2's control groups gives a stable order instead.
    const IRECT tabBar = mid.GetFromTop(30.f);
    const IRECT page = mid.GetReducedFromTop(36.f);

    g->AttachControl(new TabBarControl(
        tabBar, {"Instrument", "Presets", "Search", "Design"},
        [g](int index) {
          static const char* kGroups[] = {"instrument", "presets", "search", "design"};
          for (int i = 0; i < 4; ++i)
            g->ForControlInGroup(kGroups[i], [i, index](IControl* c) { c->Hide(i != index); });
        }));

    // -- Instrument
    g->AttachControl(new ITextControl(page.GetFromTop(20.f), "Loaded instrument",
                                      IText(12.f, kDim)), kNoTag, "instrument");
    g->AttachControl(new ITextControl(page.GetReducedFromTop(20.f).GetFromTop(30.f), "",
                                      IText(19.f, kTextCol)), kCtrlTagTypeLabel, "instrument");
    g->AttachControl(new IVScopeControl<1, 128>(page.GetReducedFromTop(58.f), "Output", style),
                     kCtrlTagScope, "instrument");

    // -- Presets
    g->AttachControl(new ITextControl(page.GetFromTop(20.f),
        "Factory bank -- 115 presets, grouped by instrument type",
        IText(12.f, kDim)), kNoTag, "presets");
    {
      auto* list = new ListControl(page.GetReducedFromTop(24.f), [this](int payload) {
        const auto& p = sl::kFactoryPresets[payload];
        SetSeed(p.seed);
        GetParam(sl::kOctave)->Set(std::clamp(p.octave, -3, 3));
        SendParameterValueFromDelegate(sl::kOctave,
                                       GetParam(sl::kOctave)->GetNormalized(), true);
      });

      // Grouped by instrument type, in type order, as the demo page does.
      std::vector<ListControl::Row> rows;
      for (int t = 0; t < 10; ++t) {
        bool header = false;
        for (int i = 0; i < sl::kNumFactoryPresets; ++i) {
          const auto& p = sl::kFactoryPresets[i];
          if (p.typeIndex != t) continue;
          if (!header) { rows.push_back({TypeName(t), "", true, -1}); header = true; }
          char detail[32];
          std::snprintf(detail, sizeof(detail), "%u", p.seed);
          rows.push_back({p.name, detail, false, i});
        }
      }
      list->SetRows(std::move(rows));
      g->AttachControl(list, kCtrlTagPresetList, "presets");
    }

    // -- Search
    g->AttachControl(new ITextControl(page.GetFromTop(46.f),
        "Searches seeds of the same instrument type and keeps the best match "
        "found. Cancelling keeps it too.",
        IText(12.f, kDim)), kNoTag, "search");
    {
      const IRECT row = page.GetReducedFromTop(50.f).GetFromTop(36.f).GetFromLeft(560.f);
      g->AttachControl(new IVButtonControl(row.GetGridCell(0, 1, 3).GetPadded(-4.f),
          [this](IControl*) {
            mSearch.start(mInstruments[mLive.load(std::memory_order_acquire)], 0.0);
          }, "Search", style), kNoTag, "search");
      g->AttachControl(new IVButtonControl(row.GetGridCell(1, 1, 3).GetPadded(-4.f),
          [this](IControl*) {
            mSearch.start(mInstruments[mLive.load(std::memory_order_acquire)],
                          mSearchThreshold);
          }, "Search until 90%", style), kNoTag, "search");
      g->AttachControl(new IVButtonControl(row.GetGridCell(2, 1, 3).GetPadded(-4.f),
          [this](IControl*) { mSearch.cancel(); }, "Stop", style), kNoTag, "search");
      g->AttachControl(new ITextControl(page.GetReducedFromTop(94.f).GetFromTop(28.f),
          "Idle", IText(15.f, kTextCol)), kCtrlTagSearchStatus, "search");
    }

    // -- Design
    g->AttachControl(new ITextControl(page, "Designer: not built yet",
        IText(14.f, IColor(255, 110, 118, 130))), kNoTag, "design");

    // Everything but the first page starts hidden.
    for (const char* grp : {"presets", "search", "design"})
      g->ForControlInGroup(grp, [](IControl* c) { c->Hide(true); });

    // ---- keyboard ------------------------------------------------------
    g->AttachControl(new IVKeyboardControl(keys), kCtrlTagKeyboard);

    RefreshSeedDisplay();
  };
#endif
}

#if IPLUG_DSP

void Seedlathe::SetSeed(uint32_t seed)
{
  GetParam(sl::kSeedHi)->Set(sl::seedHi(seed));
  GetParam(sl::kSeedLo)->Set(sl::seedLo(seed));
  SendParameterValueFromDelegate(sl::kSeedHi, GetParam(sl::kSeedHi)->GetNormalized(), true);
  SendParameterValueFromDelegate(sl::kSeedLo, GetParam(sl::kSeedLo)->GetNormalized(), true);
  RebuildInstrument();
  RefreshSeedDisplay();
}

void Seedlathe::RollRandomSeed()
{
  // xorshift: a roll only has to feel random, and this keeps no state worth
  // persisting.
  mRollState ^= mRollState << 13;
  mRollState ^= mRollState >> 17;
  mRollState ^= mRollState << 5;

  uint32_t seed = mRollState;
  const int filter = GetParam(sl::kTypeFilter)->Int();
  if (filter > 0) {
    // The last digit of the seed selects the instrument type, so constraining
    // the roll is just a matter of fixing that digit.
    seed = (seed / 10u) * 10u + static_cast<uint32_t>(filter - 1);
  }
  SetSeed(seed);
}

void Seedlathe::RefreshSeedDisplay()
{
#if IPLUG_EDITOR
  auto* ui = GetUI();
  if (!ui) return;

  const uint32_t seed = sl::seedFrom(GetParam(sl::kSeedHi)->Int(),
                                     GetParam(sl::kSeedLo)->Int());
  if (auto* c = ui->GetControlWithTag(kCtrlTagSeedBox))
    c->As<SeedBoxControl>()->SetSeed(seed);

  if (auto* c = ui->GetControlWithTag(kCtrlTagTypeLabel)) {
    const sl::Instrument& inst = mInstruments[mLive.load(std::memory_order_acquire)];
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s  -  %d oscillator%s%s",
                  TypeName(inst.typeIndex), inst.oscCount,
                  inst.oscCount == 1 ? "" : "s",
                  inst.hasFmMatrix ? "  -  FM matrix" : "");
    c->As<ITextControl>()->SetStr(buf);
  }
#endif
}

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
  if (paramIdx == sl::kSeedHi || paramIdx == sl::kSeedLo) {
    RebuildInstrument();
    RefreshSeedDisplay();
  }
}

void Seedlathe::OnIdle()
{
  mScopeSender.TransmitData(*this);

#if IPLUG_EDITOR
  auto* ui = GetUI();
  if (!ui) return;

  const bool running = mSearch.running();
  const auto best = mSearch.best();

  if (running || mSearchWasRunning) {
    if (auto* c = ui->GetControlWithTag(kCtrlTagSearchStatus)) {
      char buf[160];
      if (best.found)
        std::snprintf(buf, sizeof(buf), "%s  -  best %.1f%%  seed %u  (%llu tried)",
                      running ? "Searching" : "Done", best.score, best.seed,
                      static_cast<unsigned long long>(best.tested));
      else
        std::snprintf(buf, sizeof(buf), "%s...", running ? "Searching" : "Idle");
      c->As<ITextControl>()->SetStr(buf);
    }
  }

  // Adopt the winner once, when the search finishes.
  if (mSearchWasRunning && !running && best.found)
    SetSeed(best.seed);

  mSearchWasRunning = running;
#endif
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
  // and costs orders of magnitude more per operation.
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

  mScopeSender.ProcessBlock(outputs, nFrames, kCtrlTagScope, 1);

  _mm_setcsr(mxcsr);
}

#endif
