#include "theme.h"

#include <commctrl.h>
#include <objidl.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <string>
#include <vector>

namespace Gdiplus { // gdiplus.h uses min / max unqualified
using std::max;
using std::min;
} // namespace Gdiplus
#include <gdiplus.h>

namespace ap::theme {

namespace {

ULONG_PTR g_gdiplus = 0;

// aluminium: a lighter top going to a darker bottom, a slightly warm grey (client's sample, 2026-10-25: about
// 152 / 150 / 145 on average); a soft sheen in the middle of the width
const COLORREF kMetalTop = RGB(168, 166, 161), kMetalBottom = RGB(136, 134, 129);
const double kSheen = 8.0; // brightness added in the middle (fades to 0 at the edges)
const COLORREF kText = RGB(18, 19, 22);
// fields (edit boxes, lists) and tabs: a light grey in the metal's warm tint instead of white
const COLORREF kField = RGB(226, 225, 221), kTabBack = RGB(196, 195, 190);

HBRUSH fieldBrush() {
    static HBRUSH b = CreateSolidBrush(kField); // lives as long as the program
    return b;
}

// The brush strokes: a tile of brightness offsets, generated once from a fixed seed (the same pattern every start,
// random to the eye). Rows wrap horizontally, so the tile repeats without a visible seam.
constexpr int kTileW = 1024, kTileH = 512;

struct Random { // small linear congruential generator: no <random> state to carry around
    uint32_t s;
    double next() { // 0 .. 1
        s = s * 1664525u + 1013904223u;
        return (s >> 8) * (1.0 / 16777216.0);
    }
};

const std::vector<float>& strokes() {
    static std::vector<float> t;
    if (!t.empty()) return t;
    t.assign(static_cast<size_t>(kTileW) * kTileH, 0.0f);
    Random rnd{0x5EEDBA5Eu};
    auto at = [&](int x, int y) -> float& {
        return t[static_cast<size_t>((y + kTileH) % kTileH) * kTileW + static_cast<size_t>((x % kTileW + kTileW) % kTileW)];
    };
    // every row its own slight level: the fine lines of the grain
    for (int y = 0; y < kTileH; ++y) {
        const float row = static_cast<float>((rnd.next() + rnd.next() - 1.0) * 3.0);
        for (int x = 0; x < kTileW; ++x) at(x, y) += row;
    }
    // strokes of random length, place and strength, lighter or darker, with soft ends; some two rows high
    const int count = kTileW * kTileH / 40;
    for (int i = 0; i < count; ++i) {
        const int y = static_cast<int>(rnd.next() * kTileH), x0 = static_cast<int>(rnd.next() * kTileW);
        const double u = rnd.next();
        const int len = 16 + static_cast<int>(u * u * 600.0);
        const double amp = (rnd.next() < 0.5 ? -1.0 : 1.0) * (1.0 + rnd.next() * 6.0);
        const bool wide = rnd.next() < 0.25;
        for (int k = 0; k < len; ++k) {
            const double w = std::sin(3.14159265358979 * (k + 0.5) / len); // soft start and end
            at(x0 + k, y) += static_cast<float>(amp * w);
            if (wide) at(x0 + k, y + 1) += static_cast<float>(0.5 * amp * w);
        }
    }
    // brushed: smeared along the rows (two passes of a 5-pixel box), then a little fine noise on top
    std::vector<float> row(kTileW);
    for (int pass = 0; pass < 2; ++pass)
        for (int y = 0; y < kTileH; ++y) {
            for (int x = 0; x < kTileW; ++x) {
                float s = 0.0f;
                for (int d = -2; d <= 2; ++d) s += at(x + d, y);
                row[static_cast<size_t>(x)] = s / 5.0f;
            }
            for (int x = 0; x < kTileW; ++x) at(x, y) = row[static_cast<size_t>(x)];
        }
    for (float& v : t) v += static_cast<float>((rnd.next() - 0.5) * 1.6);
    return t;
}

int channel(double v) { return static_cast<int>(std::clamp(v, 0.0, 255.0) + 0.5); }

Gdiplus::Color gc(int r, int g, int b, int a = 255) {
    return Gdiplus::Color(static_cast<BYTE>(a), static_cast<BYTE>(r), static_cast<BYTE>(g), static_cast<BYTE>(b));
}

void rounded(Gdiplus::GraphicsPath& p, Gdiplus::RectF r, float radius) {
    const float d = std::min(2.0f * radius, std::min(r.Width, r.Height));
    if (d < 1.0f) {
        p.AddRectangle(r);
        return;
    }
    p.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270.0f, 90.0f);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.0f, 90.0f);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90.0f, 90.0f);
    p.CloseFigure();
}

// ---- 3D buttons ----

const wchar_t* const kHot = L"ApThemeHot";
const wchar_t* const kChecked = L"ApThemeChecked";

LRESULT CALLBACK buttonProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (msg) {
    case WM_MOUSEMOVE:
        if (!GetPropW(h, kHot)) {
            SetPropW(h, kHot, reinterpret_cast<HANDLE>(1));
            TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
            TrackMouseEvent(&t);
            InvalidateRect(h, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        RemovePropW(h, kHot);
        InvalidateRect(h, nullptr, FALSE);
        break;
    case WM_LBUTTONDBLCLK: // an owner-drawn button takes a fast second click as a double click: make it a click
        return DefSubclassProc(h, WM_LBUTTONDOWN, wp, lp);
    case WM_NCDESTROY:
        RemovePropW(h, kHot);
        RemovePropW(h, kChecked);
        RemoveWindowSubclass(h, buttonProc, 1);
        break;
    default:
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

void drawButton(const DRAWITEMSTRUCT* di, Metal& metal) {
    const RECT rc = di->rcItem;
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    // drawn off screen: the metal behind the corners, then the button
    HDC dc = CreateCompatibleDC(di->hDC);
    HBITMAP bmp = CreateCompatibleBitmap(di->hDC, w, h);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    const RECT local{0, 0, w, h};
    metal.paint(dc, local, di->hwndItem);

    const bool pressed = (di->itemState & ODS_SELECTED) != 0, disabled = (di->itemState & ODS_DISABLED) != 0;
    const bool checked = isChecked(di->hwndItem), hot = GetPropW(di->hwndItem, kHot) != nullptr && !disabled;
    const bool focus = (di->itemState & ODS_FOCUS) && !(di->itemState & ODS_NOFOCUSRECT);
    const float scale = static_cast<float>(GetDpiForWindow(di->hwndItem)) / 96.0f, radius = 4.0f * scale;
    struct Face {
        int top[3], bottom[3], border[3], text[3];
    };
    Face f{{98, 100, 104}, {50, 52, 56}, {22, 23, 26}, {242, 243, 245}};          // dark metal, light text
    if (hot) f = {{116, 118, 122}, {60, 62, 66}, {22, 23, 26}, {255, 255, 255}};
    if (checked) f = {{112, 204, 72}, {38, 126, 24}, {18, 60, 10}, {255, 255, 255}}; // lit green
    if (pressed) std::swap(f.top, f.bottom);                                         // pushed in: light below
    if (disabled) f = {{150, 152, 156}, {120, 122, 126}, {92, 94, 98}, {196, 198, 202}};

    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const float sh = std::max(1.0f, scale);
        Gdiplus::RectF face(0.5f, 0.5f, static_cast<float>(w) - 1.0f, static_cast<float>(h) - 1.0f - sh);
        if (!pressed) { // shadow under the button
            Gdiplus::GraphicsPath sp;
            rounded(sp, Gdiplus::RectF(face.X, face.Y + sh, face.Width, face.Height), radius);
            Gdiplus::SolidBrush sb(gc(0, 0, 0, 70));
            g.FillPath(&sb, &sp);
        } else {
            face.Y += sh; // pressed: the face sits lower
        }
        Gdiplus::GraphicsPath p;
        rounded(p, face, radius);
        Gdiplus::LinearGradientBrush fill(Gdiplus::RectF(face.X, face.Y - 1.0f, face.Width, face.Height + 2.0f),
                                          gc(f.top[0], f.top[1], f.top[2]), gc(f.bottom[0], f.bottom[1], f.bottom[2]),
                                          Gdiplus::LinearGradientModeVertical);
        g.FillPath(&fill, &p);
        if (!pressed && !disabled) { // light catching the upper edge
            Gdiplus::GraphicsPath hp;
            rounded(hp, Gdiplus::RectF(face.X + 1.0f, face.Y + 1.0f, face.Width - 2.0f, face.Height - 2.0f),
                    std::max(0.0f, radius - 1.0f));
            Gdiplus::LinearGradientBrush hb(Gdiplus::RectF(face.X, face.Y, face.Width, face.Height / 2.0f + 1.0f),
                                            gc(255, 255, 255, 90), gc(255, 255, 255, 0),
                                            Gdiplus::LinearGradientModeVertical);
            Gdiplus::Pen hpen(&hb, 1.0f);
            g.DrawPath(&hpen, &hp);
        }
        Gdiplus::Pen border(gc(f.border[0], f.border[1], f.border[2]), 1.0f);
        g.DrawPath(&border, &p);
        if (focus) { // keyboard focus: a thin light ring inside the border
            Gdiplus::GraphicsPath fp;
            rounded(fp, Gdiplus::RectF(face.X + 2.5f, face.Y + 2.5f, face.Width - 5.0f, face.Height - 5.0f),
                    std::max(0.0f, radius - 2.0f));
            Gdiplus::Pen fpen(gc(255, 255, 255, 150), 1.0f);
            fpen.SetDashStyle(Gdiplus::DashStyleDot);
            g.DrawPath(&fpen, &fp);
        }
    }

    // the caption in the button's own font (the reset arrows use a symbol font)
    const int n = GetWindowTextLengthW(di->hwndItem);
    std::wstring text(static_cast<size_t>(n), L'\0');
    GetWindowTextW(di->hwndItem, text.data(), n + 1);
    HGDIOBJ oldFont = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(di->hwndItem, WM_GETFONT, 0, 0)));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(f.text[0], f.text[1], f.text[2]));
    const int dy = pressed ? static_cast<int>(std::max(1.0f, scale)) : 0;
    RECT tr{0, dy, w, h - static_cast<int>(std::max(1.0f, scale)) + dy};
    DrawTextW(dc, text.c_str(), -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_HIDEPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, oldFont);

    BitBlt(di->hDC, rc.left, rc.top, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

// Paints a control off screen: `draw(dc, client rect)`, then one copy to the screen (no flicker).
template <class Draw> void paintBuffered(HWND h, UINT msg, WPARAM wp, Draw draw) {
    PAINTSTRUCT ps{};
    HDC target = msg == WM_PAINT ? BeginPaint(h, &ps) : reinterpret_cast<HDC>(wp);
    RECT rc;
    GetClientRect(h, &rc);
    if (rc.right > 0 && rc.bottom > 0) {
        HDC dc = CreateCompatibleDC(target);
        HBITMAP bmp = CreateCompatibleBitmap(target, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(dc, bmp);
        draw(dc, rc);
        BitBlt(target, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, old);
        DeleteObject(bmp);
        DeleteDC(dc);
    }
    if (msg == WM_PAINT) EndPaint(h, &ps);
}

// ---- tabs: drawn on the metal, the chosen tab in the field grey and joined to a line under the strip ----

LRESULT CALLBACK tabProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    case WM_PRINTCLIENT:
        paintBuffered(h, msg, wp, [&](HDC dc, const RECT& rc) {
            reinterpret_cast<Metal*>(ref)->paint(dc, rc, h);
            const float scale = static_cast<float>(GetDpiForWindow(h)) / 96.0f;
            const int sel = TabCtrl_GetCurSel(h), n = TabCtrl_GetItemCount(h);
            HGDIOBJ oldFont = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0)));
            SetBkMode(dc, TRANSPARENT);
            Gdiplus::Graphics g(dc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            const float lineH = 3.0f * scale, bottom = static_cast<float>(rc.bottom);
            Gdiplus::SolidBrush line(gc(226, 225, 221));
            g.FillRectangle(&line, 0.0f, bottom - lineH, static_cast<float>(rc.right), lineH);
            for (int pass = 0; pass < 2; ++pass) // the other tabs first, the chosen one on top
                for (int i = 0; i < n; ++i) {
                    if ((i == sel) != (pass == 1)) continue;
                    RECT r;
                    TabCtrl_GetItemRect(h, i, &r);
                    const bool chosen = i == sel;
                    Gdiplus::RectF f(static_cast<float>(r.left) + 0.5f, static_cast<float>(r.top) + (chosen ? 0.5f : 2.5f) * scale,
                                     static_cast<float>(r.right - r.left) - 1.0f, 0.0f);
                    f.Height = bottom - (chosen ? 0.0f : lineH) - f.Y + 4.0f * scale; // runs under the line
                    Gdiplus::GraphicsPath p;
                    rounded(p, f, 4.0f * scale);
                    Gdiplus::SolidBrush fill(chosen ? gc(226, 225, 221) : gc(196, 195, 190));
                    g.FillPath(&fill, &p);
                    Gdiplus::Pen border(gc(112, 111, 107), 1.0f);
                    g.SetClip(Gdiplus::RectF(f.X - 1.0f, f.Y - 1.0f, f.Width + 2.0f, bottom - lineH - f.Y + (chosen ? lineH : 1.0f)));
                    g.DrawPath(&border, &p);
                    g.ResetClip();
                    wchar_t text[64] = {};
                    TCITEMW it{};
                    it.mask = TCIF_TEXT;
                    it.pszText = text;
                    it.cchTextMax = 64;
                    TabCtrl_GetItem(h, i, &it);
                    SetTextColor(dc, kText);
                    RECT tr{r.left, static_cast<LONG>(f.Y), r.right, static_cast<LONG>(bottom - lineH)};
                    DrawTextW(dc, text, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                    if (chosen && GetFocus() == h) { // keyboard focus
                        InflateRect(&tr, -static_cast<int>(3 * scale), -static_cast<int>(2 * scale));
                        DrawFocusRect(dc, &tr);
                    }
                }
            SelectObject(dc, oldFont);
        });
        return 0;
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, tabProc, 1);
        break;
    default:
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

// ---- drop-down lists: the field in grey with a thin border and a chevron ----

LRESULT CALLBACK comboProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    case WM_PRINTCLIENT:
        paintBuffered(h, msg, wp, [&](HDC dc, const RECT& rc) {
            const float scale = static_cast<float>(GetDpiForWindow(h)) / 96.0f;
            const bool enabled = IsWindowEnabled(h) != FALSE;
            const bool focus = GetFocus() == h && !SendMessageW(h, CB_GETDROPPEDSTATE, 0, 0);
            HBRUSH back = CreateSolidBrush(enabled ? kField : kTabBack);
            FillRect(dc, &rc, back);
            DeleteObject(back);
            HBRUSH frame = CreateSolidBrush(focus ? RGB(70, 69, 66) : RGB(112, 111, 107));
            FrameRect(dc, &rc, frame);
            DeleteObject(frame);
            const int arrowW = static_cast<int>(22 * scale);
            // the chosen item's text
            const LRESULT sel = SendMessageW(h, CB_GETCURSEL, 0, 0);
            std::wstring text;
            if (sel >= 0) {
                text.assign(static_cast<size_t>(SendMessageW(h, CB_GETLBTEXTLEN, static_cast<WPARAM>(sel), 0)), L'\0');
                SendMessageW(h, CB_GETLBTEXT, static_cast<WPARAM>(sel), reinterpret_cast<LPARAM>(text.data()));
                text.resize(wcslen(text.c_str()));
            }
            HGDIOBJ oldFont = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0)));
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, enabled ? kText : RGB(110, 109, 105));
            RECT tr{rc.left + static_cast<int>(5 * scale), rc.top, rc.right - arrowW, rc.bottom};
            DrawTextW(dc, text.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            if (focus) {
                RECT fr{rc.left + static_cast<int>(2 * scale), rc.top + static_cast<int>(2 * scale),
                        rc.right - arrowW, rc.bottom - static_cast<int>(2 * scale)};
                DrawFocusRect(dc, &fr);
            }
            SelectObject(dc, oldFont);
            // chevron
            Gdiplus::Graphics g(dc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            const float cx = static_cast<float>(rc.right) - arrowW / 2.0f, cy = (rc.top + rc.bottom) / 2.0f;
            const float a = 4.0f * scale;
            Gdiplus::Pen pen(enabled ? gc(40, 40, 42) : gc(120, 119, 115), 1.5f * scale);
            Gdiplus::PointF pts[3] = {{cx - a, cy - a / 2.0f}, {cx, cy + a / 2.0f}, {cx + a, cy - a / 2.0f}};
            g.DrawLines(&pen, pts, 3);
        });
        return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
    case CB_SETCURSEL: {
        const LRESULT r = DefSubclassProc(h, msg, wp, lp);
        InvalidateRect(h, nullptr, FALSE);
        return r;
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, comboProc, 1);
        break;
    default:
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

bool isEdit(HWND h) {
    wchar_t cls[16] = {};
    GetClassNameW(h, cls, 16);
    return _wcsicmp(cls, L"Edit") == 0;
}

} // namespace

void init() {
    Gdiplus::GdiplusStartupInput in;
    Gdiplus::GdiplusStartup(&g_gdiplus, &in, nullptr);
    strokes(); // the tile once, before the first paint
}

void shutdown() {
    if (g_gdiplus) Gdiplus::GdiplusShutdown(g_gdiplus);
    g_gdiplus = 0;
}

Metal::~Metal() { release(); }

void Metal::release() {
    if (brush_) DeleteObject(brush_);
    if (mem_) {
        SelectObject(mem_, old_);
        DeleteDC(mem_);
    }
    if (bmp_) DeleteObject(bmp_);
    brush_ = nullptr;
    mem_ = nullptr;
    bmp_ = old_ = nullptr;
    w_ = h_ = 0;
}

// The whole client area: gradient + sheen + the stroke tile.
void Metal::render(HWND top) {
    top_ = top;
    RECT rc;
    GetClientRect(top, &rc);
    const int w = std::max(1, static_cast<int>(rc.right)), h = std::max(1, static_cast<int>(rc.bottom));
    if (mem_ && w == w_ && h == h_) return;
    release();
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    void* bits = nullptr;
    HDC screen = GetDC(top);
    bmp_ = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    mem_ = CreateCompatibleDC(screen);
    ReleaseDC(top, screen);
    if (!bmp_ || !mem_ || !bits) {
        release();
        return;
    }
    const std::vector<float>& tile = strokes();
    uint32_t* px = static_cast<uint32_t*>(bits);
    std::vector<float> sheen(static_cast<size_t>(w));
    for (int x = 0; x < w; ++x) {
        const double u = std::fabs(2.0 * (x + 0.5) / w - 1.0);
        sheen[static_cast<size_t>(x)] = static_cast<float>(kSheen * (1.0 - std::pow(u, 1.6)));
    }
    for (int y = 0; y < h; ++y) {
        const double t = h > 1 ? static_cast<double>(y) / (h - 1) : 0.0;
        auto part = [t](int shift) { // one colour channel of the gradient
            const double a = (kMetalTop >> shift) & 0xFF, z = (kMetalBottom >> shift) & 0xFF;
            return a + (z - a) * t;
        };
        const double r = part(0), g = part(8), b = part(16);
        const float* row = &tile[static_cast<size_t>(y % kTileH) * kTileW];
        uint32_t* out = px + static_cast<size_t>(y) * w;
        for (int x = 0; x < w; ++x) {
            const double v = row[x % kTileW] + sheen[static_cast<size_t>(x)];
            out[x] = static_cast<uint32_t>(channel(r + v) << 16 | channel(g + v) << 8 | channel(b + v));
        }
    }
    brush_ = CreatePatternBrush(bmp_); // before the bitmap is selected into the memory DC
    old_ = static_cast<HBITMAP>(SelectObject(mem_, bmp_));
    w_ = w;
    h_ = h;
}

void Metal::paint(HDC dc, const RECT& r, HWND child) {
    HWND top = GetAncestor(child, GA_ROOT);
    if (!mem_) render(top);
    if (!mem_) {
        FillRect(dc, &r, GetSysColorBrush(COLOR_BTNFACE));
        return;
    }
    POINT o{0, 0};
    MapWindowPoints(child, top, &o, 1);
    BitBlt(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, mem_, r.left + o.x, r.top + o.y, SRCCOPY);
}

HBRUSH Metal::controlBrush(HDC dc, HWND child) {
    if (!mem_) render(GetAncestor(child, GA_ROOT));
    POINT o{0, 0};
    MapWindowPoints(child, GetAncestor(child, GA_ROOT), &o, 1);
    SetBrushOrgEx(dc, -o.x, -o.y, nullptr); // the pattern lines up with the window behind the control
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, kText);
    return brush_ ? brush_ : GetSysColorBrush(COLOR_BTNFACE);
}

bool Metal::handle(HWND top, UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) {
    switch (msg) {
    case WM_SIZE:
        render(top);
        return false; // the window lays itself out too
    case WM_ERASEBKGND:
    case WM_PRINTCLIENT: {
        RECT rc;
        GetClientRect(top, &rc);
        paint(reinterpret_cast<HDC>(wp), rc, top);
        result = 1;
        return true;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        if (!isEdit(reinterpret_cast<HWND>(lp))) {
            result = reinterpret_cast<LRESULT>(controlBrush(reinterpret_cast<HDC>(wp), reinterpret_cast<HWND>(lp)));
            return true;
        }
        [[fallthrough]]; // a read-only or disabled edit field: a field like the others
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetBkColor(dc, kField);
        SetTextColor(dc, kText);
        result = reinterpret_cast<LRESULT>(fieldBrush());
        return true;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* di = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
        if (di->CtlType != ODT_BUTTON) return false;
        drawButton(di, *this);
        result = TRUE;
        return true;
    }
    default:
        return false;
    }
}

void makeButton(HWND button) {
    if (button) SetWindowSubclass(button, buttonProc, 1, 0);
}

void Metal::makeTabs(HWND tabs) {
    if (tabs) SetWindowSubclass(tabs, tabProc, 1, reinterpret_cast<DWORD_PTR>(this));
}

void makeCombo(HWND combo) {
    if (combo) SetWindowSubclass(combo, comboProc, 1, 0);
}

void setChecked(HWND button, bool on) {
    if (isChecked(button) == on) return;
    if (on) SetPropW(button, kChecked, reinterpret_cast<HANDLE>(1));
    else RemovePropW(button, kChecked);
    InvalidateRect(button, nullptr, FALSE);
}

bool isChecked(HWND button) { return GetPropW(button, kChecked) != nullptr; }

} // namespace ap::theme
