// SPDX-License-Identifier: GPL-3.0-or-later
#include "pluginview.h"

#include "meterview.h"
#include "placement.h"
#include "plugin.h"
#include "pluginids.h"
#include "settingslock.h"
#include "stages.h"
#include "updater.h"
#include "version.h"

#include "pluginterfaces/gui/iplugview.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <memory>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <string>

extern HINSTANCE ghInst; // the plug-in DLL (public.sdk/source/main/dllmain.cpp)

using namespace Steinberg;

namespace ap::plugin {

namespace {

const wchar_t* const kClass = L"DJAPPluginView";
// (the bass sliders, oversampling, look-ahead and About left the window on 2026-10-29: client, "they can all go,
// they are in other windows" - the spectrum window, the Setup window and the right-click menu)
enum Id { idPreset = 1, idColour, idBypass, idEdit, idEditColour, idGraphs, idVersion, idMeter = 20 };
enum MenuId { mSetup = 1, mAbout };

int g_views = 0; // open windows of this DLL: the classes and GDI+ live while one is open
// The update check of this DLL (client, 2026-10-28: "what if the user installs only the plug-in - can it update?"):
// one for every window, made with the first and ended (its thread joined) with the last; GitHub is asked once per
// load of the DLL, when the program's daily check is on (the plug-in is opened and closed often - GitHub answers 60
// questions an hour per network)
std::unique_ptr<Updater> g_updater;
bool g_checked = false;
// The places of the DLL's windows (client, 2026-10-29: "the plug-in does not remember the windows' places and
// sizes"): one store for every window of the DLL while one is open, in the shared settings file under "Plugin..."
// names, saved as a guest (Settings::saveMerged writes the windows it moved and keeps the program's)
std::unique_ptr<Settings> g_places, g_placesBase;
std::filesystem::path g_placesFile;

void openPlaces(const std::filesystem::path& file) {
    if (g_places || file.empty()) return;
    g_placesFile = file;
    g_places = std::make_unique<Settings>();
    std::error_code ec;
    if (std::filesystem::exists(file, ec)) g_places->load(file);
    g_placesBase = std::make_unique<Settings>(*g_places);
    placement::setStore(&g_places->windows, [] {
        if (!g_places) return;
        std::error_code e;
        std::filesystem::create_directories(g_placesFile.parent_path(), e); // a plug-in without the program
        const SettingsLock lock; // the program or another instance saving at the same moment waits
        g_places->saveMerged(g_placesFile, *g_placesBase, Settings::SaveRole::Guest);
    }, "Plugin");
}

void closePlaces() {
    placement::setStore(nullptr, {});
    g_places.reset();
    g_placesBase.reset();
}

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

} // namespace

// The program's main window at its smallest with one channel (client, 2026-10-29: "the plug-in the same size as the
// program, its smallest"): 600 px wide, the two rows at the top, the meter panel at its smallest height
// (meterViewMinHeight: every stage row and a readable spectrum) and the margin under it - no status bar. Was 820 x 640.
ViewRect PluginView::sizeFor(UINT dpi) {
    auto px = [dpi](int v) { return MulDiv(v, static_cast<int>(dpi), 96); };
    return ViewRect(0, 0, px(600), meterTop(dpi) + meterViewMinHeight(dpi) + px(12));
}

bool PluginView::Graphs::audioActive() const { return view_.plugin_ && view_.plugin_->active(); }

PresetValues PluginView::Graphs::values(int) const {
    return view_.plugin_ ? view_.plugin_->currentValues() : PresetValues{};
}

std::wstring PluginView::Graphs::title(int) const {
    if (!view_.plugin_) return L"DJAP";
    const auto& presets = view_.plugin_->presets();
    const int i = view_.plugin_->presetIndex();
    return L"DJAP   " + (i >= 0 && i < static_cast<int>(presets.size()) ? wide(presets[static_cast<size_t>(i)].name)
                                                                         : std::wstring(L"(other values)"));
}

bool PluginView::Graphs::processing(int) const { return !view_.plugin_ || view_.plugin_->plainOf(kBypassId) < 0.5; }

double PluginView::Spectrum::sampleRate() const {
    const Plugin* p = view_.plugin_;
    return p && p->active() && p->sampleRate() > 0 ? p->sampleRate() : 0.0;
}

std::vector<std::string> PluginView::Spectrum::names(bool colour) const {
    std::vector<std::string> n;
    if (view_.plugin_)
        for (const Preset& p : colour ? view_.plugin_->colours() : view_.plugin_->presets()) n.push_back(p.name);
    return n;
}

int PluginView::Spectrum::chosen(int, bool colour) const {
    const Plugin* p = view_.plugin_;
    if (!p) return -1;
    const int i = colour ? p->colourIndex() : p->presetIndex();
    const size_t size = (colour ? p->colours() : p->presets()).size();
    return i >= 0 && static_cast<size_t>(i) < size ? i : -1; // the list's size: "(other values)"
}

std::string PluginView::Spectrum::chosenName(int d, bool colour) const {
    const int i = chosen(d, colour);
    return i < 0 ? std::string() : (colour ? view_.plugin_->colours() : view_.plugin_->presets())[static_cast<size_t>(i)].name;
}

void PluginView::Spectrum::choose(int, bool colour, int index) {
    if (!view_.plugin_) return;
    view_.plugin_->choose(colour, index);
    view_.refresh();
}

void PluginView::Spectrum::set(int, const std::vector<std::pair<int, double>>& values, bool) {
    if (!view_.plugin_) return;
    for (const auto& [param, value] : values) view_.plugin_->editPlain(paramId(param), value);
    view_.refresh(); // the bass sliders of the plug-in's window follow
}

void PluginView::Spectrum::setBass(int d, int slider, int pos, bool save) { set(d, bass::changes(slider, pos), save); }

void PluginView::Spectrum::reset(int d, bool colour, HWND owner) {
    const int i = chosen(d, colour);
    if (!view_.plugin_ || i < 0) {
        placement::messageBox(owner, colour ? L"These colour values match no colour in the list." :
                                              L"These values match no loudness preset in the list.",
                              colour ? L"Reset colour" : L"Reset loudness", MB_OK | MB_ICONINFORMATION);
        return;
    }
    const std::wstring q = std::wstring(L"Set the plug-in's ") + (colour ? L"colour" : L"loudness") +
                           L" values back to those of \"" + wide(chosenName(d, colour)) + L"\"?";
    if (placement::messageBox(owner, q.c_str(), colour ? L"Reset colour" : L"Reset loudness",
                              MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;
    view_.plugin_->choose(colour, i); // the chosen one again: every value of it
    view_.refresh();
}

const wchar_t* PluginView::Spectrum::resetTip(bool colour) const {
    return colour ? L"Reset colour: the plug-in's colour values back to those of the colour chosen in the list (asks "
                    L"first) - after points were dragged here or the host changed them. The colour itself is not changed."
                  : L"Reset loudness: the plug-in's loudness values back to those of the loudness preset chosen in the "
                    L"list (asks first) - the density band handles go back with them. The preset itself is not changed.";
}

std::wstring PluginView::Spectrum::hint(int) const {
    return L"Drag a point: this plug-in's colour values (blue: bass level and frequency). A purple handle: a density "
           L"band's fixed gain. The project keeps them - the presets in the lists stay as they are; double-click resets.";
}

PluginView::PluginView(Plugin* plugin)
    : plugin_(plugin), spectrumSource_(*this), spectrumWnd_(spectrumSource_), setupWnd_(plugin_) {
    const ViewRect r = sizeFor(GetDpiForSystem());
    setRect(r);
}

PluginView::~PluginView() {
    if (plugin_) plugin_->viewClosed(this);
}

void PluginView::detach() {
    teardown();
    plugin_ = nullptr;
}

void PluginView::teardown() {
    if (plugin_) plugin_->closeEditors(); // they belong to this window (and their classes go with the last one)
    aboutWnd_.close();                    // likewise (its owner is the host's window, not this one)
    graphsWnd_.close();                   // likewise
    spectrumWnd_.close();
    setupWnd_.close();
    if (hwnd_) {
        KillTimer(hwnd_, 1);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (font_) DeleteObject(font_);
    if (bold_) DeleteObject(bold_);
    font_ = bold_ = nullptr;
    if (attached_) {
        attached_ = false;
        if (--g_views == 0) unregisterClasses();
    }
}

void PluginView::registerClasses(HINSTANCE instance) {
    theme::init();
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassW(&wc);
    registerMeterView(instance);
    EditWindow::setModule(instance); // the preset window registers its classes when it is first opened
    AboutWindow::setModule(instance); // About likewise
    AnalysisWindow::setModule(instance); // and the Graphs window
    SpectrumWindow::setModule(instance); // the spectrum window
    SetupWindow::setModule(instance);    // the Setup window
    if (!g_updater) g_updater = std::make_unique<Updater>();
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
    InitCommonControlsEx(&icc);
}

void PluginView::unregisterClasses() {
    g_updater.reset(); // waits for a check still running (its thread must not outlive the DLL)
    closePlaces();
    AboutWindow::unregisterClass();
    AnalysisWindow::unregisterClass();
    SpectrumWindow::unregisterClass();
    SetupWindow::unregisterClass();
    EditWindow::unregisterClasses();
    unregisterMeterView();
    UnregisterClassW(kClass, ghInst);
    theme::shutdown();
}

tresult PLUGIN_API PluginView::isPlatformTypeSupported(FIDString type) {
    return type && std::strcmp(type, kPlatformTypeHWND) == 0 ? kResultTrue : kResultFalse;
}

tresult PLUGIN_API PluginView::attached(void* parent, FIDString type) {
    if (isPlatformTypeSupported(type) != kResultTrue || !parent || !plugin_ || attached_) return kResultFalse;
    if (g_views++ == 0) registerClasses(ghInst);
    attached_ = true;
    Tooltips::setEnabled(plugin_->tooltips()); // the program's Setup switch
    openPlaces(plugin_->iniFile());            // the windows' remembered places
    const tresult r = CPluginView::attached(parent, type);
    dpi_ = GetDpiForWindow(static_cast<HWND>(parent));
    if (dpi_ == 0) dpi_ = 96;
    ViewRect want = sizeFor(dpi_);
    if (want.getWidth() != rect.getWidth() || want.getHeight() != rect.getHeight()) {
        setRect(want);
        if (plugFrame) plugFrame->resizeView(this, &want);
    }
    hwnd_ = CreateWindowExW(WS_EX_CONTROLPARENT, kClass, L"DJAP", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0,
                            rect.getWidth(), rect.getHeight(), static_cast<HWND>(parent), nullptr, ghInst, this);
    if (!hwnd_) {
        teardown();
        return kResultFalse;
    }
    create();
    layout();
    fill();
    // About: the plug-in's (not modal; Download opens the release's page - the plug-in runs inside the DJ software
    // and the setup needs an administrator: it is not started from here)
    // Graphs: a click on a step of the chain opens its tab, as in the program
    graphsWnd_.onOpenStage([this](int, const std::string& stage, bool colour) {
        if (plugin_ && hwnd_) plugin_->openEditor(colour, GetAncestor(hwnd_, GA_ROOT), stage);
    });
    spectrumSource_.snap_ = plugin_->snapToBands();
    aboutWnd_.setPlugin(true);
    aboutWnd_.setBeatSource([this] { return &monitor_.analysis().scopeOut; });
    aboutWnd_.setUpdates(
        [this] { return g_updater ? updateViewOf(*g_updater, plugin_ && plugin_->checkUpdates(), false, false) : UpdateView(); },
        [this] {
            if (g_updater && !g_updater->busy()) g_updater->check(hwnd_, std::string(), AP_VERSION_STRING);
        },
        [] {
            if (!g_updater) return;
            const std::string page = g_updater->release().page.empty() ? Updater::kLatestPage : g_updater->release().page;
            if (page.compare(0, 19, "https://github.com/") == 0) // only GitHub's own pages
                ShellExecuteW(nullptr, L"open", wide(page).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        });
    // (DJAP_NO_UPDATE_CHECK set: a test with editorhost - the plug-in reads the real settings file, and a test asks
    // GitHub nothing)
    if (g_updater && !g_checked && plugin_->checkUpdates() && !GetEnvironmentVariableW(L"DJAP_NO_UPDATE_CHECK", nullptr, 0)) {
        g_checked = true;
        g_updater->check(hwnd_, std::string(), AP_VERSION_STRING);
    }
    updateNote();
    SetTimer(hwnd_, 1, 50, nullptr);
    return r;
}

tresult PLUGIN_API PluginView::removed() {
    teardown();
    return CPluginView::removed();
}

LRESULT CALLBACK PluginView::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE)
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    auto* v = reinterpret_cast<PluginView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (v && v->hwnd_ == hwnd) return v->handle(msg, wp, lp);
    if (v && !v->hwnd_ && msg != WM_NCDESTROY) { // during CreateWindowExW: the handle is not stored yet
        LRESULT result = 0;
        if (v->metal_.handle(hwnd, msg, wp, lp, result)) return result;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT PluginView::handle(UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT result = 0;
    if (metal_.handle(hwnd_, msg, wp, lp, result)) return result;
    if (msg == Updater::message()) { // a check ended
        aboutWnd_.refresh();
        updateNote();
        return 0;
    }
    switch (msg) {
    case WM_TIMER:
        if (dirty_) fill();
        meters();
        graphsWnd_.refresh(); // after meters(): it read the deck's rings
        spectrumWnd_.refresh();
        setupWnd_.refresh();
        if (++ticks_ % 20 == 0) { // once a second: the update line (another window may have started the check)
            aboutWnd_.refresh();
            updateNote();
        }
        return 0;
    case WM_COMMAND:
        command(LOWORD(wp), HIWORD(wp));
        return 0;
    case WM_CONTEXTMENU: { // a right-click anywhere on the window (its controls pass it up): Setup, About
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (pt.x == -1 && pt.y == -1) { // from the keyboard: at the window's top left
            pt = {px(40), px(40)};
            ClientToScreen(hwnd_, &pt);
        }
        contextMenu(pt.x, pt.y);
        return 0;
    }
    case WM_DPICHANGED_AFTERPARENT:
        dpi_ = GetDpiForWindow(hwnd_);
        layout();
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void PluginView::create() {
    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
        return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hwnd_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), ghInst, nullptr);
    };
    title_ = make(L"STATIC", L"DJAP", SS_LEFT | SS_CENTERIMAGE, 0);
    version_ = make(L"STATIC", wide(AP_VERSION_STRING).c_str(), SS_LEFT | SS_CENTERIMAGE | SS_NOTIFY, idVersion);
    graphs_ = make(L"BUTTON", L"Graphs", BS_OWNERDRAW | WS_TABSTOP, idGraphs);
    bypass_ = make(L"BUTTON", L"Bypass Off", BS_OWNERDRAW | WS_TABSTOP, idBypass);
    presetLabel_ = make(L"STATIC", L"Loudness", SS_LEFT | SS_CENTERIMAGE, 0);
    preset_ = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, idPreset);
    colourLabel_ = make(L"STATIC", L"Colour", SS_LEFT | SS_CENTERIMAGE, 0);
    colour_ = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, idColour);
    edit_ = make(L"BUTTON", L"Edit...", BS_OWNERDRAW | WS_TABSTOP, idEdit);
    editColour_ = make(L"BUTTON", L"Edit...", BS_OWNERDRAW | WS_TABSTOP, idEditColour);
    meter_ = createMeterView(hwnd_, idMeter);
    for (HWND b : {graphs_, bypass_, edit_, editColour_}) theme::makeButton(b);
    for (HWND c : {preset_, colour_}) theme::makeCombo(c);
    fillLists();

    tips_.create(hwnd_);
    tips_.add(version_, L"DJAP processes this channel like one deck of the program, with the same presets and "
                        L"colours. Every value is also a parameter in the host's own list, and can be automated.\n\n"
                        L"Right-click anywhere on the window: Setup (look-ahead, oversampling, tooltips, the update "
                        L"check) and About DJAP. When a newer version exists it says so here; a click opens About.");
    tips_.add(preset_, L"The loudness preset: the target loudness and every stage's values. The list holds the program's "
                       L"loudness presets and your own (from the settings file the program and the plug-in share). The "
                       L"project keeps the values themselves, so it sounds the same on a computer without that "
                       L"preset; \"(other values)\" = values that match no preset in the list.");
    tips_.add(colour_, L"The colour, on top of the loudness preset: tone, auto tone, sub bass and a contour. "
                       L"Neutral changes nothing.");
    tips_.add(edit_, L"Opens the loudness window: the stage switches and every value of the loudness preset with its unit, "
                     L"range and meaning; Save as, Rename, Delete and Reset. Your presets are saved in the settings "
                     L"file the program uses too.");
    tips_.add(editColour_, L"Opens the colour window: every value of the colour; Save as, Rename, Delete and Reset.");
    tips_.add(graphs_, L"Opens the analysis window of this channel: the signal chain step by step (a click on a step "
                       L"opens its settings), the compressor's curve, loudness and gains of the last 60 s, the scope "
                       L"(right-click: input or output) and the goniometer with correlation and stereo width.");
    tips_.add(bypass_, L"Bypass Off (green): the channel runs through DJAP's chain.\nBypass On (red): the input goes "
                       L"out unchanged, delayed by the same look-ahead, so switching never shifts the sound in time.");
}

void PluginView::layout() {
    if (!hwnd_) return;
    if (font_) DeleteObject(font_);
    if (bold_) DeleteObject(bold_);
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi_);
    font_ = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfWeight = FW_BOLD;
    lf.lfHeight = lf.lfHeight * 3 / 2;
    bold_ = CreateFontIndirectW(&lf);
    for (HWND h = GetWindow(hwnd_, GW_CHILD); h; h = GetWindow(h, GW_HWNDNEXT))
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(h == title_ ? bold_ : font_), FALSE);
    tips_.setDpi(dpi_);

    RECT rc;
    GetClientRect(hwnd_, &rc);
    const int m = px(12), gap = px(6), rowH = px(28), W = rc.right - 2 * m;
    int y = m;
    // DJAP  0.1.x (update note) ..................................... [Graphs] [Bypass]
    const int bypassW = px(80), graphsW = px(72);
    MoveWindow(title_, m, y, px(70), rowH, TRUE);
    MoveWindow(version_, m + px(74), y + px(2), W - px(74) - graphsW - bypassW - 2 * gap, rowH, TRUE);
    MoveWindow(graphs_, m + W - bypassW - gap - graphsW, y, graphsW, rowH, TRUE);
    MoveWindow(bypass_, m + W - bypassW, y, bypassW, rowH, TRUE);
    y += rowH + gap;
    // Loudness [....] [Edit...]  Colour [....] [Edit...]
    const int labelW = px(66), colourLabelW = px(50), editW = px(64);
    const int combos = W - labelW - colourLabelW - 2 * editW - 5 * gap - px(10);
    const int presetW = combos / 2, colourW = combos - presetW;
    int x = m;
    MoveWindow(presetLabel_, x, y, labelW, rowH, TRUE);
    x += labelW + gap;
    MoveWindow(preset_, x, y, presetW, px(300), TRUE);
    x += presetW + gap;
    MoveWindow(edit_, x, y, editW, rowH, TRUE);
    x += editW + gap + px(10);
    MoveWindow(colourLabel_, x, y, colourLabelW, rowH, TRUE);
    x += colourLabelW + gap;
    MoveWindow(colour_, x, y, colourW, px(300), TRUE);
    x += colourW + gap;
    MoveWindow(editColour_, x, y, editW, rowH, TRUE);
    y += rowH + gap; // = meterTop(dpi_), as the program's meterTop()
    MoveWindow(meter_, m, y, W, rc.bottom - m - y, TRUE);
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

void PluginView::listsChanged() {
    if (!hwnd_) return;
    fillLists();
    fill();
}

void PluginView::fillLists() {
    // the presets and colours of the shared settings file, and one entry for values that match none
    for (const bool colour : {false, true}) {
        HWND box = colour ? colour_ : preset_;
        SendMessageW(box, CB_RESETCONTENT, 0, 0);
        for (const Preset& p : colour ? plugin_->colours() : plugin_->presets())
            SendMessageW(box, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide(p.name).c_str()));
        SendMessageW(box, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"(other values)"));
    }
}

void PluginView::fill() {
    dirty_ = false;
    if (!hwnd_) return;
    const int pi = plugin_->presetIndex(), ci = plugin_->colourIndex();
    if (SendMessageW(preset_, CB_GETCURSEL, 0, 0) != pi) SendMessageW(preset_, CB_SETCURSEL, static_cast<WPARAM>(pi), 0);
    if (SendMessageW(colour_, CB_GETCURSEL, 0, 0) != ci) SendMessageW(colour_, CB_SETCURSEL, static_cast<WPARAM>(ci), 0);
    theme::setBypass(bypass_, plugin_->plainOf(kBypassId) >= 0.5);
}

void PluginView::meters() {
    if (!meter_) return;
    monitor_.poll(plugin_->deck(), plugin_->active() ? plugin_->sampleRate() : 0.0, 0.05);
    const PresetValues v = plugin_->currentValues();
    const bool processing = plugin_->plainOf(kBypassId) < 0.5;
    MeterViewData data;
    data.m = monitor_.meters();
    data.target = v[pTarget];
    data.ceiling = v[pLimCeiling];
    data.levRange = v[pLevRange];
    data.inputGain = processing ? v[pInputGain] : 0.0;
    if (data.m.bands == 0) data.m.bands = static_cast<int>(v[pDensBands]);
    for (int k = 0; k < kStageCount; ++k) data.stageOn[k] = v[kStageParam[k]] >= 0.5;
    data.processing = processing;
    data.audio = plugin_->active();
    const DeckAnalysis& an = monitor_.analysis();
    data.spectrum = data.audio && data.m.live && an.specOut.frames() > 0;
    data.specIn = an.specIn.points();
    data.specOut = an.specOut.points();
    for (int k = 0; k < kMaxSplits; ++k) data.splits[k] = v[static_cast<size_t>(pDensSplit1 + k)];
    data.colourValues = v;
    for (int k = 0; k < 4; ++k) data.autoDb[k] = data.m.autoToneDb[k];
    data.fs = plugin_->active() && plugin_->sampleRate() > 0 ? plugin_->sampleRate() : 48000.0;
    setMeterView(meter_, data);
}

void PluginView::command(int id, int code) {
    // CBN_SELENDOK too: the item that is already chosen, picked again, sends no CBN_SELCHANGE - and choosing it again
    // loads its values again (back to the preset after changes)
    if ((id == idPreset || id == idColour) && (code == CBN_SELCHANGE || code == CBN_SELENDOK)) {
        const bool colour = id == idColour;
        const int sel = static_cast<int>(SendMessageW(colour ? colour_ : preset_, CB_GETCURSEL, 0, 0));
        plugin_->choose(colour, sel);
        fill();
    } else if (id == idBypass && code == BN_CLICKED) {
        plugin_->edit(kBypassId, plugin_->plainOf(kBypassId) >= 0.5 ? 0.0 : 1.0);
        fill();
    } else if (id == idGraphs && code == BN_CLICKED) {
        graphsWnd_.show(GetAncestor(hwnd_, GA_ROOT));
    } else if (id == idVersion && code == STN_CLICKED) { // the version (with its update note): About
        aboutWnd_.show(GetAncestor(hwnd_, GA_ROOT), plugin_->iniFile());
    } else if ((id == idEdit || id == idEditColour) && code == BN_CLICKED) {
        plugin_->openEditor(id == idEditColour, GetAncestor(hwnd_, GA_ROOT));
    } else if (id == idMeter && code == kMeterOpenRow + kMeterSpectrum) { // the spectrum: the spectrum window
        spectrumWnd_.show(GetAncestor(hwnd_, GA_ROOT), 0);
    } else if (id == idMeter && code >= kMeterOpenRow && code < kMeterOpenRow + kMeterTargets) { // a row or bar
        plugin_->openEditor(false, GetAncestor(hwnd_, GA_ROOT), meterStage(code - kMeterOpenRow));
    }
}

void PluginView::contextMenu(int x, int y) {
    if (!plugin_) return;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, mSetup, L"&Setup...");
    AppendMenuW(m, MF_STRING, mAbout, L"&About DJAP...");
    const UINT cmd = static_cast<UINT>(TrackPopupMenuEx(m, TPM_RIGHTBUTTON | TPM_RETURNCMD, x, y, hwnd_, nullptr));
    DestroyMenu(m);
    const HWND root = GetAncestor(hwnd_, GA_ROOT);
    if (cmd == mSetup) setupWnd_.show(root);
    else if (cmd == mAbout) aboutWnd_.show(root, plugin_->iniFile());
}

// A newer version found: the version line says so (the About button that showed "Update" went into the right-click
// menu, 2026-10-29); a click on it opens About.
void PluginView::updateNote() {
    if (!version_) return;
    const bool newer = g_updater && g_updater->state() == Updater::State::Available &&
                       compareVersions(g_updater->release().version, AP_VERSION_STRING) > 0;
    const std::wstring t = wide(AP_VERSION_STRING) +
                           (newer ? L"  -  " + wide(g_updater->release().version) + L" is available: click"
                                  : L"");
    wchar_t now[256] = {};
    GetWindowTextW(version_, now, 256);
    if (t != now) SetWindowTextW(version_, t.c_str());
}

} // namespace ap::plugin
