#pragma once

#include "IPlug_include_in_plug_hdr.h"
#include "IControls.h"

#include "SeedlatheParams.h"
#include "sl/Instrument.h"
#include "SharedFxRack.h"
#include "Voice.h"
#include "webaudio/WaCompressor.h"

#include <array>
#include <atomic>
#include <vector>

const int kNumPresets = 1;

enum EControlTags
{
  kCtrlTagKeyboard = 0,
  kNumCtrlTags
};

using namespace iplug;
using namespace igraphics;

class Seedlathe final : public Plugin
{
public:
  Seedlathe(const InstanceInfo& info);

#if IPLUG_DSP
  void ProcessBlock(sample** inputs, sample** outputs, int nFrames) override;
  void ProcessMidiMsg(const IMidiMsg& msg) override;
  void OnReset() override;
  void OnParamChange(int paramIdx) override;

private:
  // Rebuilds the instrument from the current Seed Hi/Lo and prewarms its
  // reverbs. Message thread only -- prewarming allocates.
  void RebuildInstrument();

  sl::SharedFxRack mRack;
  sl::VoicePool mPool;
  sl::WaCompressor mComp;

  // Double buffer: the message thread writes the inactive slot and flips the
  // index, the audio thread only ever reads the published one.
  std::array<sl::Instrument, 2> mInstruments{};
  std::atomic<int> mLive{0};

  std::vector<float> mLeft, mRight;
  uint32_t mCurrentSeed = 0xFFFFFFFFu;
  int mPreparedVoices = 0;
  bool mPrepared = false;
#endif
};
