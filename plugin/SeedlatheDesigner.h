#pragma once

// Controls for the Design tab.
//
// None of these are bound to host parameters. The designer edits an
// sl::Instrument directly -- a seed produces roughly 200 numbers and exposing
// them all as automatable parameters would bury the six that matter (seed,
// volume, octave, voices, roll type) in a list no host can present usefully.
// The edited instrument travels in the state chunk instead.
//
// Because the target of every control moves when the selected oscillator
// changes, controls bind to getter/setter pairs rather than to a field
// address, and re-read the model through Sync() whenever the instrument is
// replaced underneath them.

#include "IControl.h"
#include "IControls.h"
#include "sl/Instrument.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace iplug;
using namespace igraphics;

namespace seedlathe {

namespace dsn {
const IColor kPanelBg(255, 27, 30, 36);
const IColor kPanelEdge(255, 46, 52, 62);
const IColor kTrack(255, 18, 20, 24);
const IColor kFill(255, 62, 116, 178);
const IColor kFillHot(255, 96, 164, 236);
const IColor kText(255, 214, 221, 230);
const IColor kLabel(255, 132, 142, 158);
const IColor kAccent(255, 120, 190, 255);
const IColor kOffText(255, 96, 104, 118);

inline IText Label(float h = 10.f, EAlign a = EAlign::Near) {
    return IText(h, kLabel, nullptr, a);
}
inline IText Value(float h = 11.f, EAlign a = EAlign::Far) {
    return IText(h, kText, nullptr, a);
}
} // namespace dsn

// Every designer control re-reads its value from the model on Sync(). The tab
// calls this on the whole group after a seed change, a preset load, or an
// oscillator switch, so no control caches a stale number.
class DesignerControl : public IControl
{
public:
    explicit DesignerControl(const IRECT& bounds) : IControl(bounds) {}
    virtual void Sync() = 0;
};

// A titled background for a group of controls. Purely decorative.
class PanelControl : public IControl
{
public:
    PanelControl(const IRECT& bounds, const char* title)
    : IControl(bounds), mTitle(title ? title : "") {
        mIgnoreMouse = true;
    }

    void Draw(IGraphics& g) override {
        g.FillRoundRect(dsn::kPanelBg, mRECT, 4.f);
        g.DrawRoundRect(dsn::kPanelEdge, mRECT, 4.f, nullptr, 1.f);
        if (!mTitle.empty())
            g.DrawText(IText(10.f, dsn::kLabel, nullptr, EAlign::Near), mTitle.c_str(),
                       mRECT.GetFromTop(15.f).GetHPadded(-8.f));
    }

private:
    std::string mTitle;
};

// A horizontal label/value strip. Drag anywhere on it to change the value;
// the fill shows where it sits in range. Double-click to type a number.
//
// Vertical drag rather than horizontal: the strips are wide and short, so a
// horizontal drag would make every value jump by a large fraction of range on
// the smallest movement.
class DragValueControl : public DesignerControl
{
public:
    using GetFunc = std::function<double()>;
    using SetFunc = std::function<void(double)>;

    DragValueControl(const IRECT& bounds, const char* label, double lo, double hi,
                     int decimals, const char* unit,
                     GetFunc get, SetFunc set, std::function<void()> onChange)
    : DesignerControl(bounds), mLabel(label ? label : ""), mLo(lo), mHi(hi),
      mDecimals(decimals), mUnit(unit ? unit : ""),
      mGet(std::move(get)), mSet(std::move(set)), mOnChange(std::move(onChange)) {
        mDblAsSingleClick = false;
        mVal = mGet ? mGet() : 0.0;
    }

    void Sync() override {
        if (mGet) mVal = mGet();
        SetDirty(false);
    }

    void Draw(IGraphics& g) override {
        const double norm = (mHi > mLo) ? std::clamp((mVal - mLo) / (mHi - mLo), 0.0, 1.0) : 0.0;

        g.FillRoundRect(dsn::kTrack, mRECT, 3.f);
        if (norm > 0.0) {
            IRECT fill = mRECT;
            fill.R = mRECT.L + mRECT.W() * static_cast<float>(norm);
            g.FillRoundRect(mMouseIsOver ? dsn::kFillHot : dsn::kFill, fill, 3.f);
        }

        char buf[48];
        std::snprintf(buf, sizeof(buf), "%.*f%s", mDecimals, mVal, mUnit.c_str());
        const IRECT inner = mRECT.GetHPadded(-6.f);
        g.DrawText(dsn::Label(10.f, EAlign::Near), mLabel.c_str(), inner);
        g.DrawText(dsn::Value(11.f, EAlign::Far), buf, inner);
    }

    void OnMouseDown(float x, float y, const IMouseMod& mod) override {
        if (mod.R) { PromptForEntry(); return; }
        mDragFrom = mVal;
        mDragAccum = 0.f;
    }

    void OnMouseDblClick(float, float, const IMouseMod&) override { PromptForEntry(); }

    void OnMouseDrag(float, float, float, float dY, const IMouseMod& mod) override {
        // 150 px of travel covers the range; shift gives a tenth of that.
        const double span = (mHi - mLo) * (mod.S ? 0.1 : 1.0);
        mDragAccum -= dY;
        Commit(mDragFrom + double(mDragAccum) * span / 150.0);
    }

    void OnMouseWheel(float, float, const IMouseMod& mod, float d) override {
        const double step = (mHi - mLo) * (mod.S ? 0.002 : 0.02);
        Commit(mVal + double(d) * step);
    }

    void OnTextEntryCompletion(const char* str, int) override {
        if (str && *str) Commit(std::strtod(str, nullptr));
    }

private:
    void PromptForEntry() {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%.*f", mDecimals, mVal);
        GetUI()->CreateTextEntry(*this, IText(12.f), mRECT, buf);
    }

    void Commit(double v) {
        v = std::clamp(v, mLo, mHi);
        if (v == mVal) return;
        mVal = v;
        if (mSet) mSet(v);
        if (mOnChange) mOnChange();
        SetDirty(false);
    }

    std::string mLabel, mUnit;
    double mLo, mHi, mVal = 0.0;
    int mDecimals;
    GetFunc mGet;
    SetFunc mSet;
    std::function<void()> mOnChange;
    double mDragFrom = 0.0;
    float mDragAccum = 0.f;
};

// A row of mutually exclusive text segments: waveform, filter type, oversample.
class SegmentControl : public DesignerControl
{
public:
    using GetFunc = std::function<int()>;
    using SetFunc = std::function<void(int)>;

    SegmentControl(const IRECT& bounds, std::vector<const char*> items,
                   GetFunc get, SetFunc set, std::function<void()> onChange)
    : DesignerControl(bounds), mItems(std::move(items)),
      mGet(std::move(get)), mSet(std::move(set)), mOnChange(std::move(onChange)) {
        mSel = mGet ? mGet() : 0;
    }

    void Sync() override {
        if (mGet) mSel = mGet();
        SetDirty(false);
    }

    void Draw(IGraphics& g) override {
        const int n = static_cast<int>(mItems.size());
        if (n == 0) return;
        const float w = mRECT.W() / float(n);
        for (int i = 0; i < n; ++i) {
            const IRECT cell(mRECT.L + w * i, mRECT.T, mRECT.L + w * (i + 1), mRECT.B);
            const bool sel = (i == mSel);
            g.FillRoundRect(sel ? dsn::kFill : dsn::kTrack, cell.GetPadded(-1.f), 3.f);
            g.DrawText(IText(10.f, sel ? IColor(255, 250, 252, 255) : dsn::kLabel,
                             nullptr, EAlign::Center),
                       mItems[static_cast<size_t>(i)], cell);
        }
    }

    void OnMouseDown(float x, float, const IMouseMod&) override {
        const int n = static_cast<int>(mItems.size());
        if (n == 0) return;
        const int i = std::clamp(static_cast<int>((x - mRECT.L) / (mRECT.W() / float(n))), 0, n - 1);
        if (i == mSel) return;
        mSel = i;
        if (mSet) mSet(i);
        if (mOnChange) mOnChange();
        SetDirty(false);
    }

private:
    std::vector<const char*> mItems;
    GetFunc mGet;
    SetFunc mSet;
    std::function<void()> mOnChange;
    int mSel = 0;
};

// A labelled on/off pill.
class SwitchControl : public DesignerControl
{
public:
    using GetFunc = std::function<bool()>;
    using SetFunc = std::function<void(bool)>;

    SwitchControl(const IRECT& bounds, const char* label,
                  GetFunc get, SetFunc set, std::function<void()> onChange)
    : DesignerControl(bounds), mLabel(label ? label : ""),
      mGet(std::move(get)), mSet(std::move(set)), mOnChange(std::move(onChange)) {
        mOn = mGet && mGet();
    }

    void Sync() override {
        if (mGet) mOn = mGet();
        SetDirty(false);
    }

    void Draw(IGraphics& g) override {
        const IRECT pill = mRECT.GetFromLeft(26.f).GetVPadded(-4.f);
        g.FillRoundRect(mOn ? dsn::kAccent : dsn::kTrack, pill, pill.H() * 0.5f);
        const float r = pill.H() * 0.5f - 2.f;
        const float cx = mOn ? pill.R - r - 2.f : pill.L + r + 2.f;
        g.FillCircle(IColor(255, 24, 27, 32), cx, pill.MH(), r);
        g.DrawText(IText(11.f, mOn ? dsn::kText : dsn::kOffText, nullptr, EAlign::Near),
                   mLabel.c_str(), mRECT.GetReducedFromLeft(32.f));
    }

    void OnMouseDown(float, float, const IMouseMod&) override {
        mOn = !mOn;
        if (mSet) mSet(mOn);
        if (mOnChange) mOnChange();
        SetDirty(false);
    }

private:
    std::string mLabel;
    GetFunc mGet;
    SetFunc mSet;
    std::function<void()> mOnChange;
    bool mOn = false;
};

// Editor for one sl::Adsr: four [time, value] pairs applied as sequential
// linear ramps. Drag a handle to move its stage in time (x) and level (y).
//
// The plot holds a sustain plateau between the third and fourth stages,
// because that is what a held note actually does -- Voice schedules the first
// three ramps at note-on and the fourth at note-off. Plotting four back-to-back
// ramps would draw an envelope no note ever produces.
class AdsrControl : public DesignerControl
{
public:
    using GetFunc = std::function<sl::Adsr()>;
    using SetFunc = std::function<void(const sl::Adsr&)>;

    AdsrControl(const IRECT& bounds, const char* title, const char* scaleHint,
                GetFunc get, SetFunc set, std::function<void()> onChange)
    : DesignerControl(bounds), mTitle(title ? title : ""),
      mScaleHint(scaleHint ? scaleHint : ""),
      mGet(std::move(get)), mSet(std::move(set)), mOnChange(std::move(onChange)) {
        if (mGet) mEnv = mGet();
    }

    void Sync() override {
        if (mGet) mEnv = mGet();
        SetDirty(false);
    }

    void Draw(IGraphics& g) override {
        g.FillRoundRect(dsn::kPanelBg, mRECT, 4.f);
        g.DrawRoundRect(dsn::kPanelEdge, mRECT, 4.f, nullptr, 1.f);
        g.DrawText(IText(10.f, dsn::kLabel, nullptr, EAlign::Near), mTitle.c_str(),
                   mRECT.GetFromTop(14.f).GetHPadded(-8.f));
        if (!mScaleHint.empty())
            g.DrawText(IText(9.f, dsn::kOffText, nullptr, EAlign::Far), mScaleHint.c_str(),
                       mRECT.GetFromTop(14.f).GetHPadded(-8.f));

        const IRECT plot = Plot();
        g.FillRect(dsn::kTrack, plot);

        float px[5], py[5];
        Points(plot, px, py);

        // Envelope outline, with the sustain plateau drawn between S and R.
        g.PathClear();
        g.PathMoveTo(px[0], py[0]);
        for (int i = 1; i <= 3; ++i) g.PathLineTo(px[i], py[i]);
        g.PathLineTo(mSustainX(plot), py[3]);
        g.PathLineTo(px[4], py[4]);
        g.PathStroke(IPattern(dsn::kFillHot), 1.5f);

        // The plateau again, dotted, so it reads as "held", not "ramped".
        for (float x = px[3]; x < mSustainX(plot); x += 6.f)
            g.FillRect(dsn::kAccent, IRECT(x, py[3] - 0.5f, std::min(x + 3.f, mSustainX(plot)), py[3] + 0.5f));

        for (int i = 1; i <= 4; ++i) {
            const bool hot = (mHot == i);
            g.FillCircle(hot ? IColor(255, 255, 255, 255) : dsn::kAccent, px[i], py[i], hot ? 4.5f : 3.5f);
        }

        char buf[64];
        std::snprintf(buf, sizeof(buf), "A %.3fs  D %.3fs  S %.2f  R %.3fs",
                      mEnv.aT, mEnv.dT, mEnv.sV, mEnv.rT);
        g.DrawText(IText(9.f, dsn::kOffText, nullptr, EAlign::Center), buf,
                   mRECT.GetFromBottom(12.f));
    }

    void OnMouseDown(float x, float y, const IMouseMod&) override {
        mHot = NearestHandle(x, y);
        // Latch the time window for the whole drag. Recomputing it as the
        // envelope grows meant a stage crossing a bracket doubled the plot's
        // seconds-per-pixel mid-gesture: the handle slowed down under the
        // cursor and then slid away from it.
        mDragWindow = Window();
        mDragging = true;
        SetDirty(false);
    }

    void OnMouseUp(float, float, const IMouseMod&) override {
        mHot = 0;
        mDragging = false;
        SetDirty(false);
    }

    void OnMouseOver(float x, float y, const IMouseMod&) override {
        const int h = NearestHandle(x, y);
        if (h != mHot) { mHot = h; SetDirty(false); }
    }

    void OnMouseOut() override {
        if (mHot) { mHot = 0; SetDirty(false); }
    }

    void OnMouseDrag(float, float, float dX, float dY, const IMouseMod& mod) override {
        if (mHot < 1 || mHot > 4) return;
        const IRECT plot = Plot();
        const double secPerPx = Window() / std::max(1.f, plot.W());
        const double valPerPx = 1.0 / std::max(1.f, plot.H());
        const double fine = mod.S ? 0.15 : 1.0;

        double* t = nullptr; double* v = nullptr;
        switch (mHot) {
            case 1: t = &mEnv.aT; v = &mEnv.aV; break;
            case 2: t = &mEnv.dT; v = &mEnv.dV; break;
            case 3: t = &mEnv.sT; v = &mEnv.sV; break;
            default: t = &mEnv.rT; v = &mEnv.rV; break;
        }
        *t = std::clamp(*t + double(dX) * secPerPx * fine, 0.0, kMaxStage);
        *v = std::clamp(*v - double(dY) * valPerPx * fine, 0.0, 1.0);

        if (mSet) mSet(mEnv);
        if (mOnChange) mOnChange();
        SetDirty(false);
    }

private:
    static constexpr double kMaxStage = 4.0;

    IRECT Plot() const {
        return mRECT.GetPadded(-8.f).GetReducedFromTop(8.f).GetReducedFromBottom(10.f);
    }

    // A round window rather than a tight fit, and frozen while dragging: the
    // plot must not rescale under the cursor.
    double Window() const {
        if (mDragging) return mDragWindow;
        const double total = mEnv.aT + mEnv.dT + mEnv.sT + mEnv.rT;
        for (double w : {0.5, 1.0, 2.0, 4.0, 8.0})
            if (total <= w * 0.75) return w;
        return 16.0;
    }

    float mSustainX(const IRECT& plot) const {
        // The plateau takes a fixed slice of the plot; it has no duration.
        return plot.L + plot.W() * static_cast<float>(TimeAt(3) / Window()) + plot.W() * 0.12f;
    }

    double TimeAt(int stage) const {
        double t = 0.0;
        const double ts[4] = {mEnv.aT, mEnv.dT, mEnv.sT, mEnv.rT};
        for (int i = 0; i < stage && i < 4; ++i) t += ts[i];
        return t;
    }

    void Points(const IRECT& plot, float* px, float* py) const {
        const double win = Window();
        const double vs[5] = {0.0, mEnv.aV, mEnv.dV, mEnv.sV, mEnv.rV};
        for (int i = 0; i <= 3; ++i) {
            px[i] = plot.L + plot.W() * static_cast<float>(std::min(TimeAt(i) / win, 1.0));
            py[i] = plot.B - plot.H() * static_cast<float>(std::clamp(vs[i], 0.0, 1.0));
        }
        // The release handle sits after the plateau.
        px[4] = std::min(plot.R, mSustainX(plot) + plot.W() * static_cast<float>(mEnv.rT / win));
        py[4] = plot.B - plot.H() * static_cast<float>(std::clamp(vs[4], 0.0, 1.0));
    }

    int NearestHandle(float x, float y) const {
        const IRECT plot = Plot();
        float px[5], py[5];
        Points(plot, px, py);
        int best = 0;
        float bestD = 14.f * 14.f;
        for (int i = 1; i <= 4; ++i) {
            const float dx = x - px[i], dy = y - py[i];
            const float d = dx * dx + dy * dy;
            if (d < bestD) { bestD = d; best = i; }
        }
        return best;
    }

    std::string mTitle, mScaleHint;
    GetFunc mGet;
    SetFunc mSet;
    std::function<void()> mOnChange;
    sl::Adsr mEnv{};
    int mHot = 0;
    bool mDragging = false;
    double mDragWindow = 1.0;
};

// The 5x5 FM matrix: rows modulate columns, each cell -1..1. Drag a cell
// vertically to set its amount; right-click clears it.
class FmMatrixControl : public DesignerControl
{
public:
    using GetFunc = std::function<double(int src, int tgt)>;
    using SetFunc = std::function<void(int src, int tgt, double v)>;
    using CountFunc = std::function<int()>;

    FmMatrixControl(const IRECT& bounds, GetFunc get, SetFunc set, CountFunc count,
                    std::function<void()> onChange)
    : DesignerControl(bounds), mGet(std::move(get)), mSet(std::move(set)),
      mCount(std::move(count)), mOnChange(std::move(onChange)) {}

    void Sync() override { SetDirty(false); }

    void Draw(IGraphics& g) override {
        const int n = mCount ? std::clamp(mCount(), 0, sl::kMaxOscs) : 0;
        const IRECT grid = Grid();
        const float cw = grid.W() / float(sl::kMaxOscs);
        const float ch = grid.H() / float(sl::kMaxOscs);

        g.DrawText(IText(9.f, dsn::kOffText, nullptr, EAlign::Near), "modulator",
                   mRECT.GetFromTop(12.f).GetHPadded(-4.f));
        g.DrawText(IText(9.f, dsn::kOffText, nullptr, EAlign::Far), "carrier ->",
                   mRECT.GetFromTop(12.f).GetHPadded(-4.f));

        for (int src = 0; src < sl::kMaxOscs; ++src) {
            for (int tgt = 0; tgt < sl::kMaxOscs; ++tgt) {
                const IRECT cell(grid.L + cw * tgt, grid.T + ch * src,
                                 grid.L + cw * (tgt + 1), grid.T + ch * (src + 1));
                const bool live = (src < n && tgt < n);
                g.FillRect(live ? dsn::kTrack : IColor(255, 33, 36, 42), cell.GetPadded(-1.f));

                if (!live) continue;

                const double v = mGet ? mGet(src, tgt) : 0.0;
                if (v != 0.0) {
                    // Positive fills up from the middle, negative fills down,
                    // so the sign is readable without reading the number.
                    const float mid = cell.MH();
                    const float h = cell.H() * 0.5f * static_cast<float>(std::min(std::fabs(v), 1.0));
                    const IRECT bar = (v > 0.0) ? IRECT(cell.L + 2.f, mid - h, cell.R - 2.f, mid)
                                                : IRECT(cell.L + 2.f, mid, cell.R - 2.f, mid + h);
                    g.FillRect(v > 0.0 ? dsn::kFillHot : IColor(255, 214, 118, 96), bar);
                }
                if (mHotSrc == src && mHotTgt == tgt)
                    g.DrawRect(dsn::kAccent, cell.GetPadded(-1.f), nullptr, 1.5f);

                char buf[16];
                std::snprintf(buf, sizeof(buf), "%.2f", v);
                g.DrawText(IText(9.f, v == 0.0 ? dsn::kOffText : dsn::kText, nullptr, EAlign::Center),
                           buf, cell);
            }
        }
        g.DrawRect(dsn::kPanelEdge, grid, nullptr, 1.f);
    }

    void OnMouseDown(float x, float y, const IMouseMod& mod) override {
        if (!CellAt(x, y, mHotSrc, mHotTgt)) return;
        if (mod.R) { Commit(0.0); return; }
        mDragFrom = mGet ? mGet(mHotSrc, mHotTgt) : 0.0;
        mDragAccum = 0.f;
        SetDirty(false);
    }

    void OnMouseDrag(float, float, float, float dY, const IMouseMod& mod) override {
        if (mHotSrc < 0) return;
        mDragAccum -= dY;
        Commit(mDragFrom + double(mDragAccum) * (mod.S ? 0.002 : 0.02));
    }

    void OnMouseUp(float, float, const IMouseMod&) override { mHotSrc = mHotTgt = -1; SetDirty(false); }

private:
    IRECT Grid() const { return mRECT.GetReducedFromTop(14.f); }

    bool CellAt(float x, float y, int& src, int& tgt) const {
        const IRECT grid = Grid();
        if (!grid.Contains(x, y)) return false;
        const int n = mCount ? std::clamp(mCount(), 0, sl::kMaxOscs) : 0;
        tgt = std::clamp(static_cast<int>((x - grid.L) / (grid.W() / float(sl::kMaxOscs))), 0, sl::kMaxOscs - 1);
        src = std::clamp(static_cast<int>((y - grid.T) / (grid.H() / float(sl::kMaxOscs))), 0, sl::kMaxOscs - 1);
        return src < n && tgt < n;
    }

    void Commit(double v) {
        v = std::clamp(v, -1.0, 1.0);
        if (mSet) mSet(mHotSrc, mHotTgt, v);
        if (mOnChange) mOnChange();
        SetDirty(false);
    }

    GetFunc mGet;
    SetFunc mSet;
    CountFunc mCount;
    std::function<void()> mOnChange;
    int mHotSrc = -1, mHotTgt = -1;
    double mDragFrom = 0.0;
    float mDragAccum = 0.f;
};

// The oscillator selector: one button per slot, dimmed past the count.
class OscSelectControl : public DesignerControl
{
public:
    using CountFunc = std::function<int()>;
    using SelectFunc = std::function<void(int)>;

    OscSelectControl(const IRECT& bounds, CountFunc count, SelectFunc onSelect)
    : DesignerControl(bounds), mCount(std::move(count)), mOnSelect(std::move(onSelect)) {}

    int Selected() const { return mSel; }
    void Sync() override {
        const int n = mCount ? mCount() : 1;
        if (mSel >= n) { mSel = std::max(0, n - 1); if (mOnSelect) mOnSelect(mSel); }
        SetDirty(false);
    }

    void Draw(IGraphics& g) override {
        const int n = mCount ? mCount() : 0;
        const float w = mRECT.W() / float(sl::kMaxOscs);
        for (int i = 0; i < sl::kMaxOscs; ++i) {
            const IRECT cell(mRECT.L + w * i, mRECT.T, mRECT.L + w * (i + 1), mRECT.B);
            const bool live = i < n;
            const bool sel = live && i == mSel;
            g.FillRoundRect(sel ? dsn::kFill : dsn::kPanelBg, cell.GetPadded(-2.f), 3.f);
            char buf[24];
            std::snprintf(buf, sizeof(buf), "OSC %d", i + 1);
            g.DrawText(IText(11.f, live ? (sel ? IColor(255, 250, 252, 255) : dsn::kLabel)
                                        : IColor(255, 62, 68, 78),
                             nullptr, EAlign::Center), buf, cell);
        }
    }

    void OnMouseDown(float x, float, const IMouseMod&) override {
        const int n = mCount ? mCount() : 0;
        const int i = std::clamp(static_cast<int>((x - mRECT.L) / (mRECT.W() / float(sl::kMaxOscs))),
                                 0, sl::kMaxOscs - 1);
        if (i >= n || i == mSel) return;
        mSel = i;
        if (mOnSelect) mOnSelect(i);
        SetDirty(false);
    }

private:
    CountFunc mCount;
    SelectFunc mOnSelect;
    int mSel = 0;
};

} // namespace seedlathe
