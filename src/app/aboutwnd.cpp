// SPDX-License-Identifier: GPL-3.0-or-later
#include "aboutwnd.h"

#include "appstate.h"
#include "placement.h"
#include "resource.h"
#include "version.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include <objidl.h>
namespace Gdiplus {
using std::max;
using std::min;
} // namespace Gdiplus
#include <gdiplus.h>

namespace ap {

namespace {

const wchar_t* const kClass = L"APAboutWindow";
const int kLogoW = 520, kLogoH = 130; // the wide logo (assets/logo.png, 4:1) at 100 %
const int kWidth = 560, kHeight = 484;
enum { idCheckNow = 201, idInstall, idLink, idDisclaimer };

// The disclaimer (client, 2026-10-28; the same in substance as the README's and the manuals')
const wchar_t* const kDisclaimer =
    L"DJAP is provided \"as is\", without warranty of any kind, and you use it at your own risk (GNU GPL, sections 15 "
    L"and 16).\n\n"
    L"It can make sound louder than its source: start with the volume low, and never set levels that can damage "
    L"hearing, speakers or other equipment. The authors are not liable for any damage to hearing, equipment, "
    L"recordings, data or settings, or for loss of income - for example from a dropout during a performance.\n\n"
    L"Its meters follow the published loudness standards but are not a certified measuring instrument.\n\n"
    L"The code was written with an AI assistant and tested, but it may still contain errors. Third-party software such "
    L"as a virtual audio cable is used under its own licence and at your own responsibility.\n\n"
    L"DJAP is not affiliated with, endorsed or sponsored by any company or product it names; such names only identify "
    L"what DJAP works with.";
const UINT_PTR kAnimTimer = 1;
const UINT kAnimMs = 30;
const UINT kCreditsMsg = WM_APP + 1;
const double kPulseSeconds = 20.0, kPulseGrow = 0.06; // the logo up to 6 % larger on a beat
const double kCreditsSpeed = 0.9;                       // px (at 96 dpi) per tick

const wchar_t* const kCredits[] = {
    L"DJAP",
    L"DJ Audio Processor",
    L"",
    L"Idea, ears and every decision",
    L"hghaken",
    L"",
    L"DSP, code and manuals",
    L"Claude",
    L"",
    L"Outside reviews",
    L"Grok",
    L"",
    L"25 October 2026 - the name DJAP",
    L"26 October 2026 - the plug-in",
    L"27 October 2026 - drift correction, the scope",
    L"28 October 2026 - reviewed, tuned, released",
    L"",
    L"Thanks for listening.",
};

} // namespace

bool AboutWindow::takeKey(const MSG& m) {
    if (m.message != WM_KEYDOWN || m.wParam != 'D' || GetKeyState(VK_CONTROL) >= 0 || GetKeyState(VK_SHIFT) >= 0)
        return false;
    HWND active = GetActiveWindow();
    wchar_t cls[64] = {};
    if (!active || !GetClassNameW(active, cls, 64) || wcscmp(cls, kClass) != 0) return false;
    if (!(m.lParam & (1 << 30))) PostMessageW(active, kCreditsMsg, 0, 0); // not the key's auto-repeat
    return true;
}

LRESULT CALLBACK AboutWindow::keyHook(int code, WPARAM wp, LPARAM lp) {
    MSG* m = reinterpret_cast<MSG*>(lp);
    if (code == HC_ACTION && wp == PM_REMOVE && takeKey(*m)) m->message = WM_NULL; // taken: the host sees nothing
    return CallNextHookEx(nullptr, code, wp, lp);
}

RECT AboutWindow::topRect() const { return RECT{0, 0, px(kWidth), px(20 + kLogoH + 12)}; }

// Deck A's bass against its own average: a low-pass at 120 Hz, its power over 25 ms against over 1 s - a kick drum
// stands out, a steady level does not (the output's peaks sit at the limiter's ceiling all the time).
double AboutWindow::readBeat() {
    const WaveScope* s = beat_ ? beat_() : nullptr;
    if (!s || s->fs <= 0.0) return 0.0;
    const long long end = s->rawWritten;
    long long from = beatRead_ < 0 || beatRead_ > end ? end : beatRead_;
    from = std::max(from, end - WaveScope::kRaw / 2);
    const double pi = 3.14159265358979;
    const double aLp = 1.0 - std::exp(-2.0 * pi * 120.0 / s->fs), aFast = 1.0 - std::exp(-1.0 / (0.025 * s->fs)),
                 aSlow = 1.0 - std::exp(-1.0 / (1.0 * s->fs));
    for (long long i = from; i < end; ++i) {
        lp_ += aLp * (0.5 * (s->sample(0, i) + s->sample(1, i)) - lp_);
        const double e = lp_ * lp_;
        fast_ += aFast * (e - fast_);
        slow_ += aSlow * (e - slow_);
    }
    beatRead_ = end;
    if (slow_ < 1e-8) return 0.0; // silence
    return std::clamp((fast_ / slow_ - 1.2) / 1.5, 0.0, 1.0);
}

void AboutWindow::animate() {
    const ULONGLONG now = GetTickCount64();
    const double beat = now < pulseUntil_ ? readBeat() : 0.0;
    pulse_ = std::max(beat, pulse_ * 0.82); // up at once, down over about 150 ms
    if (credits_) {
        creditsY_ += kCreditsSpeed * dpi_ / 96.0;
        const RECT r = topRect();
        const double lines = static_cast<double>(sizeof kCredits / sizeof kCredits[0]);
        if (creditsY_ > (r.bottom - r.top) + lines * px(22)) credits_ = false; // all lines passed
    }
    const RECT r = topRect();
    InvalidateRect(hwnd_, &r, FALSE);
    if (!credits_ && now >= pulseUntil_ && pulse_ < 0.001) {
        pulse_ = 0.0;
        KillTimer(hwnd_, kAnimTimer);
        animating_ = false;
    }
}

void AboutWindow::paintTop(HDC target) {
    const RECT r = topRect();
    const int w = r.right - r.left, h = r.bottom - r.top;
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, w, h);
    HGDIOBJ old = SelectObject(dc, bmp);
    metal_.paint(dc, r, hwnd_);
    {
        Gdiplus::Graphics g(dc);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const double grow = 1.0 + kPulseGrow * pulse_;
        const double lw = px(kLogoW) * grow, lh = px(kLogoH) * grow;
        const double cx = px(20) + px(kLogoW) / 2.0, cy = px(20) + px(kLogoH) / 2.0;
        if (logo_ && !credits_)
            g.DrawImage(logo_, Gdiplus::RectF(static_cast<Gdiplus::REAL>(cx - lw / 2), static_cast<Gdiplus::REAL>(cy - lh / 2),
                                              static_cast<Gdiplus::REAL>(lw), static_cast<Gdiplus::REAL>(lh)));
        if (credits_) { // a dark plate in the logo's place and shape, the lines rolling up through it
            const Gdiplus::REAL x0 = static_cast<Gdiplus::REAL>(px(20)), y0 = static_cast<Gdiplus::REAL>(px(20)),
                                w0 = static_cast<Gdiplus::REAL>(px(kLogoW)), h0 = static_cast<Gdiplus::REAL>(px(kLogoH)),
                                d = static_cast<Gdiplus::REAL>(px(28)); // corner diameter
            Gdiplus::GraphicsPath plate;
            plate.AddArc(x0, y0, d, d, 180, 90);
            plate.AddArc(x0 + w0 - d, y0, d, d, 270, 90);
            plate.AddArc(x0 + w0 - d, y0 + h0 - d, d, d, 0, 90);
            plate.AddArc(x0, y0 + h0 - d, d, d, 90, 90);
            plate.CloseFigure();
            Gdiplus::SolidBrush shade(Gdiplus::Color(245, 22, 24, 28));
            g.FillPath(&shade, &plate);
            Gdiplus::Pen rim(Gdiplus::Color(255, 90, 92, 96), static_cast<Gdiplus::REAL>(px(1.5)));
            g.DrawPath(&rim, &plate);
            g.SetClip(&plate);
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
            Gdiplus::FontFamily family(L"Segoe UI");
            Gdiplus::Font title(&family, static_cast<Gdiplus::REAL>(px(15)), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::Font line(&family, static_cast<Gdiplus::REAL>(px(13)), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            Gdiplus::StringFormat centre;
            centre.SetAlignment(Gdiplus::StringAlignmentCenter);
            Gdiplus::SolidBrush bright(Gdiplus::Color(255, 235, 235, 235)), gold(Gdiplus::Color(255, 240, 200, 90));
            const int n = static_cast<int>(sizeof kCredits / sizeof kCredits[0]);
            for (int i = 0; i < n; ++i) {
                const double y = px(20) + px(kLogoH) - creditsY_ + i * px(22);
                if (y < px(20) - px(22) || y > px(20) + px(kLogoH)) continue;
                // names in gold, the rest white; the first line as the title
                const bool name = i > 0 && kCredits[i - 1][0] != L'\0' && kCredits[i][0] != L'\0' && i < 12 && (i % 3 == 1);
                g.DrawString(kCredits[i], -1, i == 0 ? &title : &line,
                             Gdiplus::PointF(static_cast<Gdiplus::REAL>(cx), static_cast<Gdiplus::REAL>(y)), &centre,
                             name || i == 0 ? &gold : &bright);
            }
        }
    }
    BitBlt(target, r.left, r.top, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
}

namespace {
// The module the window class and the logo belong to: the program, or the plug-in DLL (AboutWindow::setModule).
HINSTANCE g_module = nullptr;
bool g_registered = false;
HINSTANCE moduleInstance() { return g_module ? g_module : GetModuleHandleW(nullptr); }
} // namespace

void AboutWindow::setModule(HINSTANCE instance) { g_module = instance; }

void AboutWindow::unregisterClass() {
    if (g_registered) UnregisterClassW(kClass, moduleInstance());
    g_registered = false;
}

namespace {
// A PNG from the module's resources (RCDATA), decoded by GDI+; its stream lives as long as the image. Nothing when the
// module has no such resource (the plug-in has no ASIO logo).
void loadPng(int id, IStream*& stream, Gdiplus::Bitmap*& image) {
    if (image) return;
    HINSTANCE inst = moduleInstance();
    HRSRC res = FindResourceW(inst, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL data = res ? LoadResource(inst, res) : nullptr;
    const DWORD size = res ? SizeofResource(inst, res) : 0;
    const void* bytes = data ? LockResource(data) : nullptr;
    if (!bytes || !size) return;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!mem) return;
    std::memcpy(GlobalLock(mem), bytes, size);
    GlobalUnlock(mem);
    if (FAILED(CreateStreamOnHGlobal(mem, TRUE, &stream))) {
        GlobalFree(mem);
        return;
    }
    image = Gdiplus::Bitmap::FromStream(stream);
    if (image && image->GetLastStatus() != Gdiplus::Ok) {
        delete image;
        image = nullptr;
        stream->Release();
        stream = nullptr;
    }
}

void releasePng(IStream*& stream, Gdiplus::Bitmap*& image) {
    delete image;
    image = nullptr;
    if (stream) stream->Release();
    stream = nullptr;
}
} // namespace

void AboutWindow::loadLogo() {
    loadPng(IDR_LOGO, stream_, logo_);
    loadPng(plugin_ ? IDR_VST_LOGO : IDR_ASIO_LOGO, badgeStream_, badge_);
}

void AboutWindow::releaseLogo() {
    releasePng(stream_, logo_);
    releasePng(badgeStream_, badge_);
}

RECT AboutWindow::badgeRect() const {
    // its own proportions (474 x 443), 80 px high (21 mm at 100 %: the VST guidelines' minimum is 20 x 18.7 mm), at
    // the right edge beside the four licence lines
    const int h = px(80), w = badge_ ? MulDiv(h, static_cast<int>(badge_->GetWidth()), static_cast<int>(badge_->GetHeight())) : 0;
    const int top = px(20 + kLogoH + 16 + 102);
    return RECT{px(kWidth - 20) - w, top, px(kWidth - 20), top + h};
}

void AboutWindow::show(HWND owner, const std::filesystem::path& settingsFile) {
    if (hwnd_) {
        SetForegroundWindow(hwnd_);
        return;
    }
    owner_ = owner;
    body_ = plugin_ ? L"DJAP (DJ Audio Processor) " + fromUtf8(AP_VERSION_STRING) +
                          L" - plug-in\n\nOne deck of DJAP inside DJ software: a leveller, compressor, density, de-esser, "
                          L"stereo width, colour and a true-peak limiter, with only the limiter's look-ahead as delay."
                    : L"DJAP (DJ Audio Processor) " + fromUtf8(AP_VERSION_STRING) +
                          L"\n\nTwo-deck loudness and dynamics processor for DJs: a leveller, compressor, density, "
                          L"de-esser, stereo width, colour and a true-peak limiter per deck, with only the limiter's "
                          L"look-ahead as delay.";
    path_ = L"Settings and own loudness presets and colours: " + settingsFile.wstring();
    loadLogo();
    HINSTANCE inst = moduleInstance();
    if (!g_registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDI_APP));
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kClass;
        g_registered = RegisterClassExW(&wc) != 0;
    }
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    hwnd_ = CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, L"About DJAP", style, CW_USEDEFAULT, CW_USEDEFAULT,
                            CW_USEDEFAULT, CW_USEDEFAULT, owner, nullptr, inst, this);
    if (!hwnd_) return;
    // sized for the window's DPI, at its remembered place (or centred on the screen)
    RECT r{0, 0, px(kWidth), px(kHeight)};
    AdjustWindowRectExForDpi(&r, style, FALSE, WS_EX_DLGMODALFRAME, dpi_);
    SetWindowPos(hwnd_, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
    refresh();
    placement::remember(hwnd_, plugin_ ? "PluginAbout" : "About", owner);
    // modal, as a message box - not in the plug-in: it would block the DJ software's own window
    if (!plugin_) EnableWindow(owner_, FALSE);
    if (plugin_ && !keyHook_) keyHook_ = SetWindowsHookExW(WH_GETMESSAGE, keyHook, nullptr, GetCurrentThreadId());
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetFocus(ok_);
}

LRESULT CALLBACK AboutWindow::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    AboutWindow* self = reinterpret_cast<AboutWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<AboutWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    const LRESULT r = self->handle(msg, wp, lp);
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        self->hwnd_ = nullptr;
    }
    return r;
}

void AboutWindow::create() {
    HINSTANCE inst = moduleInstance();
    const int x = px(20), w = px(kWidth - 40), top = px(20 + kLogoH + 16);
    auto label = [&](const std::wstring& s, DWORD style, int y, int h) {
        return CreateWindowExW(0, L"STATIC", s.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | style, x,
                               top + px(y), w, px(h), hwnd_, nullptr, inst, nullptr);
    };
    text_ = label(body_, 0, 0, 74);
    pathText_ = label(path_, SS_PATHELLIPSIS, 80, 18); // one line, shortened in the middle when long
    // the copyright, the licence and its "no warranty" (the GPL's "Appropriate Legal Notices"), and Steinberg's
    // attribution beside its logo - ASIO in the program, VST in the plug-in (badgeRect; licence reviews, 2026-10-28)
    legal_ = label(plugin_ ? L"Copyright (C) 2026 hghaken. Free software under the GPL-3.0\n"
                             L"or later (LICENSE.txt), with ABSOLUTELY NO WARRANTY.\n"
                             L"Components: THIRD_PARTY.md. VST is a registered trademark\n"
                             L"of Steinberg Media Technologies GmbH."
                           : L"Copyright (C) 2026 hghaken. Free software under the GPL-3.0\n"
                             L"or later (LICENSE.txt), with ABSOLUTELY NO WARRANTY.\n"
                             L"Components: THIRD_PARTY.md. ASIO is a registered trademark\n"
                             L"of Steinberg Media Technologies GmbH.",
                   0, 104, 72);
    SetWindowPos(legal_, nullptr, 0, 0, w - px(100), px(72), SWP_NOMOVE | SWP_NOZORDER); // (the logo to its right)
    // the updates, as in the Setup window
    auto button = [&](const wchar_t* text, int id, int bx) {
        HWND b = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, bx, top + px(192),
                                 px(110), px(30), hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
        theme::makeButton(b);
        return b;
    };
    checkNow_ = button(L"Check now", idCheckNow, x);
    install_ = button(plugin_ ? L"Download" : L"Install update", idInstall, x + px(118));
    updateText_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | SS_NOTIFY,
                                  x + px(240), top + px(190), w - px(240), px(36), hwnd_, nullptr, inst, nullptr);
    updateShown_.clear();
    // seventh outside review: the update buttons had no tooltips, and a long message was cut off (now whole in its own)
    tips_.create(hwnd_);
    tips_.setDpi(dpi_);
    tips_.add(checkNow_, L"Asks GitHub now whether a newer DJAP is there. Nothing is downloaded or installed.");
    tips_.add(install_, plugin_ ? L"Opens the newer DJAP's page on GitHub in your web browser: download its setup "
                                  L"program there and run it (the plug-in is replaced when the DJ software has let go "
                                  L"of it - at the latest after a restart of the computer)."
                                : L"Downloads the newer DJAP, checks it against GitHub's checksum and installs it. DJAP "
                                  L"closes for this and starts again by itself; running audio starts again too.");
    // the program's GitHub page (client, 2026-10-28): opens in the default browser
    const std::wstring url = fromUtf8(Updater::kRepo);
    link_ = CreateWindowExW(0, WC_LINK, (L"Source, releases and manuals: <a href=\"" + url + L"\">" + url + L"</a>").c_str(),
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LWS_TRANSPARENT, x, top + px(240), w, px(20), hwnd_,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(idLink)), inst, nullptr);
    tips_.add(link_, L"Opens DJAP's page on GitHub in your web browser: the source code, every release with its setup "
                     L"program, and the manuals.");
    ok_ = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                          px(kWidth - 20 - 96), px(kHeight - 20 - 30), px(96), px(30), hwnd_,
                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), inst, nullptr);
    theme::makeButton(ok_);
    disclaimer_ = CreateWindowExW(0, L"BUTTON", L"Disclaimer", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, x,
                                  px(kHeight - 20 - 30), px(110), px(30), hwnd_,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(idDisclaimer)), inst, nullptr);
    theme::makeButton(disclaimer_);
    tips_.add(disclaimer_, L"Shows the disclaimer: DJAP comes without warranty, you use it at your own risk - mind your "
                           L"hearing and your equipment.");
}

void AboutWindow::setFonts() {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi_);
    font_ = CreateFontIndirectW(&ncm.lfMessageFont);
    for (HWND h : {text_, pathText_, legal_, updateText_, checkNow_, install_, link_, disclaimer_, ok_})
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
}

void AboutWindow::refresh() {
    if (!hwnd_ || !view_) return;
    const UpdateView u = view_();
    if (u.text != updateShown_) {
        updateShown_ = u.text;
        SetWindowTextW(updateText_, u.text.c_str());
        tips_.add(updateText_, u.text.empty() ? L"What the last update check found." : u.text.c_str());
    }
    EnableWindow(checkNow_, u.canCheck);
    EnableWindow(install_, u.canInstall);
}

void AboutWindow::close() {
    if (!hwnd_) return;
    if (!plugin_) EnableWindow(owner_, TRUE); // before the window goes, so the owner gets the activation back
    DestroyWindow(hwnd_);
}

LRESULT AboutWindow::handle(UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT themed = 0;
    if (metal_.handle(hwnd_, msg, wp, lp, themed)) return themed; // brushed aluminium, 3D button
    switch (msg) {
    case WM_CREATE:
        dpi_ = GetDpiForWindow(hwnd_);
        create();
        setFonts();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd_, &ps);
        paintTop(dc); // the logo over the metal (its rounded corners are transparent), drawn off screen
        if (badge_) { // unchanged, at its own proportions
            const RECT a = badgeRect();
            Gdiplus::Graphics g(dc);
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
            g.DrawImage(badge_, static_cast<INT>(a.left), static_cast<INT>(a.top), static_cast<INT>(a.right - a.left),
                        static_cast<INT>(a.bottom - a.top));
        }
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const RECT logo{px(20), px(20), px(20 + kLogoW), px(20 + kLogoH)};
        if (PtInRect(&logo, pt)) {
            const ULONGLONG now = GetTickCount64();
            if (logoClicks_ == 0 || now - firstClick_ > 3000) {
                logoClicks_ = 0;
                firstClick_ = now;
            }
            if (++logoClicks_ >= 5) {
                logoClicks_ = 0;
                pulseUntil_ = now + static_cast<ULONGLONG>(kPulseSeconds * 1000.0);
                beatRead_ = -1;
                if (!animating_) animating_ = SetTimer(hwnd_, kAnimTimer, kAnimMs, nullptr) != 0;
            }
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == kAnimTimer) animate();
        return 0;
    case WM_NOTIFY: { // the GitHub link: a click or Enter opens it in the default browser
        const NMHDR* h = reinterpret_cast<const NMHDR*>(lp);
        if (h->idFrom == idLink && (h->code == NM_CLICK || h->code == NM_RETURN)) {
            const NMLINK* l = reinterpret_cast<const NMLINK*>(lp);
            ShellExecuteW(hwnd_, L"open", l->item.szUrl, nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        break;
    }
    case WM_COMMAND:
        // OK, Enter, Escape; only clicks (an owner-drawn button also sends BN_SETFOCUS with its id)
        if ((LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) && HIWORD(wp) == BN_CLICKED) close();
        if (LOWORD(wp) == idCheckNow && HIWORD(wp) == BN_CLICKED && checkNowDo_) checkNowDo_();
        if (LOWORD(wp) == idInstall && HIWORD(wp) == BN_CLICKED && installDo_) installDo_();
        if (LOWORD(wp) == idDisclaimer && HIWORD(wp) == BN_CLICKED) {
            placement::messageBox(hwnd_, kDisclaimer, L"DJAP - Disclaimer", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        refresh();
        return 0;
    case WM_CLOSE:
        close();
        return 0;
    case WM_DESTROY: // (also when the main window goes first: before GDI+ shuts down in main)
        KillTimer(hwnd_, kAnimTimer);
        if (keyHook_) UnhookWindowsHookEx(keyHook_); // before the DLL can go
        keyHook_ = nullptr;
        animating_ = credits_ = false;
        pulseUntil_ = 0;
        pulse_ = 0.0;
        logoClicks_ = 0;
        if (font_) DeleteObject(font_);
        font_ = nullptr;
        releaseLogo();
        return 0;
    default:
        if (msg == kCreditsMsg) { // Ctrl+Shift+D (takeKey)
            credits_ = !credits_;
            creditsY_ = 0.0;
            if (!animating_) animating_ = SetTimer(hwnd_, kAnimTimer, kAnimMs, nullptr) != 0;
            const RECT r = topRect();
            InvalidateRect(hwnd_, &r, FALSE);
            return 0;
        }
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

} // namespace ap
