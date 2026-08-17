#pragma once

#include "IPlug_include_in_plug_hdr.h"
#include "IControls.h"
#include "IVScopeControl.h"
#include "IVTabbedPagesControl.h"

#include "SeedlatheControls.h"
#include "SeedlatheDesigner.h"
#include "SeedlatheParams.h"
#include "SeedlathePart.h"
#include "SampleMatch.h"
#include "Oversampler.h"
#include "UserPresets.h"
#include "SeedSearch.h"
#include "sl/Instrument.h"
#include "webaudio/WaCompressor.h"

#include <array>
#include <functional>
#include <string>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

const int kNumPresets = 1;

// Bumped whenever the layout after the parameters changes. A mis-read there
// loads the wrong instrument silently rather than failing, so the version is
// checked before anything is trusted.
constexpr int kStateVersion = 2;

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
  kCtrlTagPresetStatus,
  kCtrlTagPartStrip,
  kCtrlTagSampleResults,
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

  // Rebuilds every rate-dependent part of the engine at GetSampleRate() times
  // the oversampling factor. Allocates, so message thread only, and only when
  // the audio thread is known not to be inside ProcessBlock.
  void PrepareEngine();

  // Applies a change of oversampling factor, with the handshake that makes
  // reallocating under a running audio thread safe.
  void Reconfigure();

  // Runs `work` with the audio thread known not to be inside ProcessBlock.
  // Everything that reallocates engine buffers goes through here.
  void Quiesce(const std::function<void()>& work);
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
  void ServicePending(seedlathe::Part& part);
  void ServiceAllPending();

  // Allocates a part's engine if it has none yet. Message thread, behind the
  // same quiescence handshake Reconfigure uses.
  void EnsurePart(int index);
  void SelectPart(int index);

  // Re-reads every designer control from mEdit. Called after anything replaces
  // the instrument wholesale -- seed change, preset load, oscillator switch.
  void SyncDesigner();

  void SetOscCount(int n);
  void BuildDesigner(IGraphics* g, const IRECT& page, const IVStyle& style);
  void BuildSamplePage(IGraphics* g, const IRECT& page, const IVStyle& style);

  void LoadSampleTarget();
  void ExportWav();
  void RefreshSampleInfo();
  void RefreshResultList(int ctrlTag, const std::vector<sl::Candidate>& top);

  // Preset list and the user's own bank. Editor-only: they exist to drive
  // controls, and a DSP-only build has no list to refresh.
  void RefreshPresetList();
  void SetPresetStatus(const char* text);
  void LoadPreset(int payload);
  void PromptSavePreset();
  void PromptRenamePreset();
  void DeleteSelectedPreset();
  sl::Osc& EditOsc();

  // Sixteen parts, all but the first allocated only when something addresses
  // them. See SeedlathePart.h for why they cannot share racks and why that
  // makes lazy allocation worth the bookkeeping.
  std::array<seedlathe::Part, sl::kNumParts> mParts;
  int mEditPart = 0;
  bool mMulti = false;

  seedlathe::Part& P() { return mParts[static_cast<size_t>(mEditPart)]; }
  const seedlathe::Part& P() const { return mParts[static_cast<size_t>(mEditPart)]; }

  sl::WaCompressor mComp;
  sl::SeedSearchRunner mSearch;

  // Sample matching renders every candidate, so it runs on its own pool rather
  // than sharing the parameter-space search's single thread.
  sl::SampleSearchRunner mSampleSearch;
  bool mSampleSearchWasRunning = false;
  std::string mSampleName;
  std::string mSampleError;

  // Last published runners-up, so the lists are only rebuilt when they move.
  std::vector<sl::Candidate> mSearchTop, mSampleTop;

  sl::UserPresetStore mUserPresets;
  std::string mSelectedUserPreset;
  seedlathe::TextPromptControl* mPrompt = nullptr;



  // Gathered during layout so the designer can be refreshed without RTTI.
  // Cleared at the top of the layout function, and only used while GetUI() is
  // non-null, so a closed editor cannot leave dangling entries behind.
  std::vector<seedlathe::DesignerControl*> mDesignerControls;

  std::vector<float> mLeft, mRight;

  // Oversampling. 1, 2 or 4; the engine is prepared at that multiple of the
  // host rate and the decimator brings each block back down.
  int mOsFactor = 1;
  sl::Decimator mDecimL, mDecimR;
  std::vector<float> mDownL, mDownR;

  // Scratch for one part's render before it is summed into the bus.
  std::vector<float> mPartL, mPartR;

  // The handshake that lets the message thread reallocate the engine. Odd means
  // a block is in flight; the flag makes new blocks bail out without touching
  // anything. See Reconfigure.
  std::atomic<unsigned> mBlockSeq{0};
  std::atomic<bool> mReconfiguring{false};
  int mPreparedVoices = 0;
  bool mPrepared = false;

  IBufferSender<1, 8, 128> mScopeSender;
  bool mSearchWasRunning = false;
  double mSearchThreshold = 90.0;
  uint32_t mRollState = 0x9E3779B9u;
#endif
};
