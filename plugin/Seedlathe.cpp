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
        tabBar, {"Instrument", "Presets", "Search", "Sample", "Design"},
        [g](int index) {
          static const char* kGroups[] = {"instrument", "presets", "search", "sample", "design"};
          for (int i = 0; i < 5; ++i)
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
    {
      const IRECT bar = page.GetFromTop(30.f);
      g->AttachControl(new IVButtonControl(bar.GetFromLeft(150.f).GetPadded(-3.f),
          [this](IControl*) { PromptSavePreset(); }, "Save current...", style),
          kNoTag, "presets");
      g->AttachControl(new IVButtonControl(
          bar.GetReducedFromLeft(150.f).GetFromLeft(110.f).GetPadded(-3.f),
          [this](IControl*) { PromptRenamePreset(); }, "Rename...", style),
          kNoTag, "presets");
      g->AttachControl(new IVButtonControl(
          bar.GetReducedFromLeft(260.f).GetFromLeft(110.f).GetPadded(-3.f),
          [this](IControl*) { DeleteSelectedPreset(); }, "Delete", style),
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

    // -- Sample match and WAV export
    BuildSamplePage(g, page, style);

    // -- Design
    BuildDesigner(g, page, style);

    // Everything but the first page starts hidden.
    for (const char* grp : {"presets", "search", "sample", "design"})
      g->ForControlInGroup(grp, [](IControl* c) { c->Hide(true); });

    // ---- keyboard ------------------------------------------------------
    g->AttachControl(new IVKeyboardControl(keys), kCtrlTagKeyboard);

    RefreshPresetList();
    RefreshSeedDisplay();
  };
#endif
}

#if IPLUG_DSP

#if IPLUG_EDITOR
namespace {
const char* const kWaveNames[]   = {"Sin", "Sqr", "Saw", "Tri", "Nse"};
const char* const kFilterNames[] = {"LP", "HP", "BP", "LS", "HS", "PK", "AP"};
const char* const kOverNames[]   = {"1x", "2x", "4x"};

int overIndex(int factor) { return factor >= 4 ? 2 : (factor >= 2 ? 1 : 0); }
int overFactor(int index) { return index == 2 ? 4 : (index == 1 ? 2 : 1); }
} // namespace

sl::Osc& Seedlathe::EditOsc()
{
  return mEdit.oscs[static_cast<size_t>(std::clamp(mDesignOsc, 0, sl::kMaxOscs - 1))];
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
      [this]() { return mEdit.oscCount; },
      [this](int i) { mDesignOsc = i; SyncDesigner(); });
  g->AttachControl(oscSel, kCtrlTagOscSelect, "design");
  mDesignerControls.push_back(oscSel);

  const IRECT countRow = selRow.GetReducedFromLeft(412.f).GetFromLeft(200.f);
  g->AttachControl(new IVButtonControl(countRow.GetFromLeft(30.f),
      [this](IControl*) { SetOscCount(mEdit.oscCount - 1); }, "-", style), kNoTag, "design");
  g->AttachControl(new ITextControl(countRow.GetReducedFromLeft(34.f).GetFromLeft(130.f), "",
      IText(11.f, IColor(255, 214, 221, 230))), kCtrlTagOscCount, "design");
  g->AttachControl(new IVButtonControl(countRow.GetFromRight(30.f),
      [this](IControl*) { SetOscCount(mEdit.oscCount + 1); }, "+", style), kNoTag, "design");

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
       [this]() { return mEdit.hasFmMatrix; }, [this](bool b) { mEdit.hasFmMatrix = b; });
    track(new FmMatrixControl(
        rowIn(p, 40.f, 246.f),
        [this](int src, int tgt) {
          return mEdit.fmMatrix[static_cast<size_t>(src)][static_cast<size_t>(tgt)];
        },
        [this](int src, int tgt, double v) {
          mEdit.fmMatrix[static_cast<size_t>(src)][static_cast<size_t>(tgt)] = v;
        },
        [this]() { return mEdit.oscCount; },
        push));

    const IRECT q(c.L, c.T + 300.f, c.R, c.T + 392.f);
    panel(q, "PATCH");
    g->AttachControl(new IVButtonControl(rowIn(q, 18.f, 22.f),
        [this](IControl*) {
          if (auto* ui = GetUI())
            ui->SetTextInClipboard(sl::instrumentToJson(mEdit).dump(2).c_str());
        }, "Copy JSON", style), kNoTag, "design");
    g->AttachControl(new IVButtonControl(rowIn(q, 44.f, 22.f),
        [this](IControl*) {
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
          mEdit = parsed;
          mDesignOsc = 0;
          PushEdit();
          SyncDesigner();
        }, "Paste JSON", style), kNoTag, "design");
    g->AttachControl(new IVButtonControl(rowIn(q, 70.f, 22.f),
        [this](IControl*) { RebuildInstrument(true); }, "Revert to seed", style),
        kNoTag, "design");
  }
}
#endif // IPLUG_EDITOR

void Seedlathe::SetOscCount(int n)
{
  n = std::clamp(n, 1, sl::kMaxOscs);
  if (n == mEdit.oscCount) return;

  // A new slot is a copy of the last one, not a default Osc. A default Osc has
  // an all-zero gain envelope, so adding an oscillator would appear to do
  // nothing at all until the user rebuilt its envelope by hand.
  for (int i = mEdit.oscCount; i < n; ++i)
    mEdit.oscs[static_cast<size_t>(i)] =
        mEdit.oscs[static_cast<size_t>(std::max(0, mEdit.oscCount - 1))];

  mEdit.oscCount = n;
  if (mDesignOsc >= n) mDesignOsc = n - 1;
  PushEdit();
  SyncDesigner();
}

void Seedlathe::PushEdit()
{
  mEdited = true;
  mPendingPublish = true;
  ServicePending();
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

void Seedlathe::ServicePending()
{
  if (!mPendingPublish || !mPrepared)
    return;

  const int live = mLive.load(std::memory_order_relaxed);

  // Dragging an envelope handle emits an edit per mouse move. Rebuilding a rack
  // for each one would regenerate every reverb impulse -- an FFT per drag frame
  // -- and would be refused whenever the racks were busy, so the edit would not
  // be heard until the note ended. Only delay and reverb changes need the rack.
  if (mRacksBuilt && sameFxConfig(mEdit, mInstruments[static_cast<size_t>(live)])) {
    mInstruments[static_cast<size_t>(1 - live)] = mEdit;
    mLive.store(1 - live, std::memory_order_release);
    mPendingPublish = false;
    return;
  }

  // Impulse generation and its FFTs happen here, on the message thread, and
  // RackPool guarantees the rack it builds into is unreachable from audio.
  if (!mRacks.rebuild(mEdit, mPool))
    return;

  mInstruments[static_cast<size_t>(1 - live)] = mEdit;
  mLive.store(1 - live, std::memory_order_release);
  mRacksBuilt = true;
  mPendingPublish = false;
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
    // The seed load has already regenerated mEdit, so the stored instrument
    // goes on top of it -- and only when the preset actually holds one.
    if (p.edited) {
      mEdit = p.instrument;
      mDesignOsc = 0;
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
    std::snprintf(suggested, sizeof(suggested), "%s %u", TypeName(mEdit.typeIndex),
                  mCurrentSeed);

  mPrompt->Prompt(GetUI()->GetBounds().GetCentredInside(320.f, 30.f), suggested,
                  [this](const char* text) {
                    sl::UserPreset p;
                    p.name = sl::sanitisePresetName(text);
                    p.seed = mCurrentSeed;
                    p.octave = GetParam(sl::kOctave)->Int();
                    p.edited = mEdited;
                    p.instrument = mEdit;

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

void Seedlathe::BuildSamplePage(IGraphics* g, const IRECT& page, const IVStyle& style)
{
  g->AttachControl(new ITextControl(page.GetFromTop(52.f),
      "Load a sound and search seeds for the closest match. Every candidate is "
      "rendered and compared on\ntimbre, amplitude shape and brightness, so this "
      "runs at hundreds of seeds a second, not millions.",
      IText(12.f, kDim)), kNoTag, "sample");

  const IRECT loadRow = page.GetReducedFromTop(56.f).GetFromTop(34.f).GetFromLeft(560.f);
  g->AttachControl(new IVButtonControl(loadRow.GetFromLeft(180.f).GetPadded(-4.f),
      [this](IControl*) { LoadSampleTarget(); }, "Load sample...", style),
      kNoTag, "sample");
  g->AttachControl(new ITextControl(loadRow.GetReducedFromLeft(190.f),
      "No sample loaded", IText(13.f, kTextCol, nullptr, EAlign::Near)),
      kCtrlTagSampleInfo, "sample");

  const IRECT row = page.GetReducedFromTop(100.f).GetFromTop(36.f).GetFromLeft(560.f);
  g->AttachControl(new IVButtonControl(row.GetGridCell(0, 1, 3).GetPadded(-4.f),
      [this](IControl*) {
        mSampleSearch.start(GetParam(sl::kTypeFilter)->Int(), 0.0);
      }, "Search", style), kNoTag, "sample");
  g->AttachControl(new IVButtonControl(row.GetGridCell(1, 1, 3).GetPadded(-4.f),
      [this](IControl*) {
        // A lower bar than the parameter-space search: an unrelated recording
        // and a seed will never agree the way two seeds can.
        mSampleSearch.start(GetParam(sl::kTypeFilter)->Int(), 85.0);
      }, "Search until 85%", style), kNoTag, "sample");
  g->AttachControl(new IVButtonControl(row.GetGridCell(2, 1, 3).GetPadded(-4.f),
      [this](IControl*) { mSampleSearch.cancel(); }, "Stop", style), kNoTag, "sample");

  g->AttachControl(new ITextControl(page.GetReducedFromTop(144.f).GetFromTop(28.f),
      "Idle", IText(15.f, kTextCol)), kCtrlTagSampleStatus, "sample");

  g->AttachControl(new ITextControl(page.GetReducedFromTop(180.f).GetFromTop(40.f),
      "The Roll type knob restricts which instrument type is searched.\n"
      "Candidates are rendered at the sample's detected pitch.",
      IText(12.f, IColor(255, 110, 118, 130))), kNoTag, "sample");

  // ---- export ------------------------------------------------------------
  const IRECT exportTop = page.GetReducedFromTop(240.f);
  g->AttachControl(new ITextControl(exportTop.GetFromTop(24.f),
      "Export the current instrument as a 32-bit float stereo WAV.",
      IText(12.f, kDim)), kNoTag, "sample");
  g->AttachControl(new IVButtonControl(
      exportTop.GetReducedFromTop(28.f).GetFromTop(34.f).GetFromLeft(180.f).GetPadded(-4.f),
      [this](IControl*) { ExportWav(); }, "Export WAV...", style), kNoTag, "sample");
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
  std::snprintf(suggested, sizeof(suggested), "seedlathe-%u.wav", mCurrentSeed);
  file.Set(suggested);
  ui->PromptForFile(file, dir, EFileAction::Save, "wav");
  if (!file.GetLength()) return;

  // Rendered offline rather than captured from the audio thread, so the file
  // is the same every time and does not depend on what the host was doing.
  // Four seconds covers the longest generated reverb tail with room to spare.
  const sl::RenderResult r = sl::renderOffline(mEdit, GetParam(sl::kOctave)->Int() * 12,
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

  // The label describes the edit buffer, not what is currently sounding: the
  // two differ for as long as ServicePending is waiting for a free rack, and
  // the controls the user is looking at show the edit buffer.
  if (auto* c = ui->GetControlWithTag(kCtrlTagTypeLabel)) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s  -  %d oscillator%s%s%s",
                  TypeName(mEdit.typeIndex), mEdit.oscCount,
                  mEdit.oscCount == 1 ? "" : "s",
                  mEdit.hasFmMatrix ? "  -  FM matrix" : "",
                  mEdited ? "  -  edited" : "");
    c->As<ITextControl>()->SetStr(buf);
  }

  if (auto* c = ui->GetControlWithTag(kCtrlTagOscCount)) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%d of %d", mDesignOsc + 1, mEdit.oscCount);
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

  const auto seed = sl::seedFrom(GetParam(sl::kSeedHi)->Int(),
                                 GetParam(sl::kSeedLo)->Int());
  // `force` is Revert to seed: the seed has not moved, but the edit buffer has
  // to be thrown away and regenerated from it.
  if (mHasSeed && seed == mCurrentSeed && !force)
    return;

  mEdit = sl::generateInstrument(seed);
  mEdited = false;
  mCurrentSeed = seed;
  mHasSeed = true;
  mDesignOsc = 0;
  mPendingPublish = true;
  ServicePending();
  SyncDesigner();
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
  // prepare() reallocated every rack at the new sample rate, so none of them
  // holds an impulse for the current patch any more.
  mRacksBuilt = false;

  // A restored patch must survive this. OnReset also runs after
  // UnserializeState, and regenerating from the seed there would silently
  // discard every designer edit the host just handed back.
  if (mEdited) {
    mPendingPublish = true;
    ServicePending();
    SyncDesigner();
  } else {
    mHasSeed = false;
    RebuildInstrument();
  }
}

bool Seedlathe::SerializeState(IByteChunk& chunk) const
{
  if (!SerializeParams(chunk))
    return false;

  // Empty for an unedited patch: the seed alone reproduces it, and storing the
  // generated instrument as well would mean two sources of truth that a future
  // generator change could put out of step.
  const std::string json = mEdited ? sl::instrumentToJson(mEdit).dump() : std::string();
  return chunk.PutStr(json.c_str()) > 0;
}

int Seedlathe::UnserializeState(const IByteChunk& chunk, int startPos)
{
  // This regenerates mEdit from the restored seed as a side effect of
  // OnParamChange, which is exactly the state an unedited patch wants.
  int pos = UnserializeParams(chunk, startPos);

  WDL_String json;
  const int next = chunk.GetStr(json, pos);
  if (next < 0)
    return pos;   // state written before the designer existed
  pos = next;

  if (json.GetLength() == 0) {
    mEdited = false;
    return pos;
  }

  sl::Instrument parsed;
  try {
    parsed = sl::instrumentFromJson(nlohmann::json::parse(json.Get()));
  } catch (const std::exception&) {
    return pos;   // keep the seed's instrument rather than load a broken one
  }

  mEdit = parsed;
  mEdited = true;
  mDesignOsc = 0;
  mPendingPublish = true;
  ServicePending();
  SyncDesigner();
  return pos;
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

  // A rebuild refused because every rack was still sounding. Retry now that
  // some of them have had time to fall silent.
  ServicePending();

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

  if (mSampleSearchWasRunning && !sampleRunning && sampleBest.found)
    SetSeed(sampleBest.seed);

  mSampleSearchWasRunning = sampleRunning;
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
