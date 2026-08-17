#pragma once

#include "IPlug_include_in_plug_hdr.h"
#include "IControls.h"
#include "IVScopeControl.h"
#include "IVTabbedPagesControl.h"

#include "SeedlatheControls.h"
#include "SeedlatheParams.h"
#include "RackPool.h"
#include "SeedSearch.h"
#include "SharedFxRack.h"
#include "Voice.h"
#include "sl/Instrument.h"
#include "webaudio/WaCompressor.h"

#include <array>
#include <atomic>
#include <vector>

const int kNumPresets = 1;

enum EControlTags
{
  kCtrlTagKeyboard = 0,
  kCtrlTagScope,
  kCtrlTagSeedBox,
  kCtrlTagTypeLabel,
  kCtrlTagPresetList,
  kCtrlTagSearchStatus,
  kCtrlTagResultList,
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
  void OnIdle() override;

private:
  // Rebuilds the instrument from the current Seed Hi/Lo and prewarms its
  // reverbs. Message thread only -- prewarming allocates.
  void RebuildInstrument();
  void SetSeed(uint32_t seed);
  void RollRandomSeed();
  void RefreshSeedDisplay();

  // A pool of racks, not one. Rebuilding a rack frees and reallocates every
  // buffer inside it, so it may only ever target a rack the audio thread
  // cannot reach -- dragging the seed control used to segfault on exactly
  // that. RackPool owns the rule.
  static constexpr int kNumRacks = 4;
  sl::RackPool mRacks;

  sl::VoicePool mPool;
  sl::WaCompressor mComp;
  sl::SeedSearchRunner mSearch;

  // Double buffer: the message thread writes the inactive slot and flips the
  // index, the audio thread only ever reads the published one.
  std::array<sl::Instrument, 2> mInstruments{};
  std::atomic<int> mLive{0};

  std::vector<float> mLeft, mRight;
  uint32_t mCurrentSeed = 0xFFFFFFFFu;
  int mPreparedVoices = 0;
  bool mPrepared = false;

  IBufferSender<1, 8, 128> mScopeSender;
  bool mSearchWasRunning = false;
  double mSearchThreshold = 90.0;
  uint32_t mRollState = 0x9E3779B9u;
#endif
};
