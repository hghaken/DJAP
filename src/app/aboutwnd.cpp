#include "aboutwnd.h"

#include "appstate.h"
#include "placement.h"
#include "resource.h"
#include "version.h"

#include <algorithm>
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
const int kWidth = 560, kHeight = 380;

} // namespace

// The wide logo from the exe's resources (RCDATA, PNG), decoded by GDI+; its stream lives as long as the image.
void AboutWindow::loadLogo() {
    if (logo_) return;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(inst, MAKEINTRESOURCEW(IDR_LOGO), RT_RCDATA);
    HGLOBAL data = res ? LoadResource(inst, res) : nullptr;
    const DWORD size = res ? SizeofResource(inst, res) : 0;
    const void* bytes = data ? LockResource(data) : nullptr;
    if (!bytes || !size) return;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!mem) return;
    std::memcpy(GlobalLock(mem), bytes, size);
    GlobalUnlock(mem);
    if (FAILED(CreateStreamOnHGlobal(mem, TRUE, &stream_))) {
        GlobalFree(mem);
        return;
    }
    logo_ = Gdiplus::Bitmap::FromStream(stream_);
    if (logo_ && logo_->GetLastStatus() != Gdiplus::Ok) releaseLogo();
}

void AboutWindow::releaseLogo() {
    delete logo_;
    logo_ = nullptr;
    if (stream_) stream_->Release();
    stream_ = nullptr;
}

void AboutWindow::show(HWND owner, const std::filesystem::path& settingsFile) {
    if (hwnd_) {
        SetForegroundWindow(hwnd_);
        return;
    }
    owner_ = owner;
    body_ = L"DJAP (DJ Audio Processor) " + fromUtf8(AP_VERSION_STRING) +
            L"\n\nTwo-deck loudness and dynamics processor for DJs: a leveller, compressor, density, de-esser, "
            L"stereo width, colour and a true-peak limiter per deck, with only the limiter's look-ahead as delay.";
    path_ = L"Settings and own loudness presets and colours: " + settingsFile.wstring();
    loadLogo();
    static bool registered = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDI_APP));
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kClass;
        registered = RegisterClassExW(&wc) != 0;
    }
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    hwnd_ = CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, L"About DJAP", style, CW_USEDEFAULT, CW_USEDEFAULT,
                            CW_USEDEFAULT, CW_USEDEFAULT, owner, nullptr, inst, this);
    if (!hwnd_) return;
    // sized for the window's DPI, at its remembered place (or centred on the screen)
    RECT r{0, 0, px(kWidth), px(kHeight)};
    AdjustWindowRectExForDpi(&r, style, FALSE, WS_EX_DLGMODALFRAME, dpi_);
    SetWindowPos(hwnd_, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
    placement::remember(hwnd_, "About", owner);
    EnableWindow(owner_, FALSE); // modal, as a message box
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
    HINSTANCE inst = GetModuleHandleW(nullptr);
    const int x = px(20), w = px(kWidth - 40), top = px(20 + kLogoH + 16);
    auto label = [&](const std::wstring& s, DWORD style, int y, int h) {
        return CreateWindowExW(0, L"STATIC", s.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | style, x,
                               top + px(y), w, px(h), hwnd_, nullptr, inst, nullptr);
    };
    text_ = label(body_, 0, 0, 74);
    pathText_ = label(path_, SS_PATHELLIPSIS, 80, 18); // one line, shortened in the middle when long
    legal_ = label(L"Licence: GPL-3.0; licences of components: THIRD_PARTY.md.\n"
                   L"ASIO is a trademark and software of Steinberg Media Technologies GmbH.",
                   0, 104, 36);
    ok_ = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                          px(kWidth - 20 - 96), px(kHeight - 20 - 30), px(96), px(30), hwnd_,
                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), inst, nullptr);
    theme::makeButton(ok_);
}

void AboutWindow::setFonts() {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi_);
    font_ = CreateFontIndirectW(&ncm.lfMessageFont);
    for (HWND h : {text_, pathText_, legal_}) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    SendMessageW(ok_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
}

void AboutWindow::close() {
    EnableWindow(owner_, TRUE); // before the window goes, so the owner gets the activation back
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
        if (logo_) { // over the metal: its rounded corners are transparent
            Gdiplus::Graphics g(dc);
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
            g.DrawImage(logo_, Gdiplus::Rect(px(20), px(20), px(kLogoW), px(kLogoH)));
        }
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_COMMAND:
        // OK, Enter, Escape; only clicks (an owner-drawn button also sends BN_SETFOCUS with its id)
        if ((LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) && HIWORD(wp) == BN_CLICKED) close();
        return 0;
    case WM_CLOSE:
        close();
        return 0;
    case WM_DESTROY: // (also when the main window goes first: before GDI+ shuts down in main)
        if (font_) DeleteObject(font_);
        font_ = nullptr;
        releaseLogo();
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

} // namespace ap
