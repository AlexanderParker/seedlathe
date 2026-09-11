#pragma once

#include "IControl.h"
#include "IControls.h"
#include "SeedlatheParams.h"
#include "UserPresets.h"
#include "sl/FactoryPresets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace iplug;
using namespace igraphics;

namespace seedlathe {

inline const char* TypeName(int i) {
    static const char* kNames[] = {"Pad", "Lead", "Bass", "Key", "Pluck",
                                   "Bell", "String", "Drum", "Perc", "FX"};
    return (i >= 0 && i < 10) ? kNames[i] : "?";
}

// A glyph per instrument type, drawn into `box` -- which should be square.
//
// Hand-drawn paths rather than an icon font: the plugin ships one font for
// text and adding a second purely for eleven marks is a resource, a licence
// and a build step for something a dozen lines of geometry does. They are
// deliberately schematic -- what the type's envelope or waveform looks like --
// rather than pictorial, because at 20 px a picture of a bell is a smudge.
//
// typeIndex is 0-9 as the seed's last digit, or -1 for "any type".
inline void DrawTypeIcon(IGraphics& g, const IRECT& box, int typeIndex,
                         const IColor& color, float weight = 1.6f) {
    const float l = box.L, r = box.R, t = box.T, b = box.B;
    const float w = box.W(), h = box.H();
    const float mx = box.MW(), my = box.MH();
    const IPattern pat(color);

    const auto stroke = [&] { g.PathStroke(pat, weight); };

    g.PathClear();
    switch (typeIndex) {
        case -1:                     // Any -- a star, the wildcard
            for (int i = 0; i < 3; ++i) {
                const float a = 3.14159265f * (0.5f + float(i) / 3.f);
                const float dx = std::cos(a) * w * 0.42f, dy = std::sin(a) * h * 0.42f;
                g.PathMoveTo(mx - dx, my - dy);
                g.PathLineTo(mx + dx, my + dy);
            }
            stroke();
            break;

        case 0:                      // Pad -- a slow swell
            g.PathMoveTo(l, b);
            g.PathCubicBezierTo(l + w * 0.35f, b, l + w * 0.2f, t, mx, t);
            g.PathCubicBezierTo(r - w * 0.2f, t, r - w * 0.35f, b, r, b);
            stroke();
            break;

        case 1:                      // Lead -- a sawtooth
            g.PathMoveTo(l, b);
            g.PathLineTo(l + w * 0.45f, t);
            g.PathLineTo(l + w * 0.45f, b);
            g.PathLineTo(r - w * 0.05f, t);
            stroke();
            break;

        case 2:                      // Bass -- one long low wave
            g.PathMoveTo(l, my);
            g.PathCubicBezierTo(l + w * 0.25f, t, r - w * 0.25f, b, r, my);
            stroke();
            break;

        case 3: {                    // Key -- piano keys
            g.PathRect(IRECT(l + w * 0.08f, t + h * 0.1f, r - w * 0.08f, b - h * 0.1f));
            stroke();
            const float kw = w * 0.13f;
            for (float x : {l + w * 0.28f, l + w * 0.55f}) {
                g.PathClear();
                g.PathRect(IRECT(x, t + h * 0.1f, x + kw, my + h * 0.12f));
                g.PathFill(pat);
            }
            break;
        }

        case 4:                      // Pluck -- a spike that dies away
            g.PathMoveTo(l + w * 0.12f, b);
            g.PathLineTo(l + w * 0.24f, t);
            g.PathCubicBezierTo(l + w * 0.5f, t + h * 0.55f, r - w * 0.2f, b, r, b);
            stroke();
            break;

        case 5:                      // Bell -- the bell itself
            g.PathMoveTo(l + w * 0.14f, b - h * 0.22f);
            g.PathCubicBezierTo(l + w * 0.16f, t + h * 0.05f, r - w * 0.16f,
                                t + h * 0.05f, r - w * 0.14f, b - h * 0.22f);
            g.PathLineTo(l + w * 0.14f, b - h * 0.22f);
            stroke();
            g.PathClear();
            g.PathCircle(mx, b - h * 0.08f, w * 0.07f);
            g.PathFill(pat);
            break;

        case 6:                      // String -- a bowed, sustained wave
            g.PathMoveTo(l, my);
            for (int i = 0; i < 2; ++i) {
                const float x0 = l + w * 0.5f * float(i);
                g.PathCubicBezierTo(x0 + w * 0.12f, t + h * 0.18f,
                                    x0 + w * 0.38f, b - h * 0.18f,
                                    x0 + w * 0.5f, my);
            }
            stroke();
            break;

        case 7:                      // Drum -- a head, struck
            g.PathCircle(mx, my + h * 0.08f, w * 0.34f);
            stroke();
            g.PathClear();
            g.PathMoveTo(mx + w * 0.12f, t);
            g.PathLineTo(mx + w * 0.34f, my - h * 0.14f);
            stroke();
            break;

        case 8:                      // Perc -- a short transient burst
            for (int i = 0; i < 3; ++i) {
                const float x = l + w * (0.25f + 0.25f * float(i));
                const float amp = h * (0.42f - 0.12f * float(i));
                g.PathMoveTo(x, my - amp);
                g.PathLineTo(x, my + amp);
            }
            stroke();
            break;

        default:                     // FX -- something unpredictable
            g.PathMoveTo(l, my + h * 0.2f);
            g.PathLineTo(l + w * 0.3f, my - h * 0.3f);
            g.PathLineTo(l + w * 0.45f, my + h * 0.1f);
            g.PathLineTo(l + w * 0.72f, my - h * 0.38f);
            g.PathLineTo(r, my + h * 0.28f);
            stroke();
            break;
    }
}

// The roll-type filter, as a grid of labelled icons rather than a knob.
//
// It was an IVKnobControl, which is the wrong instrument entirely: eleven
// unordered names on a dial have no natural direction, no way to jump to one,
// and no way to see which is selected without reading a value readout. A grid
// shows all eleven and takes one click.
//
// Bound to a host parameter, so automation and the host's own generic editor
// keep working.
class TypeGridControl : public IControl {
public:
    TypeGridControl(const IRECT& bounds, int paramIdx)
    : IControl(bounds, paramIdx) {}

    void Draw(IGraphics& g) override {
        const IRECT grid = mRECT.GetReducedFromTop(12.f);
        g.DrawText(IText(10.f, IColor(255, 130, 140, 155), nullptr, EAlign::Near),
                   "ROLL TYPE", mRECT.GetFromTop(12.f));

        const int sel = Selected();
        const float cw = grid.W() / float(kCols);
        const float ch = grid.H() / float(kRows);

        for (int i = 0; i < kCount; ++i) {
            const IRECT cell = CellAt(grid, i);
            const bool on = (i == sel);
            g.FillRoundRect(on ? IColor(255, 62, 116, 178) : IColor(255, 26, 29, 34),
                            cell.GetPadded(-1.f), 3.f);

            const IColor ink = on ? IColor(255, 250, 252, 255) : IColor(255, 150, 160, 175);
            // Icon over label, both inside the cell. The label is what makes
            // the icons learnable; without it they are a guessing game.
            const float iconH = std::min(cell.H() * 0.55f, cw * 0.55f);
            const IRECT icon = IRECT(cell.MW() - iconH * 0.5f, cell.T + 3.f,
                                     cell.MW() + iconH * 0.5f, cell.T + 3.f + iconH);
            DrawTypeIcon(g, icon, i - 1, ink, on ? 1.8f : 1.4f);
            g.DrawText(IText(9.f, ink, nullptr, EAlign::Center),
                       i == 0 ? "Any" : TypeName(i - 1),
                       cell.GetFromBottom(11.f));
        }
        (void)ch;
    }

    void OnMouseDown(float x, float y, const IMouseMod&) override {
        const IRECT grid = mRECT.GetReducedFromTop(12.f);
        for (int i = 0; i < kCount; ++i) {
            if (!CellAt(grid, i).Contains(x, y)) continue;
            if (i == Selected()) return;
            SetValue(GetParam() ? GetParam()->ToNormalized(double(i))
                                : double(i) / double(kCount - 1));
            SetDirty(true);
            return;
        }
    }

private:
    static constexpr int kCount = 11;    // "Any" plus the ten types
    static constexpr int kCols = 6;
    static constexpr int kRows = 2;

    int Selected() const {
        if (!GetParam()) return 0;
        return std::clamp(static_cast<int>(std::lround(
                              GetParam()->FromNormalized(GetValue()))), 0, kCount - 1);
    }

    IRECT CellAt(const IRECT& grid, int i) const {
        const float cw = grid.W() / float(kCols);
        const float ch = grid.H() / float(kRows);
        const int col = i % kCols, row = i / kCols;
        return IRECT(grid.L + cw * col, grid.T + ch * row,
                     grid.L + cw * (col + 1), grid.T + ch * (row + 1));
    }
};

// Displays the whole 32-bit seed as one number, even though the host sees two
// 16-bit parameters. The split exists because a single float32 automation value
// cannot round-trip a 32-bit seed (see SeedlatheParams.h) -- but nobody should
// have to think in two halves to type a seed in.
//
// Click to type, drag to nudge, mouse wheel to step.
class SeedBoxControl : public IControl {
public:
    using SeedFunc = std::function<void(uint32_t)>;

    SeedBoxControl(const IRECT& bounds, SeedFunc onSeed, const IVStyle& style = DEFAULT_STYLE)
    : IControl(bounds), mOnSeed(std::move(onSeed)), mStyle(style) {
        mIgnoreMouse = false;
        mDblAsSingleClick = true;
        // 4294967295 is ten digits and IGraphics allows seven by default, so
        // without this the largest seed a user could type in was 9999999.
        SetTextEntryLength(10);
    }

    void SetSeed(uint32_t s) {
        if (s == mSeed) return;
        mSeed = s;
        SetDirty(false);
    }
    uint32_t GetSeed() const { return mSeed; }

    void Draw(IGraphics& g) override {
        const IColor bg(255, 18, 20, 24);
        const IColor edge = mMouseIsOver ? IColor(255, 120, 190, 255) : IColor(255, 60, 66, 76);
        g.FillRoundRect(bg, mRECT, 4.f);
        g.DrawRoundRect(edge, mRECT, 4.f, nullptr, 1.5f);

        char buf[32];
        std::snprintf(buf, sizeof(buf), "%u", mSeed);

        const IRECT label = mRECT.GetFromTop(16.f).GetHPadded(-8.f);
        const IRECT value = mRECT.GetReducedFromTop(14.f).GetHPadded(-8.f);

        g.DrawText(IText(11.f, IColor(255, 130, 140, 155), nullptr, EAlign::Near), "SEED", label);
        g.DrawText(IText(22.f, IColor(255, 235, 240, 248), nullptr, EAlign::Center), buf, value);
    }

    void OnMouseDown(float x, float y, const IMouseMod& mod) override {
        if (mod.R) { PromptForEntry(); return; }
        mDragAccum = 0.f;
        mDragStart = mSeed;
    }

    void OnMouseDblClick(float x, float y, const IMouseMod& mod) override { PromptForEntry(); }

    void OnMouseDrag(float x, float y, float dX, float dY, const IMouseMod& mod) override {
        // Fine by default, coarse with shift: the range is four billion wide, so
        // a plain drag has to move in useful steps without being unusable.
        const double step = mod.S ? 100000.0 : (mod.C ? 1.0 : 100.0);
        mDragAccum -= dY;
        const double delta = double(mDragAccum) * step;
        const double next = double(mDragStart) + delta;
        Commit(static_cast<uint32_t>(next < 0.0 ? 0.0 : (next > 4294967295.0 ? 4294967295.0 : next)));
    }

    void OnMouseWheel(float x, float y, const IMouseMod& mod, float d) override {
        const double step = mod.S ? 100000.0 : (mod.C ? 1.0 : 100.0);
        const double next = double(mSeed) + double(d) * step;
        Commit(static_cast<uint32_t>(next < 0.0 ? 0.0 : (next > 4294967295.0 ? 4294967295.0 : next)));
    }

    void OnTextEntryCompletion(const char* str, int) override {
        if (!str || !*str) return;
        const double v = std::strtod(str, nullptr);
        Commit(static_cast<uint32_t>(v < 0.0 ? 0.0 : (v > 4294967295.0 ? 4294967295.0 : v)));
    }

private:
    void PromptForEntry() {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%u", mSeed);
        GetUI()->CreateTextEntry(*this, IText(18.f), mRECT.GetReducedFromTop(14.f), buf);
    }

    void Commit(uint32_t s) {
        if (s == mSeed) return;
        mSeed = s;
        if (mOnSeed) mOnSeed(s);
        SetDirty(false);
    }

    SeedFunc mOnSeed;
    IVStyle mStyle;
    uint32_t mSeed = 13;
    float mDragAccum = 0.f;
    uint32_t mDragStart = 0;
};

// A row of tabs that shows one control group and hides the rest.
//
// iPlug2 ships IVTabbedPagesControl, but its pages are held in a
// std::map<const char*, ...> -- keyed on the POINTER, so the tab order is
// whatever the linker happened to lay the string literals out as -- and its
// AddPage is private, so the order cannot be fixed from outside. This is a few
// dozen lines and gives a stable order plus the look the rest of the UI uses.
class TabBarControl : public IControl {
public:
    using SelectFunc = std::function<void(int index)>;

    TabBarControl(const IRECT& bounds, std::vector<std::string> names, SelectFunc onSelect)
    : IControl(bounds), mNames(std::move(names)), mOnSelect(std::move(onSelect)) {}

    int Selected() const { return mSelected; }

    void Select(int i) {
        if (i < 0 || i >= static_cast<int>(mNames.size()) || i == mSelected) return;
        mSelected = i;
        if (mOnSelect) mOnSelect(i);
        SetDirty(false);
    }

    void Draw(IGraphics& g) override {
        const int n = static_cast<int>(mNames.size());
        if (n == 0) return;
        const float w = mRECT.W() / float(n);
        for (int i = 0; i < n; ++i) {
            const IRECT t(mRECT.L + w * i, mRECT.T, mRECT.L + w * (i + 1), mRECT.B);
            const bool sel = (i == mSelected);
            g.FillRoundRect(sel ? IColor(255, 44, 52, 64) : IColor(255, 26, 29, 34),
                            t.GetPadded(-2.f), 4.f);
            if (sel)
                g.FillRect(IColor(255, 120, 190, 255), t.GetFromBottom(2.f).GetPadded(-2.f, 0.f, -2.f, 0.f));
            g.DrawText(IText(13.f, sel ? IColor(255, 240, 245, 250) : IColor(255, 140, 150, 165),
                             nullptr, EAlign::Center),
                       mNames[static_cast<size_t>(i)].c_str(), t);
        }
    }

    void OnMouseDown(float x, float y, const IMouseMod&) override {
        const int n = static_cast<int>(mNames.size());
        if (n == 0) return;
        const int i = static_cast<int>((x - mRECT.L) / (mRECT.W() / float(n)));
        Select(std::min(std::max(i, 0), n - 1));
    }

private:
    std::vector<std::string> mNames;
    SelectFunc mOnSelect;
    int mSelected = 0;
};

// The sixteen multitimbral parts, as two rows of eight.
//
// Two rows because sixteen cells in one row across the space the top bar has
// spare would be 22 px each, which is not a click target. Parts that have never
// been addressed are drawn dimmer than the rest: they cost nothing until
// selected, and showing that is the difference between "empty" and "broken".
class PartStripControl : public IControl {
public:
    using CountFunc = std::function<bool(int index)>;   // is this part allocated
    using SelectFunc = std::function<void(int index)>;

    PartStripControl(const IRECT& bounds, int numParts, CountFunc allocated,
                     SelectFunc onSelect)
    : IControl(bounds), mNumParts(numParts), mAllocated(std::move(allocated)),
      mOnSelect(std::move(onSelect)) {}

    void SetSelected(int i) { mSelected = i; SetDirty(false); }
    void SetEnabled(bool on) { mEnabled = on; SetDirty(false); }

    void Draw(IGraphics& g) override {
        const IRECT grid = mRECT.GetReducedFromTop(12.f);
        g.DrawText(IText(10.f, IColor(255, 130, 140, 155), nullptr, EAlign::Near),
                   mEnabled ? "PART (MIDI CHANNEL)" : "PART -- multitimbral is off",
                   mRECT.GetFromTop(12.f));

        const float w = grid.W() / 8.f;
        const float h = grid.H() / 2.f;
        for (int i = 0; i < mNumParts; ++i) {
            const int col = i % 8, row = i / 8;
            const IRECT cell(grid.L + w * col, grid.T + h * row,
                             grid.L + w * (col + 1), grid.T + h * (row + 1));
            const bool live = mAllocated && mAllocated(i);
            const bool sel = (i == mSelected);

            IColor fill = IColor(255, 26, 29, 34);
            if (sel) fill = IColor(255, 62, 116, 178);
            else if (live) fill = IColor(255, 42, 47, 56);
            g.FillRoundRect(fill, cell.GetPadded(-1.f), 3.f);

            IColor text = mEnabled ? (live ? IColor(255, 208, 214, 222)
                                          : IColor(255, 96, 104, 118))
                                   : IColor(255, 70, 76, 88);
            if (sel) text = IColor(255, 250, 252, 255);

            char buf[8];
            std::snprintf(buf, sizeof(buf), "%d", i + 1);
            g.DrawText(IText(10.f, text, nullptr, EAlign::Center), buf, cell);
        }
    }

    void OnMouseDown(float x, float y, const IMouseMod&) override {
        if (!mEnabled) return;
        const IRECT grid = mRECT.GetReducedFromTop(12.f);
        if (!grid.Contains(x, y)) return;
        const int col = std::clamp(static_cast<int>((x - grid.L) / (grid.W() / 8.f)), 0, 7);
        const int row = std::clamp(static_cast<int>((y - grid.T) / (grid.H() / 2.f)), 0, 1);
        const int i = row * 8 + col;
        if (i >= mNumParts || i == mSelected) return;
        mSelected = i;
        if (mOnSelect) mOnSelect(i);
        SetDirty(false);
    }

private:
    int mNumParts;
    CountFunc mAllocated;
    SelectFunc mOnSelect;
    int mSelected = 0;
    bool mEnabled = false;
};

// Text entry that is not attached to a parameter.
//
// IGraphics only delivers OnTextEntryCompletion to the control that opened the
// entry, so asking the user for a preset name needs a control to own the
// callback. This one draws nothing and exists purely to be that owner, which
// keeps every caller from having to grow its own text-entry plumbing.
class TextPromptControl : public IControl {
public:
    using DoneFunc = std::function<void(const char* text)>;

    TextPromptControl() : IControl(IRECT()) {
        mIgnoreMouse = true;
        // The whole point of this control is collecting a name, and the
        // IGraphics default of seven characters cut every one of them off
        // mid-word with no indication that it had.
        SetTextEntryLength(static_cast<int>(sl::kMaxPresetNameChars));
    }

    void Prompt(const IRECT& where, const char* initial, DoneFunc done) {
        mDone = std::move(done);
        GetUI()->CreateTextEntry(*this, IText(14.f), where, initial ? initial : "");
    }

    void Draw(IGraphics&) override {}

    void OnTextEntryCompletion(const char* str, int) override {
        // Cancelling hands back an empty string; treat that as "no change"
        // rather than as a preset named "".
        if (mDone && str && *str) mDone(str);
        mDone = nullptr;
    }

private:
    DoneFunc mDone;
};

// A scrolling, grouped list. Used for the factory bank and for search results,
// which want the same behaviour: many rows, one selected, click to audition.
class ListControl : public IControl {
public:
    struct Row {
        std::string text;
        std::string detail;
        bool header = false;
        int payload = 0;
    };
    using PickFunc = std::function<void(int payload)>;

    ListControl(const IRECT& bounds, PickFunc onPick)
    : IControl(bounds), mOnPick(std::move(onPick)) {}

    void SetRows(std::vector<Row> rows) {
        mRows = std::move(rows);
        mScroll = 0.f;
        SetDirty(false);
    }
    void SetSelected(int payload) { mSelected = payload; SetDirty(false); }

    void Draw(IGraphics& g) override {
        g.FillRoundRect(IColor(255, 16, 18, 22), mRECT, 4.f);
        g.PathClipRegion(mRECT);

        float y = mRECT.T - mScroll;
        for (const auto& r : mRows) {
            const float h = r.header ? kHeaderH : kRowH;
            const IRECT row(mRECT.L, y, mRECT.R, y + h);
            if (row.B > mRECT.T && row.T < mRECT.B) {
                if (r.header) {
                    g.FillRect(IColor(255, 26, 30, 36), row);
                    g.DrawText(IText(11.f, IColor(255, 140, 200, 255), nullptr, EAlign::Near),
                               r.text.c_str(), row.GetHPadded(-8.f));
                } else {
                    const bool sel = (r.payload == mSelected);
                    if (sel) g.FillRect(IColor(255, 40, 70, 110), row);
                    g.DrawText(IText(13.f, sel ? IColor(255, 255, 255, 255) : IColor(255, 208, 214, 222),
                                     nullptr, EAlign::Near),
                               r.text.c_str(), row.GetHPadded(-10.f));
                    if (!r.detail.empty())
                        g.DrawText(IText(11.f, IColor(255, 120, 130, 145), nullptr, EAlign::Far),
                                   r.detail.c_str(), row.GetHPadded(-10.f));
                }
            }
            y += h;
        }
        mContentH = y + mScroll - mRECT.T;
        g.PathClipRegion(IRECT());
    }

    void OnMouseDown(float x, float y, const IMouseMod& mod) override {
        float top = mRECT.T - mScroll;
        for (const auto& r : mRows) {
            const float h = r.header ? kHeaderH : kRowH;
            if (y >= top && y < top + h) {
                if (!r.header) {
                    mSelected = r.payload;
                    if (mOnPick) mOnPick(r.payload);
                    SetDirty(false);
                }
                return;
            }
            top += h;
        }
    }

    void OnMouseWheel(float x, float y, const IMouseMod& mod, float d) override {
        const float maxScroll = std::max(0.f, mContentH - mRECT.H());
        mScroll = std::min(maxScroll, std::max(0.f, mScroll - d * 40.f));
        SetDirty(false);
    }

private:
    static constexpr float kRowH = 22.f;
    static constexpr float kHeaderH = 20.f;

    PickFunc mOnPick;
    std::vector<Row> mRows;
    int mSelected = -1;
    float mScroll = 0.f;
    float mContentH = 0.f;
};

} // namespace seedlathe
