// SPDX-License-Identifier: GPL-3.0-or-later
#include "soundwnd.h"

#include "appstate.h"
#include "editwnd.h"
#include "placement.h"
#include "resource.h"

#include <commctrl.h>

#include <algorithm>

namespace ap {

namespace {

const wchar_t* const kClass = L"APSoundWindow";
HINSTANCE g_module = nullptr;
bool g_registered = false;
HINSTANCE module() { return g_module ? g_module : GetModuleHandleW(nullptr); }
enum Id { idSound = 10, idSaveAs, idRename, idDelete, idReset, idPart0 = 20, idPart1, idEdit0 = 30, idEdit1 };
const PresetKind kKind[2] = {PresetKind::Dynamics, PresetKind::Colour};

} // namespace

void SoundWindow::setModule(HINSTANCE instance) { g_module = instance; }

void SoundWindow::unregisterClass() {
    if (g_registered) UnregisterClassW(kClass, module());
    g_registered = false;
}

SoundWindow::SoundWindow(Settings& settings, Changed onChange, OpenEditor openEditor)
    : settings_(settings), onChange_(std::move(onChange)), openEditor_(std::move(openEditor)) {}

SoundWindow::~SoundWindow() {
    if (hwnd_) DestroyWindow(hwnd_);
}

void SoundWindow::show(HWND owner, const std::string& sound, int deck) {
    deck_ = deck;
    owner_ = owner;
    if (!hwnd_) {
        HINSTANCE inst = module();
        if (!g_registered) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof wc;
            wc.lpfnWndProc = proc;
            wc.hInstance = inst;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
            wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
            wc.lpszClassName = kClass;
            g_registered = RegisterClassExW(&wc) != 0;
        }
        hwnd_ = CreateWindowExW(0, kClass, L"Sound", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                CW_USEDEFAULT, CW_USEDEFAULT, owner, nullptr, inst, this);
        if (!hwnd_) return;
        RECT r{0, 0, px(760), px(330)};
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi_);
        SetWindowPos(hwnd_, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
        placement::remember(hwnd_, "Sound", owner);
    }
    select(sound);
    ShowWindow(hwnd_, placement::showCommand(hwnd_));
    SetForegroundWindow(hwnd_);
}

void SoundWindow::refresh() {
    if (!hwnd_) return;
    if (!current_.empty() && !settings_.findSound(current_)) current_ = settings_.deck[deck_].sound;
    fill();
}

void SoundWindow::select(const std::string& sound) {
    current_ = settings_.findSound(sound) ? sound : std::string();
    fill();
}

LRESULT CALLBACK SoundWindow::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SoundWindow* self = reinterpret_cast<SoundWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<SoundWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
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

LRESULT SoundWindow::handle(UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT themed = 0;
    if (metal_.handle(hwnd_, msg, wp, lp, themed)) return themed; // brushed aluminium, 3D buttons
    switch (msg) {
    case WM_CREATE:
        dpi_ = GetDpiForWindow(hwnd_);
        createControls();
        setFonts();
        return 0;
    case WM_SIZE:
        layout();
        return 0;
    case WM_GETMINMAXINFO: {
        RECT r{0, 0, px(640), px(330)};
        AdjustWindowRectExForDpi(&r, static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE)), FALSE,
                                 static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE)), dpi_);
        reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {r.right - r.left, r.bottom - r.top};
        return 0;
    }
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wp);
        setFonts();
        tips_.setDpi(dpi_);
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
        layout();
        return 0;
    }
    case WM_COMMAND:
        command(LOWORD(wp), HIWORD(wp));
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd_, &ps);
        RECT line = line_;
        DrawEdge(dc, &line, EDGE_ETCHED, BF_TOP);
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        for (HFONT f : {font_, bold_})
            if (f) DeleteObject(f);
        font_ = bold_ = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void SoundWindow::createControls() {
    HINSTANCE inst = module();
    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
        return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hwnd_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
    };
    soundLabel_ = make(L"STATIC", L"Sound", SS_LEFT | SS_CENTERIMAGE, 0);
    sound_ = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, idSound);
    saveAs_ = make(L"BUTTON", L"Save as...", BS_OWNERDRAW | WS_TABSTOP, idSaveAs);
    rename_ = make(L"BUTTON", L"Rename...", BS_OWNERDRAW | WS_TABSTOP, idRename);
    delete_ = make(L"BUTTON", L"Delete", BS_OWNERDRAW | WS_TABSTOP, idDelete);
    reset_ = make(L"BUTTON", L"Reset sound", BS_OWNERDRAW | WS_TABSTOP, idReset);
    note_ = make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 0);
    for (int k = 0; k < 2; ++k) {
        partLabel_[k] = make(L"STATIC", k == 0 ? L"Loudness" : L"EQ", SS_LEFT | SS_CENTERIMAGE, 0);
        part_[k] = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, idPart0 + k);
        edit_[k] = make(L"BUTTON", L"Edit...", BS_OWNERDRAW | WS_TABSTOP, idEdit0 + k);
        about_[k] = make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 0);
        theme::makeCombo(part_[k]);
    }
    for (HWND b : {saveAs_, rename_, delete_, reset_, edit_[0], edit_[1]}) theme::makeButton(b);
    theme::makeCombo(sound_);
    tips_.create(hwnd_);
    tips_.add(sound_, L"The sound shown here: a loudness preset and an EQ under one name. The twelve shipped sounds "
                      L"are named after the provincial capitals; your own follow. Choosing one here only shows it - "
                      L"the main window's Sound list chooses what a deck plays.");
    tips_.add(saveAs_, L"Saves this loudness preset and EQ as a sound under a new name (or over one of your own) and "
                       L"moves the deck this window was opened from to it.");
    tips_.add(rename_, L"Renames your own sound; the decks that use it follow. Shipped sounds keep their names.");
    tips_.add(delete_, L"Deletes your own sound; a deck that uses it goes to Lelystad (Medium with Neutral). Shipped "
                       L"sounds cannot be deleted.");
    tips_.add(reset_, L"Sets the sound's loudness preset and EQ back: a shipped sound to its shipped two, your own to "
                      L"the two it was saved with.");
    tips_.add(part_[0], L"The sound's loudness preset: the target loudness and every stage's values. A new choice is "
                        L"heard at once on every deck that uses this sound, and saved.");
    tips_.add(part_[1], L"The sound's EQ, on top of the loudness preset: tone, auto tone, sub bass and a contour. A new "
                        L"choice is heard at once on every deck that uses this sound, and saved.");
    tips_.add(edit_[0], L"Opens the loudness window on this loudness preset: the stage switches and every value; Save "
                        L"as, Rename, Delete and Reset.");
    tips_.add(edit_[1], L"Opens the EQ window on this EQ: every value; Save as, Rename, Delete and Reset.");
}

void SoundWindow::setFonts() {
    for (HFONT f : {font_, bold_})
        if (f) DeleteObject(f);
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi_);
    font_ = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfWeight = FW_BOLD;
    bold_ = CreateFontIndirectW(&lf);
    for (HWND h = GetWindow(hwnd_, GW_CHILD); h; h = GetWindow(h, GW_HWNDNEXT))
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
    tips_.setDpi(dpi_);
    fill(); // the bold labels of changed parts
}

// Sound [.........] [Save as...] [Rename...] [Delete] [Reset sound]
// what it is, who uses it
// ------------------------------------------------------------
// Loudness [.........] [Edit...]
//   what the loudness preset does
// EQ       [.........] [Edit...]
//   what the EQ does
void SoundWindow::layout() {
    if (!sound_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const int m = px(12), gap = px(6), rowH = px(28), W = rc.right - 2 * m;
    const int labelW = px(70), buttonW = px(92), editW = px(80);
    int y = m;
    const int buttons = 4 * buttonW + 3 * gap;
    const int comboW = std::max(px(120), W - labelW - gap - buttons - gap);
    int x = m;
    MoveWindow(soundLabel_, x, y, labelW, rowH, TRUE);
    x += labelW + gap;
    MoveWindow(sound_, x, y + px(1), comboW, px(400), TRUE);
    x += comboW + gap;
    for (HWND b : {saveAs_, rename_, delete_, reset_}) {
        MoveWindow(b, x, y, buttonW, rowH, TRUE);
        x += buttonW + gap;
    }
    y += rowH + gap;
    MoveWindow(note_, m, y, W, px(36), TRUE);
    y += px(36) + gap;
    line_ = RECT{m, y, m + W, y + 2};
    y += gap + px(4);
    for (int k = 0; k < 2; ++k) {
        MoveWindow(partLabel_[k], m, y, labelW, rowH, TRUE);
        MoveWindow(part_[k], m + labelW + gap, y + px(1), comboW, px(400), TRUE);
        MoveWindow(edit_[k], m + labelW + gap + comboW + gap, y, editW, rowH, TRUE);
        y += rowH + px(2);
        MoveWindow(about_[k], m + labelW + gap, y, W - labelW - gap, px(52), TRUE);
        y += px(52) + gap;
    }
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

std::wstring SoundWindow::usersOf(const std::string& sound) const {
    std::wstring users;
    for (int d = 0; d < settings_.decks; ++d)
        if (!sound.empty() && settings_.deck[d].sound == sound)
            users += users.empty() ? (d == 0 ? deckA_ : L"deck B") : L" and deck B";
    return users;
}

// The sound list (with one entry for a deck without a sound: its two parts), the two part lists, the texts, the
// buttons and the labels of parts that differ from the sound's reset (bold).
void SoundWindow::fill() {
    if (!sound_) return;
    filling_ = true;
    auto setText = [](HWND h, const std::wstring& text) { // only a changed text: no needless repaint
        wchar_t now[1024] = {};
        GetWindowTextW(h, now, 1024);
        if (text != now) SetWindowTextW(h, text.c_str());
    };
    SendMessageW(sound_, CB_RESETCONTENT, 0, 0);
    int sel = -1;
    for (size_t i = 0; i < settings_.sounds.size(); ++i) {
        SendMessageW(sound_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(fromUtf8(settings_.sounds[i].name).c_str()));
        if (settings_.sounds[i].name == current_) sel = static_cast<int>(i);
    }
    const Sound* s = settings_.findSound(current_);
    const DeckSettings& deck = settings_.deck[deck_];
    if (!s) { // the deck plays two parts no sound holds
        const std::wstring none = L"(" + fromUtf8(deck.preset) + L", " + fromUtf8(deck.colour) + L")";
        sel = static_cast<int>(SendMessageW(sound_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(none.c_str())));
    }
    SendMessageW(sound_, CB_SETCURSEL, static_cast<WPARAM>(sel), 0);
    std::string reset[2];
    if (s) settings_.soundResetParts(s->name, reset[0], reset[1]);
    bool changed = false;
    for (int k = 0; k < 2; ++k) {
        const std::string& now = s ? (k == 0 ? s->loudness : s->eq) : (k == 0 ? deck.preset : deck.colour);
        SendMessageW(part_[k], CB_RESETCONTENT, 0, 0);
        const std::vector<Preset>& list = settings_.list(kKind[k]);
        int partSel = -1;
        for (size_t i = 0; i < list.size(); ++i) {
            SendMessageW(part_[k], CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(fromUtf8(list[i].name).c_str()));
            if (list[i].name == now) partSel = static_cast<int>(i);
        }
        SendMessageW(part_[k], CB_SETCURSEL, static_cast<WPARAM>(partSel), 0);
        const Preset* p = settings_.find(now, kKind[k]);
        setText(about_[k], p ? describePreset(*p, kKind[k]) : std::wstring());
        const bool differs = s && now != reset[k];
        changed = changed || differs;
        SendMessageW(partLabel_[k], WM_SETFONT, reinterpret_cast<WPARAM>(differs ? bold_ : font_), TRUE);
    }
    std::wstring note;
    const std::wstring users = usersOf(current_);
    if (!s) {
        note = L"No sound holds this loudness preset and EQ (they were chosen apart, or come from an older version). "
               L"Save as makes a sound of them; or choose a sound in the main window.";
    } else if (s->shipped) {
        note = L"Shipped sound: its loudness preset and EQ can be changed and reset; it cannot be renamed or deleted.";
    } else {
        note = L"Your own sound: it resets to the loudness preset and EQ it was saved with.";
    }
    if (s) note += users.empty() ? L" No deck uses it." : L" Used by " + users + L": a change is heard there at once.";
    if (s && changed) note += L" Bold = differs from its reset.";
    setText(note_, note);
    EnableWindow(rename_, s && !s->shipped);
    EnableWindow(delete_, s && !s->shipped);
    EnableWindow(reset_, s && changed);
    setText(hwnd_, L"Sound: " + (s ? fromUtf8(s->name) : std::wstring(L"none")));
    filling_ = false;
}

// A choice in the Loudness or the EQ list: the sound's part (every deck using it follows), or - for a deck without a
// sound - the deck's own.
void SoundWindow::choosePart(PresetKind kind) {
    const int k = kind == PresetKind::Colour ? 1 : 0;
    const LRESULT sel = SendMessageW(part_[k], CB_GETCURSEL, 0, 0);
    const std::vector<Preset>& list = settings_.list(kind);
    if (sel < 0 || static_cast<size_t>(sel) >= list.size()) return;
    const std::string name = list[static_cast<size_t>(sel)].name;
    if (settings_.findSound(current_)) {
        settings_.setSoundPart(current_, kind, name);
    } else {
        settings_.setDeckPart(deck_, kind, name);
        current_ = settings_.deck[deck_].sound; // its two parts may be a sound's now
    }
    if (settings_.linkPresets && settings_.decks == 2 && !settings_.chooseSound(1 - deck_, settings_.deck[deck_].sound)) {
        settings_.deck[1 - deck_].preset = settings_.deck[deck_].preset; // both decks
        settings_.deck[1 - deck_].colour = settings_.deck[deck_].colour;
        settings_.deck[1 - deck_].sound = settings_.deck[deck_].sound;
    }
    onChange_(false);
    fill();
}

void SoundWindow::command(int id, int code) {
    if (filling_) return;
    if (id == idSound && code == CBN_SELCHANGE) {
        const LRESULT sel = SendMessageW(sound_, CB_GETCURSEL, 0, 0);
        if (sel >= 0 && static_cast<size_t>(sel) < settings_.sounds.size()) select(settings_.sounds[static_cast<size_t>(sel)].name);
        return;
    }
    if ((id == idPart0 || id == idPart1) && code == CBN_SELCHANGE) {
        choosePart(kKind[id - idPart0]);
        return;
    }
    if (code != BN_CLICKED) return;
    const Sound* s = settings_.findSound(current_);
    const DeckSettings& deck = settings_.deck[deck_];
    // copies: the prompts below run their own message loop, and a save meanwhile merges the lists anew
    const std::string loudness = s ? s->loudness : deck.preset, eq = s ? s->eq : deck.colour;
    const bool shipped = s && s->shipped;
    s = nullptr;
    std::string err;
    if (id == idEdit0 || id == idEdit1) {
        if (openEditor_) openEditor_(kKind[id - idEdit0], id == idEdit0 ? loudness : eq, deck_);
    } else if (id == idSaveAs) {
        std::string name = current_.empty() ? loudness + " + " + eq : current_ + " (copy)";
        if (!askName(hwnd_, L"Save sound as", L"Name of the new sound:", name)) return;
        if (const Sound* existing = settings_.findSound(name)) {
            if (!existing->shipped) {
                const std::wstring q = L"Replace the sound \"" + fromUtf8(name) + L"\"?";
                if (placement::messageBox(hwnd_, q.c_str(), L"Save sound as", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
            }
        }
        if (!settings_.saveSoundAs(name, loudness, eq, err)) {
            placement::messageBox(hwnd_, fromUtf8(err).c_str(), L"Save sound as", MB_OK | MB_ICONWARNING);
            return;
        }
        if (deck.sound == current_) settings_.chooseSound(deck_, name); // the deck that used it takes the new one
        if (settings_.linkPresets && settings_.decks == 2) settings_.chooseSound(1 - deck_, settings_.deck[deck_].sound);
        current_ = name;
        onChange_(true);
        fill();
    } else if (id == idRename) {
        std::string name = current_;
        if (current_.empty() || !askName(hwnd_, L"Rename sound", L"New name:", name) || name == current_) return;
        if (!settings_.renameSound(current_, name, err)) {
            placement::messageBox(hwnd_, fromUtf8(err).c_str(), L"Rename sound", MB_OK | MB_ICONWARNING);
            return;
        }
        current_ = name;
        onChange_(true);
        fill();
    } else if (id == idDelete) {
        if (current_.empty()) return;
        const std::wstring q = L"Delete the sound \"" + fromUtf8(current_) +
                               L"\"? Its loudness preset and EQ stay; a deck that uses it changes to Lelystad (Medium "
                               L"with Neutral).";
        if (placement::messageBox(hwnd_, q.c_str(), L"Delete sound", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
            return;
        if (!settings_.removeSound(current_, err)) {
            placement::messageBox(hwnd_, fromUtf8(err).c_str(), L"Delete sound", MB_OK | MB_ICONWARNING);
            return;
        }
        if (settings_.linkPresets && settings_.decks == 2) settings_.chooseSound(1 - deck_, settings_.deck[deck_].sound);
        onChange_(true);
        select(settings_.deck[deck_].sound);
    } else if (id == idReset) {
        if (current_.empty()) return;
        std::string l, e;
        settings_.soundResetParts(current_, l, e);
        const std::wstring q = L"Set \"" + fromUtf8(current_) + L"\" back to " + fromUtf8(l) + L" with " + fromUtf8(e) +
                               (shipped ? L", as shipped?" : L", as it was saved?");
        if (placement::messageBox(hwnd_, q.c_str(), L"Reset sound", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
            return;
        settings_.resetSound(current_);
        onChange_(false);
        fill();
    }
}

} // namespace ap
