#pragma once

#include "IPlug_include_in_plug_hdr.h"
#include "IControls.h"
#include "IVScopeControl.h"
#include "IVTabbedPagesControl.h"

#include "SeedlatheControls.h"
#include "SeedlatheDesigner.h"
#include "SeedlatheParams.h"
#include "RackPool.h"
#include "SampleMatch.h"
#include "SeedSearch.h"
#include "SharedFxRack.h"
#include "Voice.h"
#include "sl/Instrument.h"
#include "webaudio/WaCompressor.h"

#include <array>
#include <string>
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
  kCtrlTagOscSelect,
  kCtrlTagOscCount,
  kCtrlTagSampleInfo,
  kCtrlTagSampleStatus,
  kCtrlTagExportStatus,
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

  // The designer edits an instrument, not a parameter list, so the edit has to
  // travel in the state chunk. Params still carry the seed -- they are the
  // authoritative source for it -- and the chunk carries only the deviation
  // from what that seed generates.
  bool SerializeState(IByteChunk& chunk) const override;
  int UnserializeState(const IByteChunk& chunk, int startPos) override;

private:
  // Regenerates the edit buffer from the current Seed Hi/Lo, discarding any
  // designer edits, and queues it for publication. Message thread only.
  void RebuildInstrument(bool force = false);
  void SetSeed(uint32_t seed);
  void RollRandomSeed();
  void RefreshSeedDisplay();

  // The designer changed mEdit: mark it edited and queue publication.
  void PushEdit();

  // Tries to hand mEdit to the audio thread. RackPool may refuse -- every rack
  // is still sounding -- in which case the request stays queued and OnIdle
  // retries. Without the queue, the last value of a drag could be dropped: the
  // refusals during the drag coalesce harmlessly, but a refusal on the final
  // value would leave the sound permanently out of step with the controls.
  void ServicePending();

  // Re-reads every designer control from mEdit. Called after anything replaces
  // the instrument wholesale -- seed change, preset load, oscillator switch.
  void SyncDesigner();

  void SetOscCount(int n);
  void BuildDesigner(IGraphics* g, const IRECT& page, const IVStyle& style);
  void BuildSamplePage(IGraphics* g, const IRECT& page, const IVStyle& style);

  void LoadSampleTarget();
  void ExportWav();
  void RefreshSampleInfo();
  sl::Osc& EditOsc();

  // A pool of racks, not one. Rebuilding a rack frees and reallocates every
  // buffer inside it, so it may only ever target a rack the audio thread
  // cannot reach -- dragging the seed control used to segfault on exactly
  // that. RackPool owns the rule.
  static constexpr int kNumRacks = 4;
  sl::RackPool mRacks;

  sl::VoicePool mPool;
  sl::WaCompressor mComp;
  sl::SeedSearchRunner mSearch;

  // Sample matching renders every candidate, so it runs on its own pool rather
  // than sharing the parameter-space search's single thread.
  sl::SampleSearchRunner mSampleSearch;
  bool mSampleSearchWasRunning = false;
  std::string mSampleName;
  std::string mSampleError;

  // Double buffer: the message thread writes the inactive slot and flips the
  // index, the audio thread only ever reads the published one.
  std::array<sl::Instrument, 2> mInstruments{};
  std::atomic<int> mLive{0};

  // The designer's working copy. Edits land here, then ServicePending hands a
  // snapshot to the audio thread through the double buffer above.
  sl::Instrument mEdit{};
  bool mEdited = false;
  bool mPendingPublish = false;
  int mDesignOsc = 0;

  // Gathered during layout so the designer can be refreshed without RTTI.
  // Cleared at the top of the layout function, and only used while GetUI() is
  // non-null, so a closed editor cannot leave dangling entries behind.
  std::vector<seedlathe::DesignerControl*> mDesignerControls;

  std::vector<float> mLeft, mRight;
  uint32_t mCurrentSeed = 0;
  bool mHasSeed = false;
  bool mRacksBuilt = false;
  int mPreparedVoices = 0;
  bool mPrepared = false;

  IBufferSender<1, 8, 128> mScopeSender;
  bool mSearchWasRunning = false;
  double mSearchThreshold = 90.0;
  uint32_t mRollState = 0x9E3779B9u;
#endif
};
