#include "Seedlathe.h"
#include "IPlug_include_in_plug_src.h"

#include "sl/FactoryPresets.h"
#include "sl/InstrumentGen.h"
#include "OfflineRender.h"
#include "PresetIO.h"
#include "SampleMatch.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <cstring>
#include <pmmintrin.h>
#include <vector>
#include <xmmintrin.h>

using seedlathe::AdsrControl;
using seedlathe::DesignerControl;
using seedlathe::DragValueControl;
using seedlathe::FmMatrixControl;
using seedlathe::ListControl;
using seedlathe::OscSelectControl;
using seedlathe::PanelControl;
using seedlathe::SegmentControl;
using seedlathe::SwitchControl;
using seedlathe::SeedBoxControl;
using seedlathe::PartStripControl;
using seedlathe::TabBarControl;
using seedlathe::TypeName;

namespace {
// zyn's demo maps MIDI note 60 to zyn note 0 (middle C).
constexpr int kMidiMiddleC = 60;
constexpr int kMaxVoices = 64;

// C2 to C6, centred on middle C.
constexpr int kKeyboardLowNote = 36;
constexpr int kKeyboardHighNote = 84;

// Where the project lives. The About panel links to both, because the
// instruments this plugin generates are zyn's and a user who wants to know
// what a seed IS has to end up there.
constexpr const char* kSeedlatheUrl = "https://github.com/AlexanderParker/seedlathe";
constexpr const char* kZynUrl = "https://github.com/AlexanderParker/zyn";

const IColor kBg(255, 22, 24, 28);
const IColor kPanel(255, 30, 33, 39);
const IColor kTextCol(255, 222, 228, 236);
const IColor kDim(255, 130, 140, 155);
const IColor kAccent(255, 120, 190, 255);

// Wraps a plain callback as a button action function.
//
// IButtonControlBase sets the control's value to 1 on mouse-down and back to
// 0 only from OnEndAnimation -- and the only thing that starts an animation is
// iPlug2's own SplashClickActionFunc. A button given its own action function
// therefore latches: it draws pressed for ever after the first click, so the
// whole top bar looked permanently active. Calling the splash first restores
// both the reset and the click feedback that went missing with it.
IActionFunction Click(std::function<void()> fn) {
  return [fn = std::move(fn)](IControl* pCaller) {
    SplashClickActionFunc(pCaller);
    if (fn) fn();
  };
}

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
  // Off by default: oversampling is a deviation from zyn, which runs its graph
  // at the context rate, so 2x and 4x change band limiting, filter coefficients
  // and compressor timing. It is a quality option, not the reference.
  GetParam(sl::kOversample)->InitEnum("Oversampling", 0, 3, "", IParam::kFlagsNone, "",
                                      "Off", "2x", "4x");
  GetParam(sl::kMultitimbral)->InitBool("Multitimbral", false);

  // Four octaves each way is enough to sweep a seed's filter from closed to
  // wide open and back; the envelope it sits on top of is scaled to 20 kHz,
  // so anything wider only pins against the Nyquist clamp. Resonance is
  // bounded at the engine too -- see Voice::renderOscillator.
  GetParam(sl::kFilterCutoff)->InitDouble("Cutoff", 0., -48., 48., 0.01, "st");
  GetParam(sl::kFilterRes)->InitDouble("Resonance", 0., -30., 30., 0.01, "dB");

  for (auto& slot : mPendingProgram) slot.store(-1, std::memory_order_relaxed);

  // Two instances constructed in the same tick would otherwise roll the same
  // sequence, so the address of this instance goes in alongside the clock.
  // Neither is a cryptographic concern; the dice only have to differ.
  {
    const uint64_t now = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const uint64_t here = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(this));
    mRollState = static_cast<uint32_t>(now ^ (now >> 32) ^ here ^ (here >> 32));
    if (mRollState == 0) mRollState = 0x9E3779B9u;   // xorshift cannot leave 0
  }

  // Part 1 always exists, so it is requested from the start. Without this
  // SerializeState skipped it -- it only writes parts that something asked
  // for -- and every designer edit on it was silently dropped from the state
  // chunk. The seed survived anyway because it is a parameter, which is what
  // made the loss look like nothing was wrong.
  mParts[0].requested = true;

  // Presets live beside the host's own plugin data rather than next to the
  // binary: a VST3 folder is often read-only, and on Windows it is under
  // Program Files, where a write would be silently redirected per user anyway.
  {
    WDL_String dir;
    AppSupportPath(dir);
    // Forward slashes on every platform: the Win32 file APIs and
    // std::filesystem both accept them, so the path needs no separator switch.
    dir.Append("/Seedlathe/Presets");
    mUserPresets.open(dir.Get());
  }

#ifdef APP_API
  // Before the editor is built, so the layout function sees the restored seed
  // and part selection rather than the defaults.
  LoadStandaloneState();
#endif

#if IPLUG_EDITOR
  mMakeGraphicsFunc = [&]() {
    return MakeGraphics(*this, PLUG_WIDTH, PLUG_HEIGHT, PLUG_FPS,
                        GetScaleForScreen(PLUG_WIDTH, PLUG_HEIGHT));
  };

  mLayoutFunc = [&](IGraphics* g) {
    const IVStyle style = DarkStyle();

    // The previous editor's controls are gone; their pointers must not outlive
    // it. This runs on every editor open, before anything is attached.
    mDesignerControls.clear();
    mPrompt = nullptr;

    // Another instance may have saved something since this editor last opened.
    mUserPresets.refresh();

    g->AttachPanelBackground(kBg);
    g->EnableMouseOver(true);
    g->LoadFont("Roboto-Regular", ROBOTO_FN);

    const IRECT all = g->GetBounds();
    const IRECT top = all.GetFromTop(124.f).GetPadded(-10.f);
    const IRECT keys = all.GetFromBottom(150.f).GetPadded(-10.f);
    const IRECT mid = all.GetReducedFromTop(124.f).GetReducedFromBottom(150.f).GetPadded(-10.f);

    // ---- top bar -------------------------------------------------------
    //
    // Two rows deep, because everything here is something you reach for WHILE
    // playing -- the seed, the sounds either side of it, the type the dice
    // rolls, the part, and the four knobs. Anything you set once and forget
    // is on the Settings tab instead.
    g->AttachControl(new SeedBoxControl(top.GetFromLeft(180.f),
                                        [this](uint32_t s) { SetSeed(s); }, style),
                     kCtrlTagSeedBox);

    // A disabled vector control is greyed by SUBTRACTING 64 from each channel,
    // which on this dark palette lands on black and reads as a broken button
    // rather than an unavailable one. Starting 64 higher puts the disabled
    // state exactly where the other buttons' normal state sits.
    const IVStyle stepStyle = style.WithColor(kFG, IColor(255, 84, 92, 106));

    const IRECT btns = top.GetReducedFromLeft(188.f).GetFromLeft(230.f);
    const auto btn = [&](int col, int row, const char* label, const IVStyle& st,
                         std::function<void()> fn, int tag = kNoTag) {
      const IRECT cell(btns.L + btns.W() / 3.f * float(col),
                       btns.T + btns.H() / 2.f * float(row),
                       btns.L + btns.W() / 3.f * float(col + 1),
                       btns.T + btns.H() / 2.f * float(row + 1));
      g->AttachControl(new IVButtonControl(cell.GetPadded(-3.f), Click(std::move(fn)),
                                           label, st), tag);
    };

    btn(0, 0, "Random", style, [this] { RollRandomSeed(); });
    btn(1, 0, "Back", stepStyle, [this] { GoBack(); }, kCtrlTagBack);
    btn(2, 0, "Next", stepStyle, [this] { GoForward(); }, kCtrlTagNext);
    btn(0, 1, "Find Similar", style, [this, g] {
      // Idempotent while a search is running: the Stop button on the Search
      // tab is what stops one. Pressing this again used to cancel, so the
      // obvious "did I actually start it?" second press threw the search away.
      if (!mSearch.running()) StartSimilaritySearch(0.0);
      // Show the page that reports it either way, or the button looks like it
      // did nothing at all.
      if (auto* t = g->GetControlWithTag(kCtrlTagTabBar))
        t->As<TabBarControl>()->Select(2);
    });
    btn(1, 1, "Panic", style, [this] { P().pool.allNotesOff(); });
    btn(2, 1, "Stop Search", style, [this] { CancelSearches(); });

    g->AttachControl(new seedlathe::TypeGridControl(
        top.GetReducedFromLeft(426.f).GetFromLeft(240.f), sl::kTypeFilter));

    {
      const IRECT strip = top.GetReducedFromLeft(674.f).GetFromLeft(260.f);
      auto* parts = new PartStripControl(
          strip, sl::kNumParts,
          [this](int i) { return mParts[static_cast<size_t>(i)].allocated; },
          [this](int i) { SelectPart(i); });
      g->AttachControl(parts, kCtrlTagPartStrip);
    }

    // Cutoff and Resonance sit beside Volume and Octave because they belong to
    // the same class of control: things you move with a chord held down.
    const IRECT knobs = top.GetFromRight(300.f);
    g->AttachControl(new IVKnobControl(knobs.GetGridCell(0, 1, 4).GetPadded(-4.f),
                                       sl::kVolume, "Volume", style));
    g->AttachControl(new IVKnobControl(knobs.GetGridCell(1, 1, 4).GetPadded(-4.f),
                                       sl::kOctave, "Octave", style));
    g->AttachControl(new IVKnobControl(knobs.GetGridCell(2, 1, 4).GetPadded(-4.f),
                                       sl::kFilterCutoff, "Cutoff", style));
    g->AttachControl(new IVKnobControl(knobs.GetGridCell(3, 1, 4).GetPadded(-4.f),
                                       sl::kFilterRes, "Resonance", style));

    // ---- tabs ----------------------------------------------------------
    // iPlug2's IVTabbedPagesControl keys its pages on const char*, so the tab
    // order is whatever the linker produced, and AddPage is private. A small
    // tab bar plus iPlug2's control groups gives a stable order instead.
    const IRECT tabBar = mid.GetFromTop(30.f);
    const IRECT page = mid.GetReducedFromTop(36.f);

    g->AttachControl(new TabBarControl(
        tabBar, {"Instrument", "Presets", "Search", "Sample", "Design", "Settings"},
        [g](int index) {
          static const char* kGroups[] = {"instrument", "presets", "search",
                                          "sample", "design", "settings"};
          for (int i = 0; i < 6; ++i)
            g->ForControlInGroup(kGroups[i], [i, index](IControl* c) { c->Hide(i != index); });
        }), kCtrlTagTabBar);

    // -- Instrument
    //
    // The overview page: what is loaded, the output, and every component the
    // seed produced. The Design tab edits one oscillator at a time; this
    // answers "what IS this" without a single click.
    g->AttachControl(new ITextControl(page.GetFromTop(18.f), "Loaded instrument",
                                      IText(12.f, kDim)), kNoTag, "instrument");
    g->AttachControl(new ITextControl(page.GetReducedFromTop(18.f).GetFromTop(26.f), "",
                                      IText(19.f, kTextCol)), kCtrlTagTypeLabel, "instrument");
    // The scope draws its trace in kFG and its centre line in kSH, both of
    // which the dark palette sets to near-invisible greys. It looked like a
    // dead control until those two were given the accent instead.
    g->AttachControl(new IVScopeControl<1, 128>(
        page.GetReducedFromTop(50.f).GetFromTop(90.f), "Output",
        style.WithColor(kFG, kAccent).WithColor(kSH, IColor(255, 58, 65, 78))),
        kCtrlTagScope, "instrument");
    {
      auto* view = new seedlathe::InstrumentViewControl(
          page.GetReducedFromTop(148.f),
          [this]() -> const sl::Instrument& { return P().edit; });
      g->AttachControl(view, kCtrlTagComponents, "instrument");
      // Synced with the designer, so switching seed, preset or part repaints
      // it without anything else having to remember it exists.
      mDesignerControls.push_back(view);
    }

    // -- Presets
    {
      const IRECT bar = page.GetFromTop(30.f);
      g->AttachControl(new IVButtonControl(bar.GetFromLeft(150.f).GetPadded(-3.f),
          Click([this] { PromptSavePreset(); }), "Save current...", style),
          kNoTag, "presets");
      g->AttachControl(new IVButtonControl(
          bar.GetReducedFromLeft(150.f).GetFromLeft(110.f).GetPadded(-3.f),
          Click([this] { PromptRenamePreset(); }), "Rename...", style),
          kNoTag, "presets");
      g->AttachControl(new IVButtonControl(
          bar.GetReducedFromLeft(260.f).GetFromLeft(110.f).GetPadded(-3.f),
          Click([this] { DeleteSelectedPreset(); }), "Delete", style),
          kNoTag, "presets");
      g->AttachControl(new ITextControl(bar.GetReducedFromLeft(380.f),
          "", IText(12.f, kDim, nullptr, EAlign::Near)),
          kCtrlTagPresetStatus, "presets");

      auto* list = new ListControl(page.GetReducedFromTop(34.f),
                                   [this](int payload) { LoadPreset(payload); });
      g->AttachControl(list, kCtrlTagPresetList, "presets");
    }

    g->AttachControl(mPrompt = new seedlathe::TextPromptControl());

    // -- Search
    g->AttachControl(new ITextControl(page.GetFromTop(46.f),
        "Searches seeds of the same instrument type and keeps the best match "
        "found. Cancelling keeps it too.",
        IText(12.f, kDim)), kNoTag, "search");
    {
      const IRECT row = page.GetReducedFromTop(50.f).GetFromTop(36.f).GetFromLeft(560.f);
      // Starting is idempotent while one is running -- a second press must not
      // throw away the progress the first made.
      g->AttachControl(new IVButtonControl(row.GetGridCell(0, 1, 3).GetPadded(-4.f),
          Click([this] { if (!mSearch.running()) StartSimilaritySearch(0.0); }),
          "Search", style), kNoTag, "search");
      g->AttachControl(new IVButtonControl(row.GetGridCell(1, 1, 3).GetPadded(-4.f),
          Click([this] { if (!mSearch.running()) StartSimilaritySearch(mSearchThreshold); }),
          "Search until 90%", style), kNoTag, "search");
      g->AttachControl(new IVButtonControl(row.GetGridCell(2, 1, 3).GetPadded(-4.f),
          Click([this] { CancelSearches(); }), "Stop", style), kNoTag, "search");
      g->AttachControl(new ITextControl(page.GetReducedFromTop(94.f).GetFromTop(28.f),
          "Idle", IText(15.f, kTextCol)), kCtrlTagSearchStatus, "search");

      // The runners-up, not just the winner. A search that reports one seed
      // throws away the interesting part: the near misses are usually worth
      // hearing, and often preferred to the top score.
      g->AttachControl(new ITextControl(page.GetReducedFromTop(126.f).GetFromTop(18.f),
          "Best matches -- click to load",
          IText(11.f, kDim, nullptr, EAlign::Near)), kNoTag, "search");
      g->AttachControl(new ListControl(page.GetReducedFromTop(146.f),
          [this](int payload) { SetSeed(static_cast<uint32_t>(payload)); }),
          kCtrlTagResultList, "search");
    }

    // -- Sample match and WAV export
    BuildSamplePage(g, page, style);

    // -- Design
    BuildDesigner(g, page, style);

    // -- Settings
    BuildSettingsPage(g, page, style);

    // Everything but the first page starts hidden.
    for (const char* grp : {"presets", "search", "sample", "design", "settings"})
      g->ForControlInGroup(grp, [](IControl* c) { c->Hide(true); });

    // ---- keyboard ------------------------------------------------------
    //
    // Four octaves rather than iPlug2's default two. The width is fixed, so
    // the default put 25 keys across the whole window and every key was wider
    // than a real one; C2 to C6 is a keyboard you can actually play a bass
    // line and a melody on without reaching for the octave knob.
    g->AttachControl(new IVKeyboardControl(keys, kKeyboardLowNote, kKeyboardHighNote),
                     kCtrlTagKeyboard);

    RefreshPresetList();
    RefreshSeedDisplay();
  };
#endif
}

Seedlathe::~Seedlathe()
{
#ifdef APP_API
  SaveStandaloneState();
#endif
}

#ifdef APP_API

WDL_String Seedlathe::StandaloneStatePath()
{
  WDL_String path;
  AppSupportPath(path);
  path.Append("/Seedlathe/standalone.state");
  return path;
}

void Seedlathe::SaveStandaloneState()
{
  IByteChunk chunk;
  if (!SerializeState(chunk))
    return;

  // Only when something moved. Comparing the serialised bytes rather than
  // tracking a dirty flag at every mutation site means no edit can be missed
  // by forgetting to mark one.
  if (chunk.Size() == mSavedState.Size() && chunk.Size() > 0 &&
      std::memcmp(chunk.GetData(), mSavedState.GetData(),
                  static_cast<size_t>(chunk.Size())) == 0)
    return;

  // Best effort: a failure to write must not be what stops the app exiting.
  const WDL_String path = StandaloneStatePath();
  std::ofstream f(path.Get(), std::ios::binary | std::ios::trunc);
  if (!f) return;
  f.write(reinterpret_cast<const char*>(chunk.GetData()), chunk.Size());
  if (!f) return;

  mSavedState = chunk;
}

void Seedlathe::LoadStandaloneState()
{
  const WDL_String path = StandaloneStatePath();
  std::ifstream f(path.Get(), std::ios::binary);
  if (!f) return;

  const std::vector<char> bytes((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
  if (bytes.empty()) return;

  IByteChunk chunk;
  chunk.PutBytes(bytes.data(), static_cast<int>(bytes.size()));
  // UnserializeState checks its own version marker, so a file from a build
  // whose chunk layout differed is ignored rather than half-applied.
  UnserializeState(chunk, 0);
}

#endif // APP_API

#if IPLUG_DSP

#if IPLUG_EDITOR
namespace {
// The name tables live in SeedlatheDesigner.h now, so the read-only view on
// the Instrument tab names a waveform the same way the editor does.
using seedlathe::dsn::kWaveNames;
using seedlathe::dsn::kFilterNames;
using seedlathe::dsn::kOverNames;

int overIndex(int factor) { return factor >= 4 ? 2 : (factor >= 2 ? 1 : 0); }
int overFactor(int index) { return index == 2 ? 4 : (index == 1 ? 2 : 1); }
} // namespace

sl::Osc& Seedlathe::EditOsc()
{
  return P().edit.oscs[static_cast<size_t>(std::clamp(P().designOsc, 0, sl::kMaxOscs - 1))];
}

void Seedlathe::BuildDesigner(IGraphics* g, const IRECT& page, const IVStyle& style)
{
  const auto push = [this]() { PushEdit(); };

  // Small factories, so each control below reads as one line of intent rather
  // than five lines of plumbing.
  const auto panel = [&](const IRECT& r, const char* title) {
    g->AttachControl(new PanelControl(r, title), kNoTag, "design");
  };
  const auto track = [&](DesignerControl* c) {
    g->AttachControl(c, kNoTag, "design");
    mDesignerControls.push_back(c);
  };
  const auto val = [&](const IRECT& r, const char* label, double lo, double hi, int dec,
                       const char* unit, std::function<double()> get,
                       std::function<void(double)> set) {
    track(new DragValueControl(r, label, lo, hi, dec, unit, std::move(get), std::move(set), push));
  };
  const auto seg = [&](const IRECT& r, std::vector<const char*> items,
                       std::function<int()> get, std::function<void(int)> set) {
    track(new SegmentControl(r, std::move(items), std::move(get), std::move(set), push));
  };
  const auto sw = [&](const IRECT& r, const char* label,
                      std::function<bool()> get, std::function<void(bool)> set) {
    track(new SwitchControl(r, label, std::move(get), std::move(set), push));
  };
  const auto env = [&](const IRECT& r, const char* title, const char* hint,
                       std::function<sl::Adsr()> get, std::function<void(const sl::Adsr&)> set) {
    track(new AdsrControl(r, title, hint, std::move(get), std::move(set), push));
  };

  // ---- oscillator selector ------------------------------------------------
  const IRECT selRow = page.GetFromTop(26.f);

  auto* oscSel = new OscSelectControl(
      selRow.GetFromLeft(400.f),
      [this]() { return P().edit.oscCount; },
      [this](int i) { P().designOsc = i; SyncDesigner(); });
  g->AttachControl(oscSel, kCtrlTagOscSelect, "design");
  mDesignerControls.push_back(oscSel);

  const IRECT countRow = selRow.GetReducedFromLeft(412.f).GetFromLeft(200.f);
  g->AttachControl(new IVButtonControl(countRow.GetFromLeft(30.f),
      Click([this] { SetOscCount(P().edit.oscCount - 1); }), "-", style), kNoTag, "design");
  g->AttachControl(new ITextControl(countRow.GetReducedFromLeft(34.f).GetFromLeft(130.f), "",
      IText(11.f, IColor(255, 214, 221, 230))), kCtrlTagOscCount, "design");
  g->AttachControl(new IVButtonControl(countRow.GetFromRight(30.f),
      Click([this] { SetOscCount(P().edit.oscCount + 1); }), "+", style), kNoTag, "design");

  // Four columns. The instrument carries far more surface than a knob-per-field
  // layout could hold at this window size, so it is grouped by what a sound
  // designer reaches for together rather than by the struct's field order.
  const float colW = (page.W() - 30.f) / 4.f;
  const auto col = [&](int i) {
    const float l = page.L + (colW + 10.f) * static_cast<float>(i);
    return IRECT(l, page.T + 34.f, l + colW, page.B);
  };
  const auto rowIn = [](const IRECT& c, float top, float h) {
    return IRECT(c.L + 8.f, c.T + top, c.R - 8.f, c.T + top + h);
  };

  // ---- column 0: the oscillator itself ------------------------------------
  {
    const IRECT c = col(0);
    const IRECT p(c.L, c.T, c.R, c.T + 118.f);
    panel(p, "OSCILLATOR");
    seg(rowIn(p, 18.f, 20.f),
        {kWaveNames[0], kWaveNames[1], kWaveNames[2], kWaveNames[3], kWaveNames[4]},
        [this]() { return static_cast<int>(EditOsc().waveform); },
        [this](int i) { EditOsc().waveform = static_cast<sl::Waveform>(i); });
    val(rowIn(p, 44.f, 20.f), "OCTAVE", -3.0, 3.0, 0, "",
        [this]() { return double(EditOsc().oct); },
        [this](double v) { EditOsc().oct = static_cast<int>(std::lround(v)); });
    val(rowIn(p, 68.f, 20.f), "DETUNE", -12.0, 12.0, 2, " st",
        [this]() { return EditOsc().detune; },
        [this](double v) { EditOsc().detune = v; });
    // Carried for the similarity scorer only: zyn never assigns the filter's
    // type, so every filter in the audio path is a lowpass. Changing this moves
    // the seed's neighbours, not its sound.
    seg(rowIn(p, 92.f, 20.f),
        {kFilterNames[0], kFilterNames[1], kFilterNames[2], kFilterNames[3],
         kFilterNames[4], kFilterNames[5], kFilterNames[6]},
        [this]() { return static_cast<int>(EditOsc().filterType); },
        [this](int i) { EditOsc().filterType = static_cast<sl::FilterType>(i); });

    env(IRECT(c.L, c.T + 126.f, c.R, c.T + 246.f), "GAIN ENVELOPE", "x note gain",
        [this]() { return EditOsc().adsrGain; },
        [this](const sl::Adsr& a) { EditOsc().adsrGain = a; });
    env(IRECT(c.L, c.T + 252.f, c.R, c.T + 372.f), "FILTER ENVELOPE", "x 20 kHz",
        [this]() { return EditOsc().adsrFilter; },
        [this](const sl::Adsr& a) { EditOsc().adsrFilter = a; });
    env(IRECT(c.L, c.T + 378.f, c.R, c.B), "FILTER Q ENVELOPE", "x 30 dB",
        [this]() { return EditOsc().adsrFilterQ; },
        [this](const sl::Adsr& a) { EditOsc().adsrFilterQ = a; });
  }

  // ---- column 1: modulation -----------------------------------------------
  {
    const IRECT c = col(1);
    struct LfoRef { const char* title; double depthMax; int dec; const char* unit; };
    const LfoRef refs[3] = {
        {"GAIN LFO",   1.0,    3, ""},
        {"FILTER LFO", 8000.0, 0, " Hz"},
        {"PITCH LFO",  12.0,   2, " x f"},
    };
    for (int k = 0; k < 3; ++k) {
      const IRECT p(c.L, c.T + 116.f * static_cast<float>(k),
                    c.R, c.T + 116.f * static_cast<float>(k) + 108.f);
      panel(p, refs[k].title);
      // One accessor per panel: the LFO it edits depends on the selected
      // oscillator, which changes under the control after it is built.
      const auto pick = [this, k]() -> sl::Lfo& {
        sl::Osc& o = EditOsc();
        return k == 0 ? o.gLfo : (k == 1 ? o.fLfo : o.pLfo);
      };
      sw(rowIn(p, 16.f, 20.f), "ON",
         [pick]() { return pick().on; }, [pick](bool b) { pick().on = b; });
      seg(rowIn(p, 40.f, 20.f), {kWaveNames[0], kWaveNames[1], kWaveNames[2], kWaveNames[3]},
          [pick]() { return static_cast<int>(pick().type); },
          [pick](int i) { pick().type = static_cast<sl::Waveform>(i); });
      val(rowIn(p, 62.f, 20.f), "RATE", 0.0, 101.0, 2, " Hz",
          [pick]() { return pick().frequency; }, [pick](double v) { pick().frequency = v; });
      val(rowIn(p, 84.f, 20.f), "DEPTH", 0.0, refs[k].depthMax, refs[k].dec, refs[k].unit,
          [pick]() { return pick().depth; }, [pick](double v) { pick().depth = v; });
    }

    const IRECT p(c.L, c.T + 348.f, c.R, c.T + 456.f);
    panel(p, "FM OSCILLATOR");
    sw(rowIn(p, 16.f, 20.f), "ON",
       [this]() { return EditOsc().fm.on; }, [this](bool b) { EditOsc().fm.on = b; });
    seg(rowIn(p, 40.f, 20.f), {kWaveNames[0], kWaveNames[1], kWaveNames[2], kWaveNames[3]},
        [this]() { return static_cast<int>(EditOsc().fm.type); },
        [this](int i) { EditOsc().fm.type = static_cast<sl::Waveform>(i); });
    val(rowIn(p, 62.f, 20.f), "RATIO", 0.0, 101.0, 3, " x",
        [this]() { return EditOsc().fm.frequency; },
        [this](double v) { EditOsc().fm.frequency = v; });
    val(rowIn(p, 84.f, 20.f), "DEPTH", 0.0, 500.0, 1, " Hz",
        [this]() { return EditOsc().fm.depth; },
        [this](double v) { EditOsc().fm.depth = v; });
  }

  // ---- column 2: pitch envelope, distortion, per-oscillator FX -------------
  {
    const IRECT c = col(2);
    IRECT p(c.L, c.T, c.R, c.T + 62.f);
    panel(p, "PITCH ENVELOPE");
    sw(rowIn(p, 16.f, 20.f), "ON",
       [this]() { return EditOsc().pEnv.on; }, [this](bool b) { EditOsc().pEnv.on = b; });
    val(rowIn(p, 38.f, 20.f), "AMOUNT", 0.0, 4.0, 3, " x f",
        [this]() { return EditOsc().pEnv.amount; },
        [this](double v) { EditOsc().pEnv.amount = v; });
    env(IRECT(c.L, c.T + 68.f, c.R, c.T + 176.f), "PITCH ENV SHAPE", "x amount",
        [this]() { return EditOsc().pEnv.env; },
        [this](const sl::Adsr& a) { EditOsc().pEnv.env = a; });

    p = IRECT(c.L, c.T + 182.f, c.R, c.T + 266.f);
    panel(p, "DISTORTION");
    sw(rowIn(p, 16.f, 20.f), "ON",
       [this]() { return EditOsc().dist.on; }, [this](bool b) { EditOsc().dist.on = b; });
    val(rowIn(p, 38.f, 20.f), "AMOUNT", 0.0, 500.0, 1, "",
        [this]() { return EditOsc().dist.amount; },
        [this](double v) { EditOsc().dist.amount = v; });
    seg(rowIn(p, 60.f, 20.f), {kOverNames[0], kOverNames[1], kOverNames[2]},
        [this]() { return overIndex(EditOsc().dist.oversample); },
        [this](int i) { EditOsc().dist.oversample = overFactor(i); });

    p = IRECT(c.L, c.T + 272.f, c.R, c.T + 356.f);
    panel(p, "DELAY");
    sw(rowIn(p, 16.f, 20.f), "ON",
       [this]() { return EditOsc().del.on; }, [this](bool b) { EditOsc().del.on = b; });
    val(rowIn(p, 38.f, 20.f), "TIME", 0.0, 2.0, 3, " s",
        [this]() { return EditOsc().del.time; },
        [this](double v) { EditOsc().del.time = v; });
    val(rowIn(p, 60.f, 20.f), "FEEDBACK", 0.0, 0.95, 3, "",
        [this]() { return EditOsc().del.feedback; },
        [this](double v) { EditOsc().del.feedback = v; });

    p = IRECT(c.L, c.T + 362.f, c.R, c.T + 446.f);
    panel(p, "REVERB");
    sw(rowIn(p, 16.f, 20.f), "ON",
       [this]() { return EditOsc().verb.on; }, [this](bool b) { EditOsc().verb.on = b; });
    val(rowIn(p, 38.f, 20.f), "DURATION", 0.05, 6.0, 3, " s",
        [this]() { return EditOsc().verb.duration; },
        [this](double v) { EditOsc().verb.duration = v; });
    val(rowIn(p, 60.f, 20.f), "DECAY", 0.0, 4.0, 3, "",
        [this]() { return EditOsc().verb.decay; },
        [this](double v) { EditOsc().verb.decay = v; });
  }

  // ---- column 3: FM matrix and patch actions ------------------------------
  {
    const IRECT c = col(3);
    const IRECT p(c.L, c.T, c.R, c.T + 294.f);
    panel(p, "FM MATRIX");
    sw(rowIn(p, 16.f, 20.f), "MATRIX ON",
       [this]() { return P().edit.hasFmMatrix; }, [this](bool b) { P().edit.hasFmMatrix = b; });
    track(new FmMatrixControl(
        rowIn(p, 40.f, 246.f),
        [this](int src, int tgt) {
          return P().edit.fmMatrix[static_cast<size_t>(src)][static_cast<size_t>(tgt)];
        },
        [this](int src, int tgt, double v) {
          P().edit.fmMatrix[static_cast<size_t>(src)][static_cast<size_t>(tgt)] = v;
        },
        [this]() { return P().edit.oscCount; },
        push));

    const IRECT q(c.L, c.T + 300.f, c.R, c.T + 392.f);
    panel(q, "PATCH");
    g->AttachControl(new IVButtonControl(rowIn(q, 18.f, 22.f),
        Click([this] {
          if (auto* ui = GetUI())
            ui->SetTextInClipboard(sl::instrumentToJson(P().edit).dump(2).c_str());
        }), "Copy JSON", style), kNoTag, "design");
    g->AttachControl(new IVButtonControl(rowIn(q, 44.f, 22.f),
        Click([this] {
          auto* ui = GetUI();
          if (!ui) return;
          WDL_String text;
          if (!ui->GetTextFromClipboard(text) || !text.GetLength()) return;
          // Anything at all can be on the clipboard, so a parse failure has to
          // leave the current patch untouched rather than half-overwrite it.
          sl::Instrument parsed;
          try {
            parsed = sl::instrumentFromJson(nlohmann::json::parse(text.Get()));
          } catch (const std::exception&) {
            return;
          }
          PushHistory();
          P().edit = parsed;
          P().designOsc = 0;
          PushEdit();
          SyncDesigner();
        }), "Paste JSON", style), kNoTag, "design");
    g->AttachControl(new IVButtonControl(rowIn(q, 70.f, 22.f),
        Click([this] { PushHistory(); RebuildInstrument(true); }),
        "Revert to seed", style), kNoTag, "design");
  }
}
#endif // IPLUG_EDITOR

void Seedlathe::SetOscCount(int n)
{
  n = std::clamp(n, 1, sl::kMaxOscs);
  if (n == P().edit.oscCount) return;

  // A new slot is a copy of the last one, not a default Osc. A default Osc has
  // an all-zero gain envelope, so adding an oscillator would appear to do
  // nothing at all until the user rebuilt its envelope by hand.
  for (int i = P().edit.oscCount; i < n; ++i)
    P().edit.oscs[static_cast<size_t>(i)] =
        P().edit.oscs[static_cast<size_t>(std::max(0, P().edit.oscCount - 1))];

  P().edit.oscCount = n;
  if (P().designOsc >= n) P().designOsc = n - 1;
  PushEdit();
  SyncDesigner();
}

void Seedlathe::PushEdit()
{
  P().edited = true;
  P().pendingPublish = true;
  ServicePending(P());
  RefreshSeedDisplay();
}

namespace {
// The shared rack is built entirely from each oscillator's delay and reverb
// settings -- SharedFxRack::prewarm walks exactly those. Everything else in the
// instrument is read per note, so it needs no rack work at all.
bool sameFxConfig(const sl::Instrument& a, const sl::Instrument& b)
{
  if (a.oscCount != b.oscCount)
    return false;
  for (int i = 0; i < a.oscCount; ++i) {
    const sl::Osc& x = a.oscs[static_cast<size_t>(i)];
    const sl::Osc& y = b.oscs[static_cast<size_t>(i)];
    if (x.del.on != y.del.on || x.del.time != y.del.time || x.del.feedback != y.del.feedback)
      return false;
    if (x.verb.on != y.verb.on || x.verb.duration != y.verb.duration || x.verb.decay != y.verb.decay)
      return false;
  }
  return true;
}
} // namespace

void Seedlathe::ServicePending(seedlathe::Part& part)
{
  if (!part.pendingPublish || !mPrepared || !part.allocated)
    return;

  const int live = part.live.load(std::memory_order_relaxed);

  // Dragging an envelope handle emits an edit per mouse move. Rebuilding a rack
  // for each one would regenerate every reverb impulse -- an FFT per drag frame
  // -- and would be refused whenever the racks were busy, so the edit would not
  // be heard until the note ended. Only delay and reverb changes need the rack.
  if (part.racksBuilt &&
      sameFxConfig(part.edit, part.instruments[static_cast<size_t>(live)])) {
    part.instruments[static_cast<size_t>(1 - live)] = part.edit;
    part.live.store(1 - live, std::memory_order_release);
    part.pendingPublish = false;
    return;
  }

  // Impulse generation and its FFTs happen here, on the message thread, and
  // RackPool guarantees the rack it builds into is unreachable from audio.
  if (!part.racks.rebuild(part.edit, part.pool))
    return;

  part.instruments[static_cast<size_t>(1 - live)] = part.edit;
  part.live.store(1 - live, std::memory_order_release);
  part.racksBuilt = true;
  part.pendingPublish = false;
}

void Seedlathe::ServicePendingPrograms()
{
  for (int i = 0; i < sl::kNumParts; ++i) {
    const int program =
        mPendingProgram[static_cast<size_t>(i)].exchange(-1, std::memory_order_acq_rel);
    if (program < 0 || program >= sl::kNumFactoryPresets) continue;
    if (!mMulti && i != 0) continue;

    // Deliberately not LoadPreset: that also drives the preset list's own
    // selection state and lives with the editor, and a program change has to
    // work whether or not anyone has the window open. The list highlights the
    // result anyway, because it matches on the seed rather than on a click.
    // Allocate before setting the seed, not after. EnsurePart seeds a fresh
    // part from part 1 when it has none, so running it second would overwrite
    // the seed the program change just asked for.
    const int was = mEditPart;
    if (mMulti) {
      EnsurePart(i);
      mEditPart = i;
    }

    const auto& preset = sl::kFactoryPresets[program];
    SetSeed(preset.seed);
    GetParam(sl::kOctave)->Set(std::clamp(preset.octave, -3, 3));
    SendParameterValueFromDelegate(sl::kOctave,
                                   GetParam(sl::kOctave)->GetNormalized(), true);

    // In multitimbral mode leave the editor on the part the host addressed --
    // that is where the user needs to be looking. Otherwise put it back.
    if (!mMulti) mEditPart = was;
    else SyncDesigner();
  }
}

void Seedlathe::ServiceAllPending()
{
  // Every allocated part, not just the edited one: a refused rebuild on a part
  // the user has since navigated away from would otherwise never be retried.
  for (auto& part : mParts)
    ServicePending(part);
}

void Seedlathe::SyncDesigner()
{
#if IPLUG_EDITOR
  if (!GetUI())
    return;
  for (auto* c : mDesignerControls)
    c->Sync();
  RefreshSeedDisplay();
#endif
}

#if IPLUG_EDITOR

// User presets sort above the factory bank and their payloads are offset past
// it, so one list can carry both without the picker having to know which list
// row came from where.
static constexpr int kUserPresetBase = 100000;

void Seedlathe::RefreshPresetList()
{
  auto* ui = GetUI();
  if (!ui) return;
  auto* c = ui->GetControlWithTag(kCtrlTagPresetList);
  if (!c) return;

  std::vector<ListControl::Row> rows;

  const auto& user = mUserPresets.presets();
  if (!user.empty()) {
    rows.push_back({"Your presets", "", true, -1});
    for (size_t i = 0; i < user.size(); ++i) {
      char detail[64];
      std::snprintf(detail, sizeof(detail), "%u%s", user[i].seed,
                    user[i].edited ? "  edited" : "");
      rows.push_back({user[i].name, detail, false,
                      kUserPresetBase + static_cast<int>(i)});
    }
  }

  // Grouped by instrument type, in type order, as the demo page does.
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

  c->As<ListControl>()->SetRows(std::move(rows));
}

void Seedlathe::SetPresetStatus(const char* text)
{
  auto* ui = GetUI();
  if (!ui) return;
  if (auto* c = ui->GetControlWithTag(kCtrlTagPresetStatus))
    c->As<ITextControl>()->SetStr(text ? text : "");
}

void Seedlathe::LoadPreset(int payload)
{
  if (payload >= kUserPresetBase) {
    const size_t i = static_cast<size_t>(payload - kUserPresetBase);
    if (i >= mUserPresets.presets().size()) return;
    const sl::UserPreset& p = mUserPresets.presets()[i];
    mSelectedUserPreset = p.name;

    SetSeed(p.seed);
    // The seed load has already regenerated P().edit, so the stored instrument
    // goes on top of it -- and only when the preset actually holds one.
    if (p.edited) {
      P().edit = p.instrument;
      P().designOsc = 0;
      PushEdit();
      SyncDesigner();
    }
    GetParam(sl::kOctave)->Set(std::clamp(p.octave, -3, 3));
    SendParameterValueFromDelegate(sl::kOctave,
                                   GetParam(sl::kOctave)->GetNormalized(), true);
    SetPresetStatus("");
    return;
  }

  if (payload < 0 || payload >= sl::kNumFactoryPresets) return;
  const auto& p = sl::kFactoryPresets[payload];
  mSelectedUserPreset.clear();
  SetSeed(p.seed);
  GetParam(sl::kOctave)->Set(std::clamp(p.octave, -3, 3));
  SendParameterValueFromDelegate(sl::kOctave,
                                 GetParam(sl::kOctave)->GetNormalized(), true);
  SetPresetStatus("");
}

void Seedlathe::PromptSavePreset()
{
  auto* ui = GetUI();
  if (!ui || !mPrompt) return;
  if (!mUserPresets.ready()) {
    SetPresetStatus(mUserPresets.error().empty()
                        ? "No writable preset folder"
                        : mUserPresets.error().c_str());
    return;
  }

  char suggested[64];
  if (!mSelectedUserPreset.empty())
    std::snprintf(suggested, sizeof(suggested), "%s", mSelectedUserPreset.c_str());
  else
    std::snprintf(suggested, sizeof(suggested), "%s %u", TypeName(P().edit.typeIndex),
                  P().seed);

  mPrompt->Prompt(GetUI()->GetBounds().GetCentredInside(320.f, 30.f), suggested,
                  [this](const char* text) {
                    sl::UserPreset p;
                    p.name = sl::sanitisePresetName(text);
                    p.seed = P().seed;
                    p.octave = GetParam(sl::kOctave)->Int();
                    p.edited = P().edited;
                    p.instrument = P().edit;

                    char buf[192];
                    if (mUserPresets.save(p)) {
                      mSelectedUserPreset = p.name;
                      std::snprintf(buf, sizeof(buf), "Saved \"%s\"", p.name.c_str());
                    } else {
                      std::snprintf(buf, sizeof(buf), "%s", mUserPresets.error().c_str());
                    }
                    RefreshPresetList();
                    SetPresetStatus(buf);
                  });
}

void Seedlathe::PromptRenamePreset()
{
  auto* ui = GetUI();
  if (!ui || !mPrompt) return;
  if (mSelectedUserPreset.empty()) {
    SetPresetStatus("Select one of your presets first");
    return;
  }

  const std::string from = mSelectedUserPreset;
  mPrompt->Prompt(GetUI()->GetBounds().GetCentredInside(320.f, 30.f), from.c_str(),
                  [this, from](const char* text) {
                    const std::string to = sl::sanitisePresetName(text);
                    char buf[192];
                    if (mUserPresets.rename(from, to)) {
                      mSelectedUserPreset = to;
                      std::snprintf(buf, sizeof(buf), "Renamed to \"%s\"", to.c_str());
                    } else {
                      std::snprintf(buf, sizeof(buf), "%s", mUserPresets.error().c_str());
                    }
                    RefreshPresetList();
                    SetPresetStatus(buf);
                  });
}

void Seedlathe::DeleteSelectedPreset()
{
  if (mSelectedUserPreset.empty()) {
    SetPresetStatus("Select one of your presets first");
    return;
  }

  char buf[192];
  if (mUserPresets.remove(mSelectedUserPreset))
    std::snprintf(buf, sizeof(buf), "Deleted \"%s\"", mSelectedUserPreset.c_str());
  else
    std::snprintf(buf, sizeof(buf), "%s", mUserPresets.error().c_str());

  mSelectedUserPreset.clear();
  RefreshPresetList();
  SetPresetStatus(buf);
}

#endif // IPLUG_EDITOR

void Seedlathe::BuildSettingsPage(IGraphics* g, const IRECT& page, const IVStyle& style)
{
  const IText body(12.f, IColor(255, 130, 140, 155), nullptr, EAlign::Near);
  const IText heading(13.f, kTextCol, nullptr, EAlign::Near);

  // Each setting gets a heading of its own here, so the controls must not draw
  // their built-in labels as well -- an empty label string falls back to the
  // parameter name, which is where the doubled "Oversampling" came from.
  const IVStyle bare = style.WithShowLabel(false);

  const auto note = [&](const IRECT& r, const char* text) {
    g->AttachControl(new ITextControl(r, text, body), kNoTag, "settings");
  };

  // Three columns: what the engine costs, how it listens, and what it is.
  const float colW = (page.W() - 24.f) / 3.f;
  const auto col = [&](int i) {
    const float l = page.L + (colW + 12.f) * float(i);
    return IRECT(l, page.T, l + colW, page.B);
  };

  // ---- engine -------------------------------------------------------------
  {
    const IRECT c = col(0);
    const IRECT p(c.L, c.T, c.R, c.B);
    g->AttachControl(new PanelControl(p, "ENGINE"), kNoTag, "settings");
    const IRECT in = p.GetHPadded(-12.f);

    g->AttachControl(new ITextControl(IRECT(in.L, p.T + 24.f, in.R, p.T + 40.f),
                                      "Oversampling", heading), kNoTag, "settings");
    g->AttachControl(new IVTabSwitchControl(
        IRECT(in.L, p.T + 42.f, in.L + 200.f, p.T + 68.f), sl::kOversample,
        {}, "", bare), kNoTag, "settings");
    note(IRECT(in.L, p.T + 72.f, in.R, p.T + 88.f),
         "Renders the engine at 2x or 4x the host rate.");
    note(IRECT(in.L, p.T + 88.f, in.R, p.T + 104.f),
         "It CHANGES the sound rather than only cleaning it");
    note(IRECT(in.L, p.T + 104.f, in.R, p.T + 120.f),
         "up: a seed is defined at the host rate, so band");
    note(IRECT(in.L, p.T + 120.f, in.R, p.T + 136.f),
         "limiting and filter timing both shift. Off is the");
    note(IRECT(in.L, p.T + 136.f, in.R, p.T + 152.f),
         "reference; 2x and 4x are a preference.");

    g->AttachControl(new ITextControl(IRECT(in.L, p.T + 164.f, in.R, p.T + 180.f),
                                      "Voices", heading), kNoTag, "settings");
    g->AttachControl(new IVSliderControl(
        IRECT(in.L, p.T + 182.f, in.R, p.T + 208.f), sl::kVoices, "", bare,
        false, EDirection::Horizontal), kNoTag, "settings");
    note(IRECT(in.L, p.T + 212.f, in.R, p.T + 228.f),
         "Polyphony ceiling. The quietest voice is stolen");
    note(IRECT(in.L, p.T + 228.f, in.R, p.T + 244.f),
         "when they run out.");
  }

  // ---- MIDI ---------------------------------------------------------------
  {
    const IRECT c = col(1);
    const IRECT p(c.L, c.T, c.R, c.B);
    g->AttachControl(new PanelControl(p, "MIDI"), kNoTag, "settings");
    const IRECT in = p.GetHPadded(-12.f);

    g->AttachControl(new ITextControl(IRECT(in.L, p.T + 24.f, in.R, p.T + 40.f),
                                      "Multitimbral", heading), kNoTag, "settings");
    g->AttachControl(new IVToggleControl(
        IRECT(in.L, p.T + 42.f, in.L + 130.f, p.T + 68.f), sl::kMultitimbral,
        "", bare, "Off", "On"), kNoTag, "settings");
    note(IRECT(in.L, p.T + 72.f, in.R, p.T + 88.f),
         "Off: every channel plays part 1, which is what a");
    note(IRECT(in.L, p.T + 88.f, in.R, p.T + 104.f),
         "host sending on channel 1 expects.");
    note(IRECT(in.L, p.T + 108.f, in.R, p.T + 124.f),
         "On: the channel selects the part, and the strip in");
    note(IRECT(in.L, p.T + 124.f, in.R, p.T + 140.f),
         "the header chooses which one you are editing.");
    note(IRECT(in.L, p.T + 144.f, in.R, p.T + 160.f),
         "Each part is a whole engine -- delay feedback and");
    note(IRECT(in.L, p.T + 160.f, in.R, p.T + 176.f),
         "reverb tails cannot be shared between instruments");
    note(IRECT(in.L, p.T + 176.f, in.R, p.T + 192.f),
         "-- so parts are allocated as you address them.");
    note(IRECT(in.L, p.T + 200.f, in.R, p.T + 216.f),
         "Pitch bend, sustain (CC 64) and program change are");
    note(IRECT(in.L, p.T + 216.f, in.R, p.T + 232.f),
         "handled per part.");
  }

  // ---- about --------------------------------------------------------------
  {
    const IRECT c = col(2);
    const IRECT p(c.L, c.T, c.R, c.B);
    g->AttachControl(new PanelControl(p, "ABOUT"), kNoTag, "settings");
    const IRECT in = p.GetHPadded(-12.f);

    g->AttachControl(new ITextControl(
        IRECT(in.L, p.T + 24.f, in.R, p.T + 48.f), PLUG_NAME,
        IText(20.f, kTextCol, nullptr, EAlign::Near)), kNoTag, "settings");
    note(IRECT(in.L, p.T + 50.f, in.R, p.T + 66.f), "Version " PLUG_VERSION_STR);
    note(IRECT(in.L, p.T + 76.f, in.R, p.T + 92.f),
         "A seed-driven procedural synthesizer. Every");
    note(IRECT(in.L, p.T + 92.f, in.R, p.T + 108.f),
         "instrument comes from one 32-bit integer, and the");
    note(IRECT(in.L, p.T + 108.f, in.R, p.T + 124.f),
         "same seed sounds the same here as it does in");
    note(IRECT(in.L, p.T + 124.f, in.R, p.T + 140.f),
         "zyn.js, the library it is a port of.");

    g->AttachControl(new IVButtonControl(
        IRECT(in.L, p.T + 154.f, in.L + 150.f, p.T + 180.f),
        Click([this] { if (auto* ui = GetUI()) ui->OpenURL(kSeedlatheUrl); }),
        "Seedlathe on GitHub", style), kNoTag, "settings");
    g->AttachControl(new IVButtonControl(
        IRECT(in.L, p.T + 186.f, in.L + 150.f, p.T + 212.f),
        Click([this] { if (auto* ui = GetUI()) ui->OpenURL(kZynUrl); }),
        "zyn.js on GitHub", style), kNoTag, "settings");
  }
}

void Seedlathe::BuildSamplePage(IGraphics* g, const IRECT& page, const IVStyle& style)
{
  g->AttachControl(new ITextControl(page.GetFromTop(52.f),
      "Load a sound and search seeds for the closest match. Every candidate is "
      "rendered and compared on\ntimbre, amplitude shape and brightness, so this "
      "runs at hundreds of seeds a second, not millions.",
      IText(12.f, kDim)), kNoTag, "sample");

  const IRECT loadRow = page.GetReducedFromTop(56.f).GetFromTop(34.f).GetFromLeft(560.f);
  g->AttachControl(new IVButtonControl(loadRow.GetFromLeft(180.f).GetPadded(-4.f),
      Click([this] { LoadSampleTarget(); }), "Load sample...", style),
      kNoTag, "sample");
  g->AttachControl(new ITextControl(loadRow.GetReducedFromLeft(190.f),
      "No sample loaded", IText(13.f, kTextCol, nullptr, EAlign::Near)),
      kCtrlTagSampleInfo, "sample");

  // Starting is idempotent while one is running, so a second press cannot
  // discard the candidates the first has already rendered.
  const auto startSample = [this](double threshold) {
    if (mSampleSearch.running()) return;
    mSampleSearch.start(GetParam(sl::kTypeFilter)->Int(), threshold);
    mAdoptSampleSearch = true;
  };

  const IRECT row = page.GetReducedFromTop(100.f).GetFromTop(36.f).GetFromLeft(560.f);
  g->AttachControl(new IVButtonControl(row.GetGridCell(0, 1, 3).GetPadded(-4.f),
      Click([startSample] { startSample(0.0); }), "Search", style), kNoTag, "sample");
  g->AttachControl(new IVButtonControl(row.GetGridCell(1, 1, 3).GetPadded(-4.f),
      // A lower bar than the parameter-space search: an unrelated recording
      // and a seed will never agree the way two seeds can.
      Click([startSample] { startSample(85.0); }), "Search until 85%", style),
      kNoTag, "sample");
  g->AttachControl(new IVButtonControl(row.GetGridCell(2, 1, 3).GetPadded(-4.f),
      Click([this] { CancelSearches(); }), "Stop", style), kNoTag, "sample");

  g->AttachControl(new ITextControl(page.GetReducedFromTop(144.f).GetFromTop(28.f),
      "Idle", IText(15.f, kTextCol)), kCtrlTagSampleStatus, "sample");

  g->AttachControl(new ITextControl(page.GetReducedFromTop(178.f).GetFromTop(18.f),
      "Roll type restricts which type is searched. Candidates are rendered at "
      "the sample's detected pitch.",
      IText(11.f, IColor(255, 110, 118, 130), nullptr, EAlign::Near)),
      kNoTag, "sample");

  // The runners-up, not just the winner.
  g->AttachControl(new ListControl(
      page.GetReducedFromTop(200.f).GetFromTop(260.f),
      [this](int payload) { SetSeed(static_cast<uint32_t>(payload)); }),
      kCtrlTagSampleResults, "sample");

  // ---- export ------------------------------------------------------------
  const IRECT exportTop = page.GetReducedFromTop(400.f);
  g->AttachControl(new ITextControl(exportTop.GetFromTop(24.f),
      "Export the current instrument as a 32-bit float stereo WAV.",
      IText(12.f, kDim)), kNoTag, "sample");
  g->AttachControl(new IVButtonControl(
      exportTop.GetReducedFromTop(28.f).GetFromTop(34.f).GetFromLeft(180.f).GetPadded(-4.f),
      Click([this] { ExportWav(); }), "Export WAV...", style), kNoTag, "sample");
  g->AttachControl(new ITextControl(
      exportTop.GetReducedFromTop(28.f).GetFromTop(34.f).GetReducedFromLeft(190.f),
      "", IText(13.f, kTextCol, nullptr, EAlign::Near)), kCtrlTagExportStatus, "sample");
}

void Seedlathe::LoadSampleTarget()
{
  auto* ui = GetUI();
  if (!ui) return;

  WDL_String file, dir;
  ui->PromptForFile(file, dir, EFileAction::Open, "wav");
  if (!file.GetLength()) return;

  // Reading and analysing a file happens on the message thread. It is bounded
  // -- one resample and one spectrogram over 1.5 s of audio -- and doing it on
  // a worker would only add a handshake for something the user is waiting on.
  const bool ok = mSampleSearch.loadTarget(file.Get());

  const char* slash = std::strrchr(file.Get(), '\\');
  if (!slash) slash = std::strrchr(file.Get(), '/');
  mSampleName = slash ? slash + 1 : file.Get();
  mSampleError = ok ? std::string() : mSampleSearch.error();
  RefreshSampleInfo();
}

void Seedlathe::ExportWav()
{
  auto* ui = GetUI();
  if (!ui) return;

  WDL_String file, dir;
  char suggested[64];
  std::snprintf(suggested, sizeof(suggested), "seedlathe-%u.wav", P().seed);
  file.Set(suggested);
  ui->PromptForFile(file, dir, EFileAction::Save, "wav");
  if (!file.GetLength()) return;

  // Rendered offline rather than captured from the audio thread, so the file
  // is the same every time and does not depend on what the host was doing.
  // Four seconds covers the longest generated reverb tail with room to spare.
  const sl::RenderResult r = sl::renderOffline(P().edit, GetParam(sl::kOctave)->Int() * 12,
                                               1.0, 4.0, GetSampleRate());
  const bool ok = sl::writeWav(file.Get(), r, GetSampleRate());

  if (auto* c = ui->GetControlWithTag(kCtrlTagExportStatus)) {
    char buf[160];
    if (ok) {
      const char* slash = std::strrchr(file.Get(), '\\');
      if (!slash) slash = std::strrchr(file.Get(), '/');
      std::snprintf(buf, sizeof(buf), "Wrote %s", slash ? slash + 1 : file.Get());
    } else {
      std::snprintf(buf, sizeof(buf), "Could not write that file");
    }
    c->As<ITextControl>()->SetStr(buf);
  }
}

void Seedlathe::RefreshResultList(int ctrlTag, const std::vector<sl::Candidate>& top)
{
#if IPLUG_EDITOR
  auto* ui = GetUI();
  if (!ui) return;
  auto* c = ui->GetControlWithTag(ctrlTag);
  if (!c) return;

  // Rebuilding twenty rows at UI rate costs nothing, but doing it when nothing
  // moved would reset the user's scroll position every frame, so the list is
  // only replaced when its contents actually changed.
  auto& cached = (ctrlTag == kCtrlTagResultList) ? mSearchTop : mSampleTop;
  if (cached.size() == top.size()) {
    bool same = true;
    for (size_t i = 0; i < top.size(); ++i)
      if (cached[i].seed != top[i].seed || cached[i].score != top[i].score) {
        same = false;
        break;
      }
    if (same) return;
  }
  cached = top;

  std::vector<ListControl::Row> rows;
  rows.reserve(top.size());
  for (const auto& e : top) {
    char label[48], detail[32];
    std::snprintf(label, sizeof(label), "%.2f%%", e.score);
    std::snprintf(detail, sizeof(detail), "%u", e.seed);
    rows.push_back({label, detail, false, static_cast<int>(e.seed)});
  }
  c->As<ListControl>()->SetRows(std::move(rows));
#endif
}

void Seedlathe::RefreshSampleInfo()
{
#if IPLUG_EDITOR
  auto* ui = GetUI();
  if (!ui) return;
  auto* c = ui->GetControlWithTag(kCtrlTagSampleInfo);
  if (!c) return;

  char buf[256];
  if (!mSampleError.empty())
    std::snprintf(buf, sizeof(buf), "%s  -  %s", mSampleName.c_str(), mSampleError.c_str());
  else if (mSampleSearch.hasTarget())
    std::snprintf(buf, sizeof(buf), "%s  -  root note %+d", mSampleName.c_str(),
                  mSampleSearch.target().rootNote);
  else
    std::snprintf(buf, sizeof(buf), "No sample loaded");

  c->As<ITextControl>()->SetStr(buf);
#endif
}

void Seedlathe::PushHistory()
{
  // Nothing to go back to before the part has a sound at all, and nothing
  // worth recording while GoBack is itself mid-flight.
  if (mRestoring) return;
  seedlathe::Part& part = P();
  if (!part.hasSeed) return;

  seedlathe::Snapshot s;
  s.seed = part.seed;
  s.inst = part.edit;
  s.edited = part.edited;
  s.octave = GetParam(sl::kOctave)->Int();
  part.pushHistory(s);
}

seedlathe::Snapshot Seedlathe::CurrentSnapshot() const
{
  const seedlathe::Part& part = P();
  seedlathe::Snapshot s;
  s.seed = part.seed;
  s.inst = part.edit;
  s.edited = part.edited;
  s.octave = GetParam(sl::kOctave)->Int();
  return s;
}

void Seedlathe::ApplySnapshot(const seedlathe::Snapshot& s)
{
  seedlathe::Part& part = P();
  mRestoring = true;

  // The seed parameters are the authoritative seed for part 1, so they have to
  // move with it or the host and the engine disagree about what is loaded.
  if (mEditPart == 0) {
    GetParam(sl::kSeedHi)->Set(sl::seedHi(s.seed));
    GetParam(sl::kSeedLo)->Set(sl::seedLo(s.seed));
    SendParameterValueFromDelegate(sl::kSeedHi, GetParam(sl::kSeedHi)->GetNormalized(), true);
    SendParameterValueFromDelegate(sl::kSeedLo, GetParam(sl::kSeedLo)->GetNormalized(), true);
  }

  // The instrument is restored wholesale rather than regenerated from the
  // seed: for an edited patch the seed is only half of what was there.
  part.seed = s.seed;
  part.hasSeed = true;
  part.edit = s.inst;
  part.edited = s.edited;
  part.designOsc = 0;
  part.pendingPublish = true;
  ServicePending(part);

  GetParam(sl::kOctave)->Set(std::clamp(s.octave, -3, 3));
  SendParameterValueFromDelegate(sl::kOctave,
                                 GetParam(sl::kOctave)->GetNormalized(), true);

  mRestoring = false;

  // Stepping through history is the user choosing a sound, so a search still
  // running must not overwrite it when it ends.
  mAdoptSearch = false;
  mAdoptSampleSearch = false;

  SyncDesigner();
  RefreshSeedDisplay();
}

void Seedlathe::GoBack()
{
  seedlathe::Snapshot restored;
  if (!P().stepBack(CurrentSnapshot(), restored)) return;
  ApplySnapshot(restored);
}

void Seedlathe::GoForward()
{
  seedlathe::Snapshot restored;
  if (!P().stepForward(CurrentSnapshot(), restored)) return;
  ApplySnapshot(restored);
}

void Seedlathe::StartSimilaritySearch(double threshold)
{
  mSearch.start(P().livePatch(), threshold);
  mAdoptSearch = true;
}

void Seedlathe::CancelSearches()
{
  mSearch.cancel();
  mSampleSearch.cancel();
  mAdoptSearch = false;
  mAdoptSampleSearch = false;
}

void Seedlathe::SetSeed(uint32_t seed)
{
  PushHistory();

  // The user has chosen a sound. A search that finishes after this must not
  // reach back and replace it -- which is exactly what rolling the dice
  // mid-search used to do a few seconds later.
  if (!mRestoring) {
    mAdoptSearch = false;
    mAdoptSampleSearch = false;
  }

  seedlathe::Part& part = P();
  if (mEditPart == 0) {
    // Only part 1's seed is a host parameter; see SeedlatheParams.h.
    GetParam(sl::kSeedHi)->Set(sl::seedHi(seed));
    GetParam(sl::kSeedLo)->Set(sl::seedLo(seed));
    SendParameterValueFromDelegate(sl::kSeedHi, GetParam(sl::kSeedHi)->GetNormalized(), true);
    SendParameterValueFromDelegate(sl::kSeedLo, GetParam(sl::kSeedLo)->GetNormalized(), true);
    RebuildInstrument();
  } else {
    part.seed = seed;
    // The seed did not come from a parameter, so nothing else will notice it
    // moved. Force the regeneration rather than let the equality check skip it.
    RebuildInstrument(true);
  }
  RefreshSeedDisplay();
}

void Seedlathe::RollRandomSeed()
{
  // Rolling the dice abandons the hunt. A search is a search for something
  // LIKE the current instrument, so the moment that instrument is thrown away
  // the search is answering a question nobody is asking any more -- and it
  // would go on burning a thread to do it.
  CancelSearches();

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

  const uint32_t seed = (mEditPart == 0)
                            ? sl::seedFrom(GetParam(sl::kSeedHi)->Int(),
                                           GetParam(sl::kSeedLo)->Int())
                            : P().seed;
  if (auto* c = ui->GetControlWithTag(kCtrlTagSeedBox))
    c->As<SeedBoxControl>()->SetSeed(seed);

  // Greyed rather than hidden: a Back button that vanishes when the history
  // empties moves everything beside it.
  if (auto* c = ui->GetControlWithTag(kCtrlTagBack))
    c->SetDisabled(!CanGoBack());
  if (auto* c = ui->GetControlWithTag(kCtrlTagNext))
    c->SetDisabled(!CanGoForward());

  if (auto* c = ui->GetControlWithTag(kCtrlTagPartStrip)) {
    auto* strip = c->As<PartStripControl>();
    strip->SetEnabled(mMulti);
    strip->SetSelected(mEditPart);
  }

  // The label describes the edit buffer, not what is currently sounding: the
  // two differ for as long as ServicePending is waiting for a free rack, and
  // the controls the user is looking at show the edit buffer.
  if (auto* c = ui->GetControlWithTag(kCtrlTagTypeLabel)) {
    char buf[160];
    char part[24] = "";
    if (mMulti) std::snprintf(part, sizeof(part), "Part %d  -  ", mEditPart + 1);
    std::snprintf(buf, sizeof(buf), "%s%s  -  %d oscillator%s%s%s",
                  part,
                  TypeName(P().edit.typeIndex), P().edit.oscCount,
                  P().edit.oscCount == 1 ? "" : "s",
                  P().edit.hasFmMatrix ? "  -  FM matrix" : "",
                  P().edited ? "  -  edited" : "");
    c->As<ITextControl>()->SetStr(buf);
  }

  // Highlight the preset the current seed corresponds to, however it was
  // reached. Matching on the seed rather than only on a click means rolling
  // onto a preset's seed shows you where you landed.
  if (auto* c = ui->GetControlWithTag(kCtrlTagPresetList)) {
    int payload = -1;
    const auto& user = mUserPresets.presets();
    for (size_t i = 0; i < user.size(); ++i)
      if (user[i].seed == P().seed && user[i].name == mSelectedUserPreset) {
        payload = kUserPresetBase + static_cast<int>(i);
        break;
      }
    if (payload < 0)
      for (size_t i = 0; i < user.size(); ++i)
        if (user[i].seed == P().seed) { payload = kUserPresetBase + static_cast<int>(i); break; }
    if (payload < 0)
      for (int i = 0; i < sl::kNumFactoryPresets; ++i)
        if (sl::kFactoryPresets[i].seed == P().seed) { payload = i; break; }
    c->As<ListControl>()->SetSelected(payload);
  }

  if (auto* c = ui->GetControlWithTag(kCtrlTagOscCount)) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%d of %d", P().designOsc + 1, P().edit.oscCount);
    c->As<ITextControl>()->SetStr(buf);
  }
#endif
}

void Seedlathe::RebuildInstrument(bool force)
{
  // iPlug2 fires OnParamChange while the plugin is still being constructed,
  // before OnReset has run, so there is a window where the rack has no pools
  // and no sample rate. Nothing to rebuild against yet; OnReset does it.
  if (!mPrepared)
    return;

  seedlathe::Part& part = P();
  if (!part.allocated)
    return;

  const uint32_t seed =
      (mEditPart == 0) ? sl::seedFrom(GetParam(sl::kSeedHi)->Int(),
                                      GetParam(sl::kSeedLo)->Int())
                       : part.seed;

  // `force` is Revert to seed, and is also how a part whose seed is not a
  // parameter gets rebuilt at all: for those the equality check is always true.
  if (part.hasSeed && seed == part.seed && !force)
    return;

  part.edit = sl::generateInstrument(seed);
  part.edited = false;
  part.seed = seed;
  part.hasSeed = true;
  part.designOsc = 0;
  part.pendingPublish = true;
  ServicePending(part);
  SyncDesigner();
}

void Seedlathe::EnsurePart(int index)
{
  if (index < 0 || index >= sl::kNumParts) return;
  seedlathe::Part& part = mParts[static_cast<size_t>(index)];
  part.requested = true;
  if (part.allocated || !mPrepared) return;

  // Allocating a part is the same class of operation as changing the
  // oversampling factor: it hands the audio thread new buffers to walk. Same
  // handshake, for the same reason.
  if (!Quiesce([this, &part] {
        part.prepare(GetSampleRate() * mOsFactor, mMulti,
                     GetParam(sl::kVoices)->Int());
      })) {
    // Still requested, so OnIdle comes back to it.
    return;
  }

  if (!part.hasSeed) {
    // A fresh part starts on the same seed as part 1 rather than on silence,
    // so selecting it produces a sound to work from.
    part.seed = mParts[0].seed;
    part.edit = sl::generateInstrument(part.seed);
    part.hasSeed = true;
  }
  part.pendingPublish = true;
  ServicePending(part);
}

void Seedlathe::SelectPart(int index)
{
  if (index < 0 || index >= sl::kNumParts || index == mEditPart) return;
  EnsurePart(index);
  mEditPart = index;
  SyncDesigner();
  RefreshSeedDisplay();
}

void Seedlathe::OnReset()
{
  mOsFactor = 1 << GetParam(sl::kOversample)->Int();
  mMulti = GetParam(sl::kMultitimbral)->Bool();
  PrepareEngine();
  mPrepared = true;

  // A restored session may name parts the engine had no sample rate for when
  // the chunk arrived. Now it has one.
  for (int i = 1; i < sl::kNumParts; ++i)
    if (mParts[static_cast<size_t>(i)].requested)
      EnsurePart(i);

  // A restored patch must survive this. OnReset also runs after
  // UnserializeState, and regenerating from the seed there would silently
  // discard every designer edit the host just handed back.
  for (int i = 0; i < sl::kNumParts; ++i) {
    seedlathe::Part& part = mParts[static_cast<size_t>(i)];
    if (!part.allocated) continue;
    if (part.edited || i != 0) {
      part.pendingPublish = true;
      ServicePending(part);
    }
  }

  if (!mParts[0].edited) {
    mParts[0].hasSeed = false;
    const int was = mEditPart;
    mEditPart = 0;
    RebuildInstrument();
    mEditPart = was;
  }
  SyncDesigner();
}

bool Seedlathe::SerializeState(IByteChunk& chunk) const
{
  // Header first, parameters second. See kStateMagic for why the order is
  // load-bearing rather than tidy.
  const uint32_t magic = kStateMagic;
  const int version = kStateVersion;
  chunk.Put(&magic);
  chunk.Put(&version);

  if (!SerializeParams(chunk))
    return false;

  chunk.Put(&mEditPart);

  for (const auto& part : mParts) {
    const int used = part.requested ? 1 : 0;
    chunk.Put(&used);
    if (!used) continue;
    chunk.Put(&part.seed);
    // Empty for an unedited patch: the seed alone reproduces it, and storing
    // the generated instrument as well would mean two sources of truth that a
    // future generator change could put out of step.
    const std::string json =
        part.edited ? sl::instrumentToJson(part.edit).dump() : std::string();
    chunk.PutStr(json.c_str());
  }
  return true;
}

int Seedlathe::UnserializeState(const IByteChunk& chunk, int startPos)
{
  // Refuse before touching a single parameter. A chunk that fails this is
  // from a build whose layout differed, and reading it would not produce an
  // approximation of the saved session -- it would produce a different one,
  // silently, with plausible-looking values.
  uint32_t magic = 0;
  int version = 0;
  int pos = chunk.Get(&magic, startPos);
  if (pos < 0 || magic != kStateMagic) return startPos;
  pos = chunk.Get(&version, pos);
  if (pos < 0 || version != kStateVersion) return startPos;

  // This regenerates part 1 from the restored seed as a side effect of
  // OnParamChange, which is exactly the state an unedited patch wants.
  pos = UnserializeParams(chunk, pos);
  if (pos < 0) return startPos;

  int editPart = 0;
  pos = chunk.Get(&editPart, pos);
  if (pos < 0) return startPos;

  int next = 0;
  for (int i = 0; i < sl::kNumParts; ++i) {
    int used = 0;
    next = chunk.Get(&used, pos);
    if (next < 0) return pos;
    pos = next;
    if (!used) continue;

    seedlathe::Part& part = mParts[static_cast<size_t>(i)];
    uint32_t seed = 0;
    pos = chunk.Get(&seed, pos);
    WDL_String json;
    next = chunk.GetStr(json, pos);
    if (next < 0) return pos;
    pos = next;

    part.requested = true;
    part.seed = seed;
    part.hasSeed = true;
    part.edited = false;
    part.edit = sl::generateInstrument(seed);

    if (json.GetLength() > 0) {
      try {
        part.edit = sl::instrumentFromJson(nlohmann::json::parse(json.Get()));
        part.edited = true;
      } catch (const std::exception&) {
        // Keep the seed's instrument rather than load a broken one.
      }
    }
    part.designOsc = 0;
    part.pendingPublish = true;
    EnsurePart(i);            // no-op until OnReset has given a sample rate
    ServicePending(part);
  }

  mEditPart = std::clamp(editPart, 0, sl::kNumParts - 1);
  SyncDesigner();
  return pos;
}

void Seedlathe::OnParamChange(int paramIdx)
{
  if (paramIdx == sl::kSeedHi || paramIdx == sl::kSeedLo) {
    RebuildInstrument();
    RefreshSeedDisplay();
  } else if (paramIdx == sl::kOversample || paramIdx == sl::kMultitimbral ||
             paramIdx == sl::kVoices) {
    Reconfigure();
  }
}

void Seedlathe::OnIdle()
{
  mScopeSender.TransmitData(*this);

#ifdef APP_API
  // Every couple of seconds rather than only on exit. The destructor is not
  // reached if the app is force-killed or crashes, and losing a session's work
  // to that is exactly the failure this exists to prevent. OnIdle runs at
  // PLUG_FPS, so this is roughly a two-second debounce.
  if (++mIdleTicksSinceSave >= PLUG_FPS * 2) {
    mIdleTicksSinceSave = 0;
    SaveStandaloneState();
  }
#endif

  // Anything the quiescence handshake refused, because the audio thread was
  // still inside a block when it was asked.
  if (mPendingReconfigure) Reconfigure();
  for (int i = 1; i < sl::kNumParts; ++i) {
    seedlathe::Part& part = mParts[static_cast<size_t>(i)];
    if (part.requested && !part.allocated) EnsurePart(i);
  }

  // A program change the audio thread could not act on itself.
  ServicePendingPrograms();

  // A rebuild refused because every rack was still sounding. Retry now that
  // some of them have had time to fall silent.
  ServiceAllPending();

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

  // Adopt the winner once, when the search finishes -- and only if the user
  // has not picked a sound of their own since it started.
  if (mSearchWasRunning && !running && best.found && mAdoptSearch)
    SetSeed(best.seed);
  if (!running) mAdoptSearch = false;

  RefreshResultList(kCtrlTagResultList, mSearch.top());

  mSearchWasRunning = running;

  // ---- sample match ------------------------------------------------------
  const bool sampleRunning = mSampleSearch.running();
  const auto sampleBest = mSampleSearch.best();

  if (sampleRunning || mSampleSearchWasRunning) {
    if (auto* c = ui->GetControlWithTag(kCtrlTagSampleStatus)) {
      char buf[192];
      if (sampleBest.found)
        std::snprintf(buf, sizeof(buf), "%s  -  best %.1f%%  seed %u  (%llu rendered)",
                      sampleRunning ? "Searching" : "Done", sampleBest.score,
                      sampleBest.seed,
                      static_cast<unsigned long long>(sampleBest.tested));
      else
        std::snprintf(buf, sizeof(buf), "%s...", sampleRunning ? "Searching" : "Idle");
      c->As<ITextControl>()->SetStr(buf);
    }
  }

  if (mSampleSearchWasRunning && !sampleRunning && sampleBest.found &&
      mAdoptSampleSearch)
    SetSeed(sampleBest.seed);
  if (!sampleRunning) mAdoptSampleSearch = false;

  RefreshResultList(kCtrlTagSampleResults, mSampleSearch.top());

  mSampleSearchWasRunning = sampleRunning;
#endif
}

void Seedlathe::ProcessMidiMsg(const IMidiMsg& msg)
{
  // In single mode every channel drives part 1, which is what a host sending
  // on channel 1 expects and what omni mode means. In multitimbral mode the
  // channel selects the part, and a channel whose part has never been
  // allocated is silent -- allocating one here would mean the audio thread
  // asking for sixty megabytes of buffer mid-block.
  const int channel = msg.Channel();
  const int index = mMulti ? std::clamp(channel, 0, sl::kNumParts - 1) : 0;
  seedlathe::Part& part = mParts[static_cast<size_t>(index)];
  if (!part.allocated)
    return;

  const int octave = GetParam(sl::kOctave)->Int();
  const auto status = msg.StatusMsg();

  if (status == IMidiMsg::kNoteOn && msg.Velocity() > 0)
  {
    // zyn's gain: 0.5 * volume * velocity, with 0.5 applied inside the voice.
    const double gain = (msg.Velocity() / 127.0) * (GetParam(sl::kVolume)->Value() / 100.0);
    const int note = msg.NoteNumber() - kMidiMiddleC + octave * 12;
    part.pool.noteOn(part.racks.liveRack(), part.livePatch(), note, gain, true);
  }
  else if (status == IMidiMsg::kNoteOff ||
           (status == IMidiMsg::kNoteOn && msg.Velocity() == 0))
  {
    part.pool.noteOff(msg.NoteNumber() - kMidiMiddleC + octave * 12);
  }
  else if (status == IMidiMsg::kProgramChange)
  {
    // Recorded, not acted on: loading a preset moves the seed and rebuilds
    // racks, and neither belongs on the audio thread.
    const int program = msg.Program();
    if (program >= 0 && program < sl::kNumFactoryPresets)
      mPendingProgram[static_cast<size_t>(index)].store(program,
                                                        std::memory_order_release);
  }
  else if (status == IMidiMsg::kPitchWheel)
  {
    // PitchWheel() is -1..1. Two semitones each way is the MIDI default and
    // what a controller assumes when nothing has told it otherwise.
    part.pool.setPitchBend(msg.PitchWheel() * 2.0);
  }
  else if (status == IMidiMsg::kControlChange)
  {
    switch (msg.ControlChangeIdx())
    {
      case IMidiMsg::kAllNotesOff:
        part.pool.allNotesOff();
        break;
      case IMidiMsg::kSustainOnOff:
        // The MIDI convention: 64 and above is down. Some pedals only ever
        // send 0 and 127, others sweep, and treating anything non-zero as down
        // makes a half-pedal position stick.
        part.pool.setSustainPedal(msg.ControlChange(IMidiMsg::kSustainOnOff) >= 0.5);
        break;
      default:
        break;
    }
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

  // Half of the handshake lives here; the ordering inside enter() is the other
  // half. See core/src/Quiescer.h.
  const bool bail = !mQuiescer.enter() ||
                    static_cast<int>(mLeft.size()) < nFrames * mOsFactor;

  if (bail)
  {
    // Either the engine is being rebuilt, or the host handed over more frames
    // than PrepareEngine sized for. Silence beats allocating here.
    for (int c = 0; c < nChans; ++c)
      for (int s = 0; s < nFrames; ++s) outputs[c][s] = 0.;
    mQuiescer.leave();
    _mm_setcsr(mxcsr);
    return;
  }

  // At 2x and 4x the engine is prepared at that multiple of the host rate, so
  // it renders that many frames and the decimator brings them back down.
  const int engineFrames = nFrames * mOsFactor;

  for (int s = 0; s < engineFrames; ++s) { mLeft[s] = 0.f; mRight[s] = 0.f; }

  // Parts sum into one bus, which is what a multitimbral instrument on a single
  // stereo output means. Silent parts are skipped entirely rather than rendered
  // and added as zero: with sixteen parts allocated that is most of them.
  // Read once per block rather than from OnParamChange: automation arrives on
  // this thread anyway, and a block boundary is exactly the granularity the
  // voices smooth from. Applied to every allocated part, sounding or not --
  // a part that starts a note later this block must already be in step.
  const double modCutoff = GetParam(sl::kFilterCutoff)->Value();
  const double modRes = GetParam(sl::kFilterRes)->Value();
  for (auto& part : mParts)
    if (part.allocated) part.pool.setFilterMod(modCutoff, modRes);

  for (auto& part : mParts)
  {
    if (!part.allocated || !part.sounding()) continue;

    part.racks.audioBlockStarted(part.pool);
    part.pool.render(mPartL.data(), mPartR.data(), engineFrames,
                     part.racks.all(), part.racks.allCount());
    for (int s = 0; s < engineFrames; ++s)
    {
      mLeft[s] += mPartL[s];
      mRight[s] += mPartR[s];
    }
  }

  // The compressor runs at the engine rate, before decimation, and across the
  // summed parts: it is zyn's single master compressor, not a per-part one.
  for (int s = 0; s < engineFrames; ++s)
  {
    double l = 0.0, r = 0.0;
    mComp.process(mLeft[s], mRight[s], l, r);   // Z.masterGain is unity
    mLeft[s] = static_cast<float>(l);
    mRight[s] = static_cast<float>(r);
  }

  if (mOsFactor == 1)
  {
    for (int s = 0; s < nFrames; ++s)
    {
      if (nChans > 0) outputs[0][s] = mLeft[s];
      if (nChans > 1) outputs[1][s] = mRight[s];
    }
  }
  else
  {
    mDecimL.process(mLeft.data(), mDownL.data(), nFrames);
    mDecimR.process(mRight.data(), mDownR.data(), nFrames);
    for (int s = 0; s < nFrames; ++s)
    {
      if (nChans > 0) outputs[0][s] = mDownL[s];
      if (nChans > 1) outputs[1][s] = mDownR[s];
    }
  }

  mScopeSender.ProcessBlock(outputs, nFrames, kCtrlTagScope, 1);

  mQuiescer.leave();
  _mm_setcsr(mxcsr);
}

bool Seedlathe::Quiesce(const std::function<void()>& work)
{
  return mQuiescer.withQuiescence(work);
}

void Seedlathe::Reconfigure()
{
  if (!mPrepared)
    return;

  const int wantedOs = 1 << GetParam(sl::kOversample)->Int();
  const bool wantedMulti = GetParam(sl::kMultitimbral)->Bool();
  const int wantedVoices = GetParam(sl::kVoices)->Int();
  if (wantedOs == mOsFactor && wantedMulti == mMulti &&
      wantedVoices == mPreparedVoices) {
    mPendingReconfigure = false;
    return;
  }

  // The new settings are committed INSIDE the handshake, not before it. They
  // decide how large the buffers must be, so a refusal after committing them
  // would leave ProcessBlock comparing the new size against the old buffers
  // and bailing to silence on every block, permanently.
  const bool done = Quiesce([this, wantedOs, wantedMulti] {
    mOsFactor = wantedOs;
    mMulti = wantedMulti;
    PrepareEngine();
  });

  if (!done) {
    // The audio thread did not leave its block in time. Try again on the next
    // idle tick rather than reallocating underneath it.
    mPendingReconfigure = true;
    return;
  }
  mPendingReconfigure = false;

  // Every rack was thrown away, so every allocated part needs republishing.
  for (auto& part : mParts)
  {
    if (!part.allocated) continue;
    part.pendingPublish = true;
    ServicePending(part);
  }

  if (!mMulti && mEditPart != 0)
    SelectPart(0);

  // The part strip is enabled by multitimbral mode, so it has to be told when
  // that changed; nothing else on screen reads the flag.
  RefreshSeedDisplay();
}

void Seedlathe::PrepareEngine()
{
  // Everything downstream of here is sized and tuned for the rate the engine
  // runs at, which is the host rate times the oversampling factor.
  const double rate = GetSampleRate() * mOsFactor;
  mPreparedVoices = GetParam(sl::kVoices)->Int();

  // Part 1 always exists. The rest keep whatever allocation state they had:
  // re-preparing an allocated part is required (its buffers are stale at the
  // new rate), allocating an untouched one is not.
  mParts[0].prepare(rate, mMulti, mPreparedVoices);
  for (int i = 1; i < sl::kNumParts; ++i)
    if (mParts[static_cast<size_t>(i)].allocated)
      mParts[static_cast<size_t>(i)].prepare(rate, mMulti, mPreparedVoices);

  mComp.prepare(rate);
  mComp.setParams(-12.0, 6.0, 8.0, 0.003, 0.15);   // zyn's Z.init settings

  mDecimL.prepare(mOsFactor);
  mDecimR.prepare(mOsFactor);

  // Generous headroom: a host may hand ProcessBlock more frames than the
  // reported block size, and growing the buffer there would allocate.
  const size_t hostCap = std::max<size_t>(static_cast<size_t>(GetBlockSize()), 1024) * 4;
  const size_t engineCap = hostCap * static_cast<size_t>(mOsFactor);
  mLeft.assign(engineCap, 0.f);
  mRight.assign(engineCap, 0.f);
  mPartL.assign(engineCap, 0.f);
  mPartR.assign(engineCap, 0.f);
  mDownL.assign(hostCap, 0.f);
  mDownR.assign(hostCap, 0.f);

  SetLatency(mDecimL.latencySamples());
}

#endif
