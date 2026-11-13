// SPDX-License-Identifier: GPL-3.0-or-later
#include "graphstrip.h"

#include "graphgdi.h"

#include <cmath>
#include <cwchar>

namespace ap {

namespace {
const wchar_t* const kClass = L"APGraphStrip";
bool g_registered = false;
} // namespace

namespace graph {

COLORREF blend(COLORREF fg, COLORREF bg, double a) {
    auto channel = [&](int shift) {
        const int f = static_cast<int>((fg >> shift) & 0xFF), b = static_cast<int>((bg >> shift) & 0xFF);
        return static_cast<COLORREF>(std::lround(b + (f - b) * a)) << shift;
    };
    return channel(0) | channel(8) | channel(16);
}

POINT ipt(double x, double y) { return POINT{static_cast<LONG>(std::lround(x)), static_cast<LONG>(std::lround(y))}; }

std::wstring number(double v, int decimals, const wchar_t* unit, bool sign) {
    wchar_t b[48];
    swprintf(b, 48, sign ? L"%+.*f%ls" : L"%.*f%ls", decimals, v, unit);
    return b;
}

} // namespace graph

using namespace graph;

double GraphStrip::Plot::X(double v) const {
    const double f = logX ? std::log(std::max(v, 1e-9) / x0) / std::log(x1 / x0) : (v - x0) / (x1 - x0);
    return l + std::min(1.0, std::max(0.0, f)) * (r - l);
}

double GraphStrip::Plot::Y(double v) const {
    const double f = (v - y0) / (y1 - y0);
    return b - std::min(1.0, std::max(0.0, f)) * (b - t);
}

double GraphStrip::Plot::fromX(double px) const {
    const double f = std::min(1.0, std::max(0.0, (px - l) / std::max(1.0, r - l)));
    return logX ? x0 * std::pow(x1 / x0, f) : x0 + f * (x1 - x0);
}

double GraphStrip::Plot::fromY(double py) const {
    const double f = std::min(1.0, std::max(0.0, (b - py) / std::max(1.0, b - t)));
    return y0 + f * (y1 - y0);
}

GraphStrip::~GraphStrip() {
    if (font_) DeleteObject(font_);
    if (symbol_) DeleteObject(symbol_);
    if (still_) DeleteObject(still_);
    if (frame_) DeleteObject(frame_);
    if (layer_) DeleteObject(layer_);
}

void GraphStrip::unregisterClass(HINSTANCE module) {
    if (g_registered) UnregisterClassW(kClass, module);
    g_registered = false;
}

void GraphStrip::create(HWND parent, HINSTANCE module, UINT dpi) {
    if (!g_registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof wc;
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = proc;
        wc.hInstance = module;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        g_registered = RegisterClassExW(&wc) != 0;
    }
    mark_.assign(static_cast<size_t>(markCount()), POINT{-10000, -10000});
    setDpi(dpi);
    hwnd_ = CreateWindowExW(0, kClass, L"", WS_CHILD, 0, 0, 10, 10, parent, nullptr, module, this);
}

void GraphStrip::setDpi(UINT dpi) {
    dpi_ = dpi ? dpi : 96;
    if (font_) DeleteObject(font_);
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi_);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfHeight = lf.lfHeight * 85 / 100; // the analysis window's small font
    font_ = CreateFontIndirectW(&lf);
    if (symbol_) DeleteObject(symbol_);
    lf = ncm.lfMessageFont; // the arrow of the rows' reset buttons, smaller
    lf.lfHeight = lf.lfHeight * 7 / 5;
    wcscpy_s(lf.lfFaceName, L"Segoe UI Symbol");
    symbol_ = CreateFontIndirectW(&lf);
    if (HDC dc = GetDC(nullptr)) { // the small font's height, and the widths a strip lays its legends out with
        HGDIOBJ old = SelectObject(dc, font_);
        TEXTMETRICW tm{};
        GetTextMetricsW(dc, &tm);
        textH_ = tm.tmHeight;
        measure(dc);
        SelectObject(dc, old);
        ReleaseDC(nullptr, dc);
    }
    stillDirty_ = true;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

int GraphStrip::textWidth(HDC dc, const wchar_t* s) const {
    SIZE z{};
    GetTextExtentPoint32W(dc, s, lstrlenW(s), &z);
    return static_cast<int>(z.cx);
}

// Also while a mark is dragged: the owner has the value the graph gave it, and what a rule moved with it (a split
// pushes its neighbours) has to show.
void GraphStrip::setValues(const PresetValues& values, const PresetValues& reset) {
    PresetValues v = values;
    normaliseValues(v);
    if (v == values_ && reset == resetValues_) return;
    values_ = v;
    resetValues_ = reset;
    stillDirty_ = true;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void GraphStrip::setLive(const AnalysisSource* source, int deck) {
    source_ = source;
    deck_ = deck;
}

void GraphStrip::refresh() {
    if (hwnd_ && IsWindowVisible(hwnd_)) InvalidateRect(hwnd_, nullptr, FALSE);
}

std::vector<GraphStrip::Tip> GraphStrip::tips() {
    RECT rc{};
    if (hwnd_) GetClientRect(hwnd_, &rc);
    icons_.clear();
    steppers_.clear();
    layout(rc.right, rc.bottom);
    std::vector<Tip> all;
    fillTips(all);
    return all;
}

bool GraphStrip::lit(int param) const {
    const size_t p = static_cast<size_t>(param);
    return std::fabs(values_[p] - resetValues_[p]) > 1e-9;
}

bool GraphStrip::live() const { return source_ && source_->audioActive() && source_->meters(deck_).live; }

const DeckAnalysis* GraphStrip::analysis() const { return source_ ? &source_->analysis(deck_) : nullptr; }

const DeckMeters* GraphStrip::meters() const { return source_ ? &source_->meters(deck_) : nullptr; }

double GraphStrip::stepped(int param, int steps) const {
    const double now = values_[static_cast<size_t>(param)];
    return paramValueAt(param, std::min(paramPositions(param), std::max(0, paramPositionOf(param, now) + steps)));
}

void GraphStrip::layoutFour(int w, int h, RECT panel[4]) const {
    static const double kBeside[4] = {2.2, 1.0, 1.0, 1.0};
    const int gap = px(5), pad = px(5);
    const int innerH = h - 2 * pad, topH = static_cast<int>((innerH - gap) * 0.58);
    const bool twoRows = innerH >= 2 * px(130) + gap && (w - 2 * pad) <= 4.6 * topH;
    const int first = twoRows ? 1 : 0, top = twoRows ? pad + topH + gap : pad;
    if (twoRows) panel[0] = {pad, pad, w - pad, pad + topH};
    double sum = 0.0;
    for (int i = first; i < 4; ++i) sum += kBeside[i];
    const double usable = w - 2 * pad - (4 - first - 1) * gap;
    double x = pad;
    for (int i = first; i < 4; ++i) {
        const double pw = usable * kBeside[i] / sum;
        panel[i] = {static_cast<LONG>(x + 0.5), top, static_cast<LONG>(x + pw + 0.5), h - pad};
        x += pw + gap;
    }
}

RECT GraphStrip::cornerIcon(const RECT& p) const {
    return RECT{p.right - px(3) - px(22), p.top + px(1), p.right - px(3), p.top + px(1) + px(18)};
}

void GraphStrip::measureLegend(HDC dc, Legend& lg) const {
    for (int j = 0; j < lg.count; ++j) {
        lg.nameW[j] = textWidth(dc, lg.name[j]);
        lg.valueW[j] = textWidth(dc, lg.widest[j]);
    }
}

void GraphStrip::layoutLegend(Legend& lg, const RECT& c, int titleW, int rightW) {
    const int cTop = static_cast<int>(c.top), cLeft = static_cast<int>(c.left);
    const int between = px(14), iconW = px(22), rowH = px(18);
    int inLine = 0;
    for (int j = 0; j < lg.count; ++j) inLine += lg.nameW[j] + px(6) + lg.valueW[j] + px(2) + iconW + (j ? between : 0);
    const int from = cLeft + px(6) + titleW + px(16), until = static_cast<int>(c.right) - px(6) - rightW - px(12);
    lg.inLine = from + inLine <= until;
    lg.beside = false;
    if (lg.inLine) {
        const int top = cTop + px(3) - (rowH - textH_) / 2; // the texts on the title's base line
        int x = from;
        for (int j = 0; j < lg.count; ++j) {
            const int item = lg.nameW[j] + px(6) + lg.valueW[j] + px(2);
            lg.item[j] = {x, top, x + item, top + rowH};
            icons_.push_back({RECT{x + item, top, x + item + iconW, top + rowH}, lg.param[j]});
            x += item + iconW + between;
        }
        lg.bounds = {from, top, x - between, top + rowH};
        lg.headerBottom = std::max(cTop + textH_ + px(5), top + rowH);
        return;
    }
    int nameW = 0, valueW = 0;
    for (int j = 0; j < lg.count; ++j) {
        nameW = std::max(nameW, lg.nameW[j]);
        valueW = std::max(valueW, lg.valueW[j]);
    }
    const int item = nameW + px(8) + valueW + px(2), cell = item + iconW;
    int perLine = std::min(lg.count, std::max(1, (static_cast<int>(c.right) - cLeft - 2 * px(6) + between) / (cell + between)));
    if (perLine == 3 && lg.count == 4) perLine = 2;
    const int top = cTop + textH_ + px(5), lines = (lg.count + perLine - 1) / perLine;
    for (int j = 0; j < lg.count; ++j) {
        const int x = cLeft + px(6) + (j % perLine) * (cell + between), y = top + (j / perLine) * rowH;
        lg.item[j] = {x, y, x + item, y + rowH};
        icons_.push_back({RECT{x + item, y, x + cell, y + rowH}, lg.param[j]});
    }
    lg.bounds = {cLeft + px(6), top, cLeft + px(6) + perLine * (cell + between) - between, top + lines * rowH};
    lg.headerBottom = top + lines * rowH;
}

int GraphStrip::legendColumnWidth(const Legend& lg) const {
    int nameW = 0, valueW = 0;
    for (int j = 0; j < lg.count; ++j) {
        nameW = std::max(nameW, lg.nameW[j]);
        valueW = std::max(valueW, lg.valueW[j]);
    }
    return nameW + px(8) + valueW + px(2) + px(22);
}

void GraphStrip::layoutLegendColumn(Legend& lg, const RECT& c) {
    const int iconW = px(22), rowH = px(18), item = legendColumnWidth(lg) - iconW;
    const int x = static_cast<int>(c.left) + px(6), top = static_cast<int>(c.top) + textH_ + px(5);
    for (int j = 0; j < lg.count; ++j) {
        const int y = top + j * rowH;
        lg.item[j] = {x, y, x + item, y + rowH};
        icons_.push_back({RECT{x + item, y, x + item + iconW, y + rowH}, lg.param[j]});
    }
    lg.bounds = {x, top, x + item + iconW, top + lg.count * rowH};
    lg.headerBottom = top;
    lg.inLine = false;
    lg.beside = true;
}

// On the title's line: left of the legend, right of it, and everything under the header. Else (in columns under the
// title, or one column beside the graph): the title line above it, everything right of it, and what is under it.
void GraphStrip::legendTips(const Legend& lg, const RECT& c, const wchar_t* text, std::vector<Tip>& out) {
    const RECT& lb = lg.bounds;
    if (lg.inLine) {
        out.push_back({RECT{c.left, c.top, lb.left, lg.headerBottom}, text, -1, -1});
        out.push_back({RECT{lb.right, c.top, c.right, lg.headerBottom}, text, -1, -1});
        out.push_back({RECT{c.left, lg.headerBottom, c.right, c.bottom}, text, -1, -1});
    } else {
        out.push_back({RECT{c.left, c.top, c.right, lb.top}, text, -1, -1});
        out.push_back({RECT{lb.right, lb.top, c.right, c.bottom}, text, -1, -1});
        out.push_back({RECT{c.left, lb.bottom, lb.right, c.bottom}, text, -1, -1});
    }
}

void GraphStrip::drawPanels(Gdiplus::Graphics& g, const RECT* panel, int count) {
    using namespace Gdiplus;
    SolidBrush panelBrush(col(kPanel));
    Pen edge(col(kEdge), 1.0f);
    for (int i = 0; i < count; ++i) {
        const RECT& p = panel[i];
        g.FillRectangle(&panelBrush, static_cast<REAL>(p.left), static_cast<REAL>(p.top),
                        static_cast<REAL>(p.right - p.left), static_cast<REAL>(p.bottom - p.top));
        g.DrawRectangle(&edge, static_cast<REAL>(p.left) + 0.5f, static_cast<REAL>(p.top) + 0.5f,
                        static_cast<REAL>(p.right - p.left - 1), static_cast<REAL>(p.bottom - p.top - 1));
    }
}

void GraphStrip::drawAttack(Gdiplus::Graphics& g, Plot& q, int mark, double hitDb, double reduce, double attackMs,
                            double spanMs, double tickMs, const wchar_t* spanText, std::vector<Label>& labels) {
    using namespace Gdiplus;
    const float lw = static_cast<float>(std::max(1.0, dpi_ / 96.0));
    Pen grid(col(kGrid), lw), dash(col(kOrange), lw), curve(col(kTeal), 2.0f * lw), inputDash(col(kInput), 1.5f * lw);
    dash.SetDashStyle(DashStyleDash);
    inputDash.SetDashStyle(DashStyleDash);
    curve.SetLineJoin(LineJoinRound);
    SolidBrush orangeFill(col(kOrange, 60));
    q.logX = false;
    q.x0 = 0.0;
    q.x1 = spanMs;
    q.y0 = 0.0;
    q.y1 = hitDb + 2.0;
    auto level = [&](double ms) { return ms < kHitAtMs ? 0.0 : hitDb - reduce * (1.0 - std::exp(-(ms - kHitAtMs) / attackMs)); };
    g.SetClip(RectF(static_cast<REAL>(q.l - 1), static_cast<REAL>(q.t - 1), static_cast<REAL>(q.r - q.l + 2),
                    static_cast<REAL>(q.b - q.t + 2)));
    g.DrawLine(&grid, pt(q.l, q.Y(0)), pt(q.r, q.Y(0)));
    std::vector<PointF> line, area;
    line.push_back(pt(q.X(0), q.Y(0)));
    line.push_back(pt(q.X(kHitAtMs), q.Y(0)));
    area.push_back(pt(q.X(kHitAtMs), q.Y(hitDb)));
    const double step = spanMs / 160.0;
    for (double ms = kHitAtMs; ms <= spanMs + 0.01; ms += step) {
        line.push_back(pt(q.X(ms), q.Y(level(ms))));
        area.push_back(line.back());
    }
    area.push_back(pt(q.X(spanMs), q.Y(hitDb)));
    g.FillPolygon(&orangeFill, area.data(), static_cast<INT>(area.size()));
    const PointF hit[4] = {pt(q.X(0), q.Y(0)), pt(q.X(kHitAtMs), q.Y(0)), pt(q.X(kHitAtMs), q.Y(hitDb)),
                           pt(q.X(spanMs), q.Y(hitDb))};
    g.DrawLines(&inputDash, hit, 4);
    g.DrawLines(&curve, line.data(), static_cast<INT>(line.size()));
    const double xm = q.X(kHitAtMs + attackMs);
    g.DrawLine(&dash, pt(xm, q.t), pt(xm, q.b));
    for (double ms = tickMs; ms <= spanMs - kHitAtMs + 0.01; ms += tickMs)
        g.DrawLine(&grid, pt(q.X(kHitAtMs + ms), q.b - px(4)), pt(q.X(kHitAtMs + ms), q.b));
    g.ResetClip();
    drawMark(g, mark, xm, q.Y(level(kHitAtMs + attackMs)));
    const bool right = xm > q.l + 0.6 * (q.r - q.l);
    labels.push_back({static_cast<int>(xm + (right ? -px(5) : px(5))), static_cast<int>(q.t),
                      number(attackMs, 0, L" ms"), kOrange, static_cast<UINT>(right ? TA_RIGHT : TA_LEFT)});
    labels.push_back({static_cast<int>(q.r), static_cast<int>(q.b) - textH_ - px(4), spanText, kDim, TA_RIGHT});
}

void GraphStrip::drawRelease(Gdiplus::Graphics& g, Plot& q, int mark, double reduce, double releaseMs, double fromMs,
                             double toMs, double tickMs, double endMs, const wchar_t* endText,
                             std::vector<Label>& labels) {
    using namespace Gdiplus;
    const float lw = static_cast<float>(std::max(1.0, dpi_ / 96.0));
    Pen grid(col(kGrid), lw), dash(col(kOrange), lw), curve(col(kTeal), 2.0f * lw);
    dash.SetDashStyle(DashStyleDash);
    curve.SetLineJoin(LineJoinRound);
    SolidBrush orangeFill(col(kOrange, 60)), inputFill(col(kInput, 70));
    q.logX = false;
    q.x0 = fromMs;
    q.x1 = toMs;
    q.y0 = -10.0;
    q.y1 = 0.6;
    auto gain = [&](double ms) { return ms < 0.0 ? -reduce : -reduce * std::exp(-ms / releaseMs); };
    g.SetClip(RectF(static_cast<REAL>(q.l - 1), static_cast<REAL>(q.t - 1), static_cast<REAL>(q.r - q.l + 2),
                    static_cast<REAL>(q.b - q.t + 2)));
    g.FillRectangle(&inputFill, static_cast<REAL>(q.X(fromMs)), static_cast<REAL>(q.t),
                    static_cast<REAL>(q.X(0) - q.X(fromMs)), static_cast<REAL>(q.b - q.t));
    g.DrawLine(&grid, pt(q.l, q.Y(0)), pt(q.r, q.Y(0)));
    std::vector<PointF> line, area;
    area.push_back(pt(q.X(fromMs), q.Y(0)));
    const double step = (toMs - fromMs) / 240.0;
    for (double ms = fromMs; ms <= toMs + 0.01; ms += step) {
        line.push_back(pt(q.X(ms), q.Y(gain(ms))));
        area.push_back(line.back());
    }
    area.push_back(pt(q.X(toMs), q.Y(0)));
    g.FillPolygon(&orangeFill, area.data(), static_cast<INT>(area.size()));
    g.DrawLines(&curve, line.data(), static_cast<INT>(line.size()));
    const double xm = q.X(releaseMs);
    g.DrawLine(&dash, pt(xm, q.t), pt(xm, q.b));
    for (double ms = tickMs; ms <= endMs + 0.01; ms += tickMs) g.DrawLine(&grid, pt(q.X(ms), q.b - px(4)), pt(q.X(ms), q.b));
    g.ResetClip();
    drawMark(g, mark, xm, q.Y(gain(releaseMs)));
    const bool right = xm > q.l + 0.6 * (q.r - q.l);
    const int y = static_cast<int>(q.b) - textH_ - px(4);
    labels.push_back({static_cast<int>(xm + (right ? -px(5) : px(5))), y, number(releaseMs, 0, L" ms"), kOrange,
                      static_cast<UINT>(right ? TA_RIGHT : TA_LEFT)});
    labels.push_back({static_cast<int>(q.X(0)) - px(3), y, L"hit", kDim, TA_RIGHT});
    if (releaseMs < 0.78 * endMs) labels.push_back({static_cast<int>(q.X(endMs)) - px(3), y, endText, kDim, TA_RIGHT});
}

// A round mark that can be dragged, and where it is (markAt).
void GraphStrip::drawMark(Gdiplus::Graphics& g, int id, double x, double y) {
    using namespace Gdiplus;
    const double lw = std::max(1.0, dpi_ / 96.0), mr = px(45) / 10.0;
    SolidBrush ring(col(kPanel)), fill(col(kOrange));
    g.FillEllipse(&ring, static_cast<REAL>(x - mr - 2 * lw), static_cast<REAL>(y - mr - 2 * lw),
                  static_cast<REAL>(2 * (mr + 2 * lw)), static_cast<REAL>(2 * (mr + 2 * lw)));
    g.FillEllipse(&fill, static_cast<REAL>(x - mr), static_cast<REAL>(y - mr), static_cast<REAL>(2 * mr),
                  static_cast<REAL>(2 * mr));
    if (id >= 0 && id < static_cast<int>(mark_.size()))
        mark_[static_cast<size_t>(id)] = {static_cast<LONG>(x + 0.5), static_cast<LONG>(y + 0.5)};
}

// GdiAlphaBlend: gdi32's own.
void GraphStrip::blendLayer(HDC dc, const RECT& r) {
    if (!layer_) return;
    const int x = std::max(0L, r.left), y = std::max(0L, r.top);
    const int w = std::min<int>(bitmapW_, r.right) - x, h = std::min<int>(bitmapH_, r.bottom) - y;
    if (w <= 0 || h <= 0) return;
    HDC layer = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(layer, layer_);
    const BLENDFUNCTION over{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    GdiAlphaBlend(dc, x, y, w, h, layer, x, y, w, h, over);
    SelectObject(layer, old);
    DeleteDC(layer);
}

void GraphStrip::polygon(HDC dc, const std::vector<POINT>& pts, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, pts.data(), static_cast<int>(pts.size()));
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(b);
}

void GraphStrip::polyline(HDC dc, const std::vector<POINT>& pts, COLORREF c, int width, int style) {
    HPEN pen = CreatePen(style, width, c);
    HGDIOBJ op = SelectObject(dc, pen);
    Polyline(dc, pts.data(), static_cast<int>(pts.size()));
    SelectObject(dc, op);
    DeleteObject(pen);
}

void GraphStrip::text(HDC dc, double x, double y, const std::wstring& s, COLORREF c, UINT align) {
    SetTextColor(dc, c);
    SetTextAlign(dc, align | TA_TOP);
    TextOutW(dc, static_cast<int>(x + 0.5), static_cast<int>(y + 0.5), s.c_str(), static_cast<int>(s.size()));
}

int GraphStrip::clip(HDC dc, const Plot& q) {
    const int saved = SaveDC(dc);
    IntersectClipRect(dc, static_cast<int>(q.l) - 1, static_cast<int>(q.t) - 1, static_cast<int>(q.r) + 2,
                      static_cast<int>(q.b) + 2);
    return saved;
}

void GraphStrip::labels(HDC dc, const std::vector<Label>& all) {
    for (const Label& l : all) text(dc, l.x, l.y, l.text, l.colour, l.align);
}

// What does not move with the audio is drawn once into `still_` (again when a value, the size or the DPI changes);
// every refresh copies that and draws the live parts on it. Drawn whole at every refresh, a strip took a quarter of
// a processor core (GDI+ anti-aliasing and blended fills over the full area, 20 times a second). Still things that
// lie over moving ones - the compressor's curve over the level - go into `layer_`, a bitmap with transparency that
// the live drawing lays over its background (blendLayer); the reset icons and steppers are drawn last.
void GraphStrip::paint(HDC target) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return;
    if (!still_ || !frame_ || w != bitmapW_ || h != bitmapH_) {
        if (still_) DeleteObject(still_);
        if (frame_) DeleteObject(frame_);
        if (layer_) DeleteObject(layer_);
        still_ = CreateCompatibleBitmap(target, w, h);
        frame_ = CreateCompatibleBitmap(target, w, h);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h; // top-down: the first row first, as GDI+ wants it
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        layerBits_ = nullptr;
        layer_ = CreateDIBSection(target, &bi, DIB_RGB_COLORS, &layerBits_, nullptr, 0);
        bitmapW_ = w;
        bitmapH_ = h;
        stillDirty_ = true;
    }
    HDC dc = CreateCompatibleDC(target), from = CreateCompatibleDC(target);
    HGDIOBJ oldFont = SelectObject(dc, font_);
    HGDIOBJ oldBmp = SelectObject(dc, still_);
    SetBkMode(dc, TRANSPARENT);
    if (stillDirty_) {
        icons_.clear();
        moreIcons_.clear();
        steppers_.clear();
        layout(w, h);
        renderStill(dc, w, h);
        if (layer_ && layerBits_) {
            Gdiplus::Bitmap layer(w, h, w * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(layerBits_));
            Gdiplus::Graphics g(&layer);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.Clear(Gdiplus::Color(0, 0, 0, 0));
            renderLayer(g, w, h);
        }
        stillDirty_ = false;
    }
    SelectObject(dc, frame_);
    HGDIOBJ oldFrom = SelectObject(from, still_);
    BitBlt(dc, 0, 0, w, h, from, 0, 0, SRCCOPY);
    SelectObject(from, oldFrom);
    paintLive(dc);
    drawControls(dc);
    BitBlt(target, 0, 0, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    DeleteDC(from);
    DeleteDC(dc);
}

// The reset icons - lit when the value differs from its reset value, as the rows' reset buttons - and the steppers.
void GraphStrip::drawControls(HDC dc) {
    SetTextAlign(dc, TA_LEFT | TA_TOP);
    HGDIOBJ oldFont = SelectObject(dc, symbol_);
    for (const std::vector<Icon>* list : {&icons_, &moreIcons_}) {
        for (const Icon& icon : *list) {
            SetTextColor(dc, lit(icon.param) ? kText : kGrid);
            RECT r = icon.rect;
            DrawTextW(dc, L"\x21BA", 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP | DT_NOPREFIX);
        }
    }
    SelectObject(dc, font_);
    for (const Stepper& s : steppers_) {
        const double now = values_[static_cast<size_t>(s.param)];
        const bool can = stepped(s.param, s.steps) != now;
        HBRUSH fill = CreateSolidBrush(blend(kInput, kPanel, 0.16)), frame = CreateSolidBrush(kGrid);
        FillRect(dc, &s.rect, fill);
        FrameRect(dc, &s.rect, frame);
        DeleteObject(fill);
        DeleteObject(frame);
        SetTextColor(dc, can ? kText : kGrid);
        RECT r = s.rect;
        DrawTextW(dc, s.sign, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP | DT_NOPREFIX);
    }
    SelectObject(dc, oldFont);
}

int GraphStrip::markAt(POINT p) const {
    const int reach = px(11);
    int best = -1;
    long bestD = static_cast<long>(reach) * reach + 1;
    for (size_t i = 0; i < mark_.size(); ++i) {
        const long dx = p.x - mark_[i].x, dy = p.y - mark_[i].y, d = dx * dx + dy * dy;
        if (d < bestD) {
            bestD = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

const GraphStrip::Icon* GraphStrip::iconAt(POINT p) const {
    for (const std::vector<Icon>* list : {&icons_, &moreIcons_})
        for (const Icon& icon : *list)
            if (PtInRect(&icon.rect, p) && lit(icon.param)) return &icon;
    return nullptr;
}

const GraphStrip::Stepper* GraphStrip::stepperAt(POINT p) const {
    for (const Stepper& s : steppers_)
        if (PtInRect(&s.rect, p)) return &s;
    return nullptr;
}

// The mark being dragged follows the mouse along its own direction; its value goes on one of the slider's positions.
void GraphStrip::drag(POINT p) {
    const int param = markParam(drag_);
    double v = 0.0;
    if (param < 0 || !dragValue(drag_, p, v)) return;
    const ParamInfo& in = paramInfo(param);
    v = std::min(in.max, std::max(in.min, v));
    const double snapped = paramValueAt(param, paramPositionOf(param, v));
    if (snapped != values_[static_cast<size_t>(param)]) change(param, snapped, false);
}

// A value set in the graph: the picture first, then the owner (heard at once; saved when `save`).
void GraphStrip::change(int param, double value, bool save) {
    values_[static_cast<size_t>(param)] = value;
    stillDirty_ = true;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    if (edit_) edit_(param, value, save);
}

LRESULT CALLBACK GraphStrip::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* self = static_cast<GraphStrip*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    auto* self = reinterpret_cast<GraphStrip*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == WM_NCDESTROY) { // the window goes (with its parent): the object lives on without it
        self->hwnd_ = nullptr;
        self->drag_ = -1;
        self->pressParam_ = -1;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return self->handle(msg, wp, lp);
}

LRESULT GraphStrip::handle(UINT msg, WPARAM wp, LPARAM lp) {
    auto point = [&] { return POINT{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))}; };
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd_, &ps);
        paint(dc);
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const POINT p = point();
        // a reset icon or a stepper acts when the button is let go on it, as a button
        if (const Icon* icon = iconAt(p)) {
            pressParam_ = icon->param;
            pressSteps_ = 0;
        } else if (const Stepper* s = stepperAt(p)) {
            pressParam_ = s->param;
            pressSteps_ = s->steps;
        } else {
            drag_ = markAt(p);
            if (drag_ < 0) drag_ = lineAt(p);
        }
        if (pressParam_ >= 0 || drag_ >= 0) SetCapture(hwnd_);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (drag_ >= 0) drag(point());
        return 0;
    case WM_LBUTTONUP:
        if (pressParam_ >= 0) {
            const int param = pressParam_, steps = pressSteps_;
            const POINT p = point();
            pressParam_ = -1;
            ReleaseCapture();
            if (steps == 0) {
                const Icon* icon = iconAt(p);
                if (icon && icon->param == param) {
                    if (resetTo_) resetTo_(param);
                    else change(param, resetValues_[static_cast<size_t>(param)], true);
                }
            } else {
                const Stepper* s = stepperAt(p);
                const double v = stepped(param, steps);
                if (s && s->param == param && s->steps == steps && v != values_[static_cast<size_t>(param)])
                    change(param, v, true);
            }
        } else if (drag_ >= 0) {
            const int param = markParam(drag_);
            drag_ = -1;
            ReleaseCapture();
            if (edit_ && param >= 0) edit_(param, values_[static_cast<size_t>(param)], true); // saved now
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (drag_ >= 0) { // taken away in the middle of a drag: what is heard is saved
            const int param = markParam(drag_);
            drag_ = -1;
            if (edit_ && param >= 0) edit_(param, values_[static_cast<size_t>(param)], true);
        }
        pressParam_ = -1;
        return 0;
    case WM_MOUSEWHEEL: { // one step of a value that has no slider: the fine control the slider's arrow keys were
        POINT p = point();
        ScreenToClient(hwnd_, &p);
        const int param = drag_ < 0 ? wheelParam(p) : -1;
        if (param < 0) break;
        wheel_ += GET_WHEEL_DELTA_WPARAM(wp);
        const int steps = wheel_ / WHEEL_DELTA;
        wheel_ -= steps * WHEEL_DELTA;
        const double v = stepped(param, steps);
        if (steps && v != values_[static_cast<size_t>(param)]) change(param, v, true);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd_, &p);
            if (pressParam_ >= 0 || (drag_ < 0 && (iconAt(p) || stepperAt(p)))) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            int m = drag_ >= 0 ? drag_ : markAt(p);
            if (m < 0) m = lineAt(p);
            if (m >= 0) {
                SetCursor(LoadCursorW(nullptr, markUpDown(m) ? IDC_SIZENS : IDC_SIZEWE));
                return TRUE;
            }
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

} // namespace ap
