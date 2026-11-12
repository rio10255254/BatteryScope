#include "platform.h"
#include "energy.h"
#include "motion.h"
#include "i18n.h"
#include "version.h"
#include "update.h"
#include "lab.h"
#include "governor.h"
#include "asus.h"
#include "ui.h"
#include <commctrl.h>
#include <pdh.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <cwctype>
#include <set>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <sstream>
#include <thread>

namespace {
using namespace bs;
constexpr UINT SampleMessage = WM_APP + 1, TrayMessage = WM_APP + 2, ActionMessage = WM_APP + 3, RefreshMessage = WM_APP + 4,
    PowerModeMessage = WM_APP + 5, UpdateMessage = WM_APP + 6, UpdateDownloadedMessage = WM_APP + 7, LabExportMessage = WM_APP + 8,
    GovernorNowMessage = WM_APP + 9, // input or a program switch: re-run the governor now
    GpuUsersMessage = WM_APP + 10,   // programs found on the discrete GPU (before switching it off)
    SectionRefreshMessage = WM_APP + 11, AsusKeyMessage = WM_APP + 12, AsusServicesMessage = WM_APP + 13,
    OsdMessage = WM_APP + 14,      // a key's result from the key worker: wParam what, lParam the level (-1 unknown)
    PopupClickMessage = WM_APP + 15, // a notification was clicked: wParam its BalloonTarget
    TuningDoneMessage = WM_APP + 16; // fan or power preset applied: wParam the status
constexpr int ListId = 100, CloseId = 101, KillId = 102, ServiceId = 103, DetailsId = 104,
    BackgroundId = 105, AllId = 106, StartupId = 107, HideId = 108, MenuId = 109,
    LanguageZhId = 110, LanguageEnId = 111, PowerDetailsId = 112, PowerModeId = 113,
    EnergySaverSettingsId = 114, ReviewCandidatesId = 115, InlineBackId = 116, InlineConfirmId = 117,
    LedgerId = 118, ExportCsvId = 119, LowBatteryOffId = 120, LowBattery10Id = 121, LowBattery20Id = 122, LowBattery30Id = 123,
    HotkeyToggleId = 124, ThemeSystemId = 125, ThemeDarkId = 126, ThemeLightId = 127, StylePixelId = 128, StyleSmoothId = 129,
    ModeAcEfficiencyId = 140, ModeAcBalancedId = 141, ModeAcPerformanceId = 142,
    ModeDcEfficiencyId = 143, ModeDcBalancedId = 144, ModeDcPerformanceId = 145, RestoreModesId = 146, TrayPercentId = 147,
    DrainAlertId = 148, HistoryId = 149, HistoryToggleId = 150, ClearHistoryId = 151,
    ChargeReminderOffId = 152, ChargeReminder80Id = 153, ChargeReminder85Id = 154, ChargeReminder90Id = 155, ChargeReminder100Id = 156,
    SleepAlertId = 157, AutoSaverOffId = 158, AutoSaver20Id = 159, AutoSaver30Id = 160, PerfAppsPageId = 161, ClearPerfAppsId = 162,
    EcoToggleId = 163, PerfAppToggleId = 164, MascotToggleId = 165, ShareCardId = 166, MiniToggleId = 167, MiniOpenId = 168,
    UpdateCheckId = 169, UpdateNowId = 170, UpdateInstallId = 171,
    LabPageId = 172, LabRecordId = 173, LabExperimentId = 174 /* ..177, one per knob */, LabStopId = 178, LabFolderId = 179, LabExportId = 180, LabAutoId = 181, LabAutoStartId = 182,
    DynamicToggleId = 183, DynamicPageId = 184, DynamicRefreshId = 185, DynamicEcoId = 186, DynamicTargetId = 187, SettingsPageId = 188, GoalPageId = 189, GoalCycleId = 190, LabQuickId = 191, LabQuickStartId = 192, UpdateAutoId = 193, UpdateNotifyId = 194, UpdateOffId = 195, UpdatePageId = 196,
    HardwarePageId = 197, AsusSilentId = 198, AsusBalancedId = 199, AsusTurboId = 202, GpuEcoId = 203, GpuStandardId = 204,
    Charge60Id = 205, Charge80Id = 206, Charge100Id = 207, OverdriveId = 208, AsusAutoId = 209, GpuAutoId = 210, ReleaseHoldsId = 211,
    HwModeMenuId = 212, HwGpuMenuId = 213, HwChargeMenuId = 214, HwKeyboardMenuId = 215, HwKeysMenuId = 216, HwServicesMenuId = 217,
    AppRulesMenuId = 218, LabSingleMenuId = 219, Keyboard0Id = 220 /* ..223 */, KeyboardAutoOffId = 224, KeysHandleId = 225,
    RogOpenId = 226, RogModeId = 227, RogNoneId = 228, ServicesStopId = 229, ServicesStartId = 230,
    ServicesDefaultsId = 231, FnLockId = 232, FnCLockId = 233, FnCNoneId = 234, HwSlashMenuId = 235, SlashToggleId = 236, SlashBatteryId = 237,
    SlashBright1Id = 238 /* ..240 */, SlashIntervalId = 241 /* ..246 */, SlashModeId = 250 /* ..265 */,
    RefreshAutoId = 266, DarkModeId = 267, AutoSaverCycleId = 268, PerfPageId = 269, FanMenuId = 270, PowerLimitMenuId = 271,
    FanPreset0Id = 272 /* ..275 */, PowerPreset0Id = 276 /* ..279 */, CpuBoostToggleId = 280, WinModeMenuId = 281,
    SettingsListId = 300,
    TrayShow = 200, TrayExit = 201;
constexpr int PanelHotkey = 1, FnKeyHotkey = 10 /* ..20: F1-F11 under the software Fn lock */;
const wchar_t* ClassName = L"BatteryScope.Main.v1";
const wchar_t* const AppTitle = L"BatteryScope " BS_VERSION_STRING;
// Theme palette; the short names below mirror it for the drawing code.
ui::ThemeChoice themeChoice = ui::ThemeChoice::system;
bool previewContrast = false; // --high-contrast preview option
// Pixel style: stepped shapes, cell meters, bitmap digits, and the embedded pixel
// font for UI text (strings it cannot render fall back to the system font).
bool pixelStyle = true;
constexpr int PixelFontResource = 102;
ui::Palette theme = ui::palette(ui::ThemeChoice::dark);
COLORREF Ink = theme.ink, Muted = theme.muted, Green = theme.accent, Background = theme.background,
    Surface = theme.surface, Border = theme.border, Selected = theme.selected, Danger = theme.danger;
ui::IconCache processIcons;
HWND mainWindow = nullptr, viewport = nullptr, list = nullptr, closeButton = nullptr, killButton = nullptr, serviceButton = nullptr,
    detailsButton = nullptr, backgroundCheck = nullptr, allCheck = nullptr, hideButton = nullptr, menuButton = nullptr, powerButton = nullptr,
    inlineText = nullptr, inlineBack = nullptr, inlineConfirm = nullptr;
HFONT normalFont = nullptr, smallFont = nullptr, strongFont = nullptr, titleFont = nullptr, valueFont = nullptr, metricFont = nullptr;
HFONT systemFont = nullptr;              // dynamic text: details pages and tooltips
std::map<HFONT, HFONT> fallbackFonts;    // pixel font -> system font for uncovered text
std::vector<HFONT> fallbackOwned;
HICON appIcon = nullptr, smallIcon = nullptr, headerIcon = nullptr;
bool updateTray(bool add);
UINT dpi = 96, taskbarCreated = 0;
std::atomic<bool> quit{false}, hidden{false}, resetSamples{false};
// Daily history, owned by the sampler thread; the UI flips these flags.
std::atomic<bool> historyEnabled{true}, clearHistoryRequest{false};
int localDate(int daysBack = 0); // yyyymmdd in local time, `daysBack` days ago
std::wstring healthTrendText();
std::mutex mailboxMutex, wakeMutex;
std::condition_variable wake;
// The sampler hands each process list over by move: mailboxProcesses says the
// mailbox carries a new list, otherwise the UI keeps the list it already has.
Snapshot mailbox, current;
bool mailboxFresh = false, mailboxProcesses = false;
std::vector<ProcessRow> displayed;
std::vector<std::wstring> displayedLabels;
uint64_t listStructuralChanges = 0;
std::thread worker, actionWorker;
bool pendingAction = false, forceAction = false, serviceAction = false, trayAdded = false, demo = false;
bool trayVersion4 = false, backgroundOnly = false, showAll = false, autoStart = false;
bool powerSaving = false;
// Power mode page: what Windows applies now, and the plugged-in / on-battery settings.
EffectiveMode effectiveMode = EffectiveMode::unknown;
void* effectiveWatch = nullptr;
PowerModes powerModes;
HWND modeButtons[6] = {}, restoreModesButton = nullptr, energySettingsButton = nullptr;
bool isolatedPreview = false;
enum class InlinePage { none, process, power, candidates, ledger, powerMode, history, perfApps, lab, settings, dynamic, goal, hardware, performance, confirm, error };
unsigned lowBatteryThreshold = 0; // percent; 0 = off
LowBatteryMonitor lowBattery;
bool hotkeyEnabled = false, hotkeyRegistered = false;
bool trayPercent = true;          // show the battery percentage as the notification icon
bool drainAlerts = false;         // notify about sustained high-power background work
std::set<std::pair<uint32_t, uint64_t>> drainNotified;
uint64_t lastDrainAlert = 0;
enum class BalloonTarget { none, candidates, powerDetails, update };
bool updateChecks = true; // daily update check, on unless turned off
bool updateAuto = true;    // install a new version by itself (after any running test)
bool updateDeferred = false;
bool gpuAuto = false;          // ASUS: discrete GPU in Eco on battery, standard when plugged in
bool refreshAuto = true;       // 60 Hz on battery, the panel's highest rate plugged in
bool keyboardAutoOff = false;   // ASUS: keyboard backlight off after 30 s without input on battery
bool asusKeysHandled = true;    // ASUS: BatteryScope acts on the ASUS keys the ASUS software is not there to handle
unsigned rogKeyAction = 0;      // 0 open BatteryScope, 1 cycle the performance mode, 2 nothing
unsigned fnCAction = 0;         // Fn+C: 0 toggles the Fn lock, 1 nothing
std::deque<std::pair<unsigned, uint64_t>> asusKeysSeen; // recent ASUS key codes, newest last
// Dynamic power saving (the governor), opt-in. Target 0 means no battery target.
bool dynamicEnabled = false, dynamicRefresh = false, dynamicEco = true;
unsigned dynamicTarget = 0;
// Battery power goals in watts while in use and while still (0 = no goal).
unsigned goalUse = 10, goalStill = 7;
HWND settingsList = nullptr;
BalloonTarget balloonTarget = BalloonTarget::none;  // what clicking the last notification opens
bool sleepAlerts = true;          // notify after a sleep with unusual battery drain
unsigned seenSleeps = 0;
unsigned chargeReminderTarget = 0; // percent; 0 = off
ChargeReminder chargeReminder;
// Processes BatteryScope put in efficiency mode, with their original priority; undone on exit.
struct EfficientProcess { uint32_t pid; uint64_t created; DWORD priority; };
std::vector<EfficientProcess> efficientProcesses;
HWND ecoButton = nullptr, perfAppButton = nullptr;
// Automatic power mode rules and the overrides they currently hold.
unsigned autoSaverThreshold = 0;          // percent; 0 = off
std::set<std::wstring> performanceApps;   // lowercase executable names
AutoMode autoApplied = AutoMode::none;
struct AutoOverride { bool active = false; PowerMode mode = PowerMode::balanced; GUID replaced{}; };
AutoOverride autoAc, autoDc;
uint64_t performanceSeenAt = 0;
// Battery mascot in the header, the mini floating window, and the share card.
bool showMascot = true, miniEnabled = false;
unsigned sampleTick = 0;
RECT mascotRect{};
RECT batteryCardsRect{}; // the two cards under the battery: a click opens its history
HWND miniWindow = nullptr;
const wchar_t* MiniClassName = L"BatteryScope.Mini.v1";
std::wstring lowerName(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return wchar_t(towlower(c)); });
    return text;
}
bool isEfficient(const ProcessRow& row) {
    return std::any_of(efficientProcesses.begin(), efficientProcesses.end(),
        [&](const EfficientProcess& p) { return p.pid == row.pid && p.created == row.created; });
}
bool performanceListed(const ProcessRow& row) { return !row.name.empty() && performanceApps.count(lowerName(row.name)) != 0; }
void refreshPowerModeControls();
void showLabPage();
void showSettingsPage();
void showDynamicPage();
void showPerformancePage();
int pluggedRefresh();
bool powerModeHeld();      // dynamic saving is leaving the battery power mode to the user
void showGoalPage();
void showHardwarePage();
// The power lab; absent in demo and preview modes.
std::unique_ptr<bs::lab::Controller> labController;
std::unique_ptr<bs::lab::WorkloadEngine> labEngine; // the automatic test's loads and window
bool autoTestActive = false;
// GPU engine use for the activity detector, sampled by the worker while sensing is on.
std::atomic<bool> governorGpu{false};
std::mutex governorGpuMutex;
bs::gov::GpuActivity governorGpuLatest;
uint64_t governorGpuAt = 0;
bs::gov::DetectionTally detectionTally; // activity detection scored during the automatic test
bs::gov::Detector detector;           // dynamic saving's activity detector (and its program rules)
uint64_t goalPageShownAt = 0;
// ASUS firmware state, read at most every 2 seconds.
bool asusAvailable = false;
bs::asus::Snapshot asusState;
uint64_t asusReadAt = 0;
const bs::asus::Snapshot& asusNow() {
    const uint64_t now = GetTickCount64();
    if (!asusReadAt || now - asusReadAt >= 2000) { asusState = bs::asus::read(); asusReadAt = now; }
    return asusState;
}
void flushDetectionLog();
std::string utf8(const std::wstring& text);
void exportLab();
// Learned charging curve and detected charge limit, saved under HKCU\Software\BatteryScope.
ChargeCurve chargeCurve;
std::optional<int> chargeLimit;
uint64_t curveLastMs = 0, curveSavedAt = 0;
bool curveDirty = false;
HICON trayIcon = nullptr;         // generated percentage icon (owned)
std::wstring trayIconKey;
InlinePage inlinePage = InlinePage::none;
bool inlineClosing = false;
std::wstring inlineTitle;
std::optional<ProcessRow> inlineProcess;
std::optional<ProcessRow> pendingForceRow;
std::wstring notice = tr(L"正在讀取電池與程序資料…");
uint64_t noticeUntil = 0;
std::deque<uint64_t> actionMarks;
constexpr UINT_PTR MotionTimer = 92, NoticeTimer = 93, UpdateTimer = 94, GovernorTimer = 95, KeyboardTimer = 96, AsusTimer = 97;
constexpr int PanelWidth = 360, PanelHeight = 640, RowHeight = 51;
bool motionEnabled = true, suppressMotion = false, panelTargetShown = false, motionTimerRunning = false;
double motionNow() {
    static const double frequency = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return double(f.QuadPart); }();
    LARGE_INTEGER counter{}; QueryPerformanceCounter(&counter);
    return counter.QuadPart * 1000.0 / frequency;
}
Transition panelMotion;
Transition panelHeightMotion;
int expandedPanelHeight = 0;
Transition scrollMotion;
double scrollPosition = 0;
bool scrollingThumb = false, listRefreshPending = false, applyingScroll = false;
void applyScroll(double position);
void stopScroll();
bool deferListRefresh();
double appliedPanelProgress = -1;
RECT panelRest{};
POINT panelOffset{};
struct RowMotion { uint32_t pid; uint64_t created; Transition bar, hover, selected; };
std::vector<RowMotion> rowMotions;
std::map<HWND, Transition> buttonMotions, pressMotions;
int hoveredRow = -1;
bool refreshingList = false, pointerDown = false;
HWND tooltip = nullptr;
std::wstring tooltipText;
bool recordMotion = false;
std::vector<double> motionTicks;
bool canAnimate() { return motionEnabled && !suppressMotion; }
void stepMotion(double now);
void refreshPowerButton();
std::wstring dynamicPill(); // "動態 · 閱讀" while dynamic saving runs on battery
void closeInlinePage(bool immediate = false);
void finishInlineClose();
void armMotionTimer() {
    if (!motionTimerRunning) motionTimerRunning = SetTimer(mainWindow, MotionTimer, USER_TIMER_MINIMUM, nullptr) != 0;
    if (!motionTimerRunning) {
        panelMotion.duration = 0;
        panelHeightMotion.duration = 0;
        scrollMotion.duration = 0;
        for (auto& row : rowMotions) row.bar.duration = row.hover.duration = row.selected.duration = 0;
        for (auto& button : buttonMotions) button.second.duration = 0;
        for (auto& button : pressMotions) button.second.duration = 0;
        stepMotion(motionNow());
    }
}
void readMotionPreference() {
    BOOL enabled = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0)) motionEnabled = enabled != FALSE;
}

int px(int value) { return MulDiv(value, int(dpi), 96); }
int sp(int value) { return px(MulDiv(value, 85, 100)); }
COLORREF blend(COLORREF a, COLORREF b, double t) { return ui::mix(a, b, t); }
std::wstring number(double value, int precision = 1) {
    std::wostringstream stream; stream << std::fixed << std::setprecision(precision) << value; return stream.str();
}
std::wstring valueText(Value value, const wchar_t* suffix, int precision = 1) {
    return value ? number(*value, precision) + suffix : L"—";
}
std::wstring qualityText(ObservationQuality quality) {
    switch (quality) {
    case ObservationQuality::strong: return tr(L"觀測完整");
    case ObservationQuality::partial: return tr(L"部分觀測");
    case ObservationQuality::limited: return tr(L"觀測有限");
    default: return tr(L"資料不足");
    }
}
std::wstring activityText(const ProcessRow& row) {
    std::wstring text;
    switch (row.activity) {
    case ActivityReason::limited: text = tr(L"統計受限"); break;
    case ActivityReason::warming: text = tr(L"採集中"); break;
    case ActivityReason::low: text = tr(L"活動低"); break;
    case ActivityReason::cpu: text = tr(L"CPU 活動"); break;
    case ActivityReason::io: text = tr(L"I/O 活動"); break;
    }
    const bool active = row.activity == ActivityReason::cpu || row.activity == ActivityReason::io;
    if (active && row.busySeconds >= 30) text += tr(L"・持續 ") + std::to_wstring(int(row.busySeconds)) + tr(L" 秒");
    if (row.services.size() > 1) text += tr(L"・共用程序");
    return text;
}
void font(HDC dc, HFONT value) { SelectObject(dc, value); }
HFONT fontFor(HFONT face, const std::wstring& text) {
    auto fallback = fallbackFonts.find(face);
    return fallback != fallbackFonts.end() && !ui::pixelCovers(text) ? fallback->second : face;
}
void drawText(HDC dc, const std::wstring& text, RECT r, COLORREF color, HFONT face, UINT format = DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS) {
    font(dc, fontFor(face, text)); SetTextColor(dc, color); SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text.c_str(), -1, &r, format | DT_NOPREFIX);
}
void fill(HDC dc, RECT rect, COLORREF color) {
    SetDCBrushColor(dc, color); FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}
// `radius` is the corner radius in logical pixels.
// Pixel grid in device pixels, and corner stair steps for a smooth-style radius.
int pixelUnit() { return std::max(2, px(2)); }
int pixelSteps(int radius) { return radius >= 9 ? 2 : 1; }
void rounded(HDC dc, RECT rect, COLORREF color, int radius = 12) {
    if (pixelStyle) ui::pixelFill(dc, rect, color, pixelUnit(), pixelSteps(radius));
    else ui::fillRound(dc, rect, color, float(sp(radius)));
}
void outline(HDC dc, RECT rect, COLORREF color, int radius, float width = 1.0f) {
    if (pixelStyle) ui::pixelFrame(dc, rect, color, pixelUnit(), pixelSteps(radius));
    else ui::strokeRound(dc, rect, color, float(sp(radius)), width);
}
// A surface card. Pixel style always outlines it; contrast themes outline it
// because fills alone may match the background.
void card(HDC dc, RECT rect, COLORREF color, int radius) {
    if (pixelStyle) { ui::pixelBox(dc, rect, color, theme.highContrast ? Border : blend(Border, Background, .15), pixelUnit(), 2); return; }
    rounded(dc, rect, color, radius);
    if (theme.highContrast) outline(dc, rect, Border, radius);
}
int textWidth(HDC dc, const std::wstring& text, HFONT face) {
    font(dc, fontFor(face, text)); SIZE size{}; GetTextExtentPoint32W(dc, text.c_str(), int(text.size()), &size); return size.cx;
}
// ----- Menus -----
// One menu look for the whole app instead of the Windows popup menu: the same
// surface, borders, fonts and pixel or smooth style as the panel, larger rows,
// a check pill for options that are on, and submenus that open in place with a
// back row (no flyouts drifting off the panel). Works with the mouse, the
// keyboard (arrows, Enter, Esc, Left/Backspace for back) and touch.
struct MenuItem {
    int id = 0;                       // command sent on choose; 0 for separators and submenus
    std::wstring label, hint;         // hint: a short value on the right, e.g. "開"
    bool checked = false, disabled = false, separator = false;
    std::vector<MenuItem> children;   // non-empty: a submenu
};
MenuItem menuItem(int id, const std::wstring& label, bool checked = false, bool disabled = false) {
    MenuItem item; item.id = id; item.label = label; item.checked = checked; item.disabled = disabled; return item;
}
MenuItem menuSeparator() { MenuItem item; item.separator = true; return item; }
MenuItem submenu(const std::wstring& label, std::vector<MenuItem> children, bool disabled = false) {
    MenuItem item; item.label = label; item.children = std::move(children); item.disabled = disabled; return item;
}
struct MenuState {
    std::vector<const std::vector<MenuItem>*> levels; // the open level is last
    std::vector<std::wstring> titles;
    int hover = -1, chosen = 0;
    bool done = false;
    double openedAt = -1;   // motionNow() when it opened: it fades in and settles from a little lower
    POINT rest{};
    const std::vector<MenuItem>& items() const { return *levels.back(); }
    bool nested() const { return levels.size() > 1; }
};
constexpr wchar_t PopupClass[] = L"BatteryScope.Menu.v1";
int menuRowHeight(const MenuItem& item) { return item.separator ? sp(11) : sp(36); }
int menuHeaderHeight(const MenuState& state) { return state.nested() ? sp(38) : 0; }
int menuPadding() { return sp(6); }
SIZE menuSize(HDC dc, const MenuState& state) {
    int width = sp(212), height = menuPadding() * 2 + menuHeaderHeight(state);
    for (const auto& item : state.items()) {
        height += menuRowHeight(item);
        if (!item.separator) width = std::max(width, textWidth(dc, item.label, normalFont) + sp(item.children.empty() ? 92 : 64));
    }
    if (state.nested()) width = std::max(width, textWidth(dc, state.titles.back(), strongFont) + sp(64));
    return {std::min(width, sp(340)), height};
}
// Row rectangles in client coordinates; index -1 is the back header.
RECT menuRow(const MenuState& state, int index, int width) {
    int y = menuPadding();
    if (index < 0) return {menuPadding(), y, width - menuPadding(), y + menuHeaderHeight(state)};
    y += menuHeaderHeight(state);
    for (int i = 0; i < index; ++i) y += menuRowHeight(state.items()[size_t(i)]);
    return {menuPadding(), y, width - menuPadding(), y + menuRowHeight(state.items()[size_t(index)])};
}
void drawMenuArrow(HDC dc, int x, int y, int size, COLORREF color, bool left) {
    // A small chevron; pixel style draws it from square cells.
    if (pixelStyle) {
        const int u = std::max(2, size / 4);
        for (int i = 0; i < 3; ++i) {
            const int cx = left ? x + (2 - i) * u : x + i * u;
            fill(dc, {cx, y + i * u, cx + u, y + i * u + u}, color);
            fill(dc, {cx, y + (4 - i) * u, cx + u, y + (4 - i) * u + u}, color);
        }
        return;
    }
    HPEN pen = CreatePen(PS_SOLID, std::max(1, sp(2)), color); HGDIOBJ old = SelectObject(dc, pen);
    const int h = size / 2;
    if (left) { MoveToEx(dc, x + h, y, nullptr); LineTo(dc, x, y + h); LineTo(dc, x + h + 1, y + size + 1); }
    else { MoveToEx(dc, x, y, nullptr); LineTo(dc, x + h, y + h); LineTo(dc, x - 1, y + size + 1); }
    SelectObject(dc, old); DeleteObject(pen);
}
void paintMenu(HDC dc, const MenuState& state, RECT client) {
    const int width = client.right - client.left;
    fill(dc, client, Background);
    card(dc, client, Surface, 12);
    if (!pixelStyle) outline(dc, client, blend(Border, Surface, .2), 12);
    if (state.nested()) {
        const RECT header = menuRow(state, -1, width);
        if (state.hover == -2) rounded(dc, header, blend(Surface, Selected, .55), 8);
        drawMenuArrow(dc, header.left + sp(12), (header.top + header.bottom) / 2 - sp(5), sp(10), Muted, true);
        drawText(dc, state.titles.back(), {header.left + sp(30), header.top, header.right - sp(10), header.bottom}, Ink, pixelStyle ? normalFont : strongFont);
        fill(dc, {header.left + sp(4), header.bottom - 1, header.right - sp(4), header.bottom}, Border);
    }
    for (int i = 0; i < int(state.items().size()); ++i) {
        const auto& item = state.items()[size_t(i)];
        const RECT row = menuRow(state, i, width);
        if (item.separator) { fill(dc, {row.left + sp(8), (row.top + row.bottom) / 2, row.right - sp(8), (row.top + row.bottom) / 2 + 1}, Border); continue; }
        const bool hot = i == state.hover && !item.disabled;
        if (hot) rounded(dc, {row.left, row.top + sp(1), row.right, row.bottom - sp(1)}, theme.highContrast ? Selected : blend(Surface, Selected, .6), 8);
        const COLORREF text = item.disabled ? blend(Muted, Surface, .35) : hot && theme.highContrast ? theme.selectedInk : Ink;
        int right = row.right - sp(12);
        if (!item.children.empty()) { drawMenuArrow(dc, right - sp(6), (row.top + row.bottom) / 2 - sp(5), sp(10), item.disabled ? text : Muted, false); right -= sp(18); }
        else if (item.checked || !item.hint.empty()) {
            // An "on" pill for checked options, or the item's value.
            const std::wstring value = !item.hint.empty() ? item.hint : std::wstring(tr(L"開"));
            const int pillWidth = std::max(sp(34), textWidth(dc, value, smallFont) + sp(16));
            const RECT pill{right - pillWidth, row.top + sp(8), right, row.bottom - sp(8)};
            rounded(dc, pill, item.checked ? blend(Green, Surface, theme.highContrast ? 0 : .25) : blend(Surface, Border, .9), 6);
            drawText(dc, value, pill, item.checked ? theme.accentInk : Muted, smallFont, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            right = pill.left - sp(8);
        }
        drawText(dc, item.label, {row.left + sp(12), row.top, right, row.bottom}, text, normalFont);
    }
}
LRESULT CALLBACK menuProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<MenuState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    auto hitTest = [&](POINT pt) {
        RECT client{}; GetClientRect(window, &client);
        if (state->nested()) { const RECT header = menuRow(*state, -1, client.right); if (PtInRect(&header, pt)) return -2; }
        for (int i = 0; i < int(state->items().size()); ++i) { const RECT row = menuRow(*state, i, client.right); if (PtInRect(&row, pt)) return i; }
        return -1;
    };
    switch (message) {
    case WM_TIMER: {
        if (!state || wp != 2) break;
        const double t = state->openedAt < 0 ? 1 : std::clamp((motionNow() - state->openedAt) / 140.0, 0.0, 1.0);
        const double eased = 1 - (1 - t) * (1 - t) * (1 - t);
        SetLayeredWindowAttributes(window, 0, BYTE(std::lround(eased * 255)), LWA_ALPHA);
        RECT bounds{}; GetWindowRect(window, &bounds);
        const int y = state->rest.y + int(std::lround((1 - eased) * sp(6)));
        if (bounds.top != y) SetWindowPos(window, nullptr, bounds.left, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        if (t >= 1) KillTimer(window, 2);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps{}; HDC target = BeginPaint(window, &ps); RECT r{}; GetClientRect(window, &r);
        HDC dc = CreateCompatibleDC(target); HBITMAP bitmap = CreateCompatibleBitmap(target, r.right, r.bottom);
        HGDIOBJ old = SelectObject(dc, bitmap);
        if (state) paintMenu(dc, *state, r);
        BitBlt(target, 0, 0, r.right, r.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc); EndPaint(window, &ps); return 0;
    }
    case WM_MOUSEMOVE: {
        if (!state) break;
        const int hit = hitTest({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        if (hit != state->hover) { state->hover = hit; InvalidateRect(window, nullptr, FALSE); }
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0}; TrackMouseEvent(&tracking);
        return 0;
    }
    case WM_MOUSELEAVE: if (state && state->hover >= 0) { state->hover = -1; InvalidateRect(window, nullptr, FALSE); } return 0;
    case WM_LBUTTONUP: case WM_RBUTTONUP: {
        if (!state) break;
        const int hit = hitTest({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        if (hit == -2) { PostMessageW(window, WM_KEYDOWN, VK_LEFT, 0); return 0; }
        if (hit >= 0) { state->hover = hit; PostMessageW(window, WM_KEYDOWN, VK_RETURN, 0); }
        return 0;
    }
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: {
        // With the mouse captured, a press outside the menu arrives here too and closes it.
        RECT client{}; GetClientRect(window, &client);
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (state && !PtInRect(&client, pt)) { state->done = true; PostMessageW(window, WM_NULL, 0, 0); }
        return 0;
    }
    case WM_ACTIVATE: case WM_CAPTURECHANGED: // switched to another window, or capture lost
        if (state && (message == WM_CAPTURECHANGED ? HWND(lp) != window : LOWORD(wp) == WA_INACTIVE)) {
            state->done = true; PostMessageW(window, WM_NULL, 0, 0);
        }
        return 0;
    case WM_KEYDOWN: {
        if (!state) break;
        const auto& items = state->items();
        auto move = [&](int step) {
            for (int n = 0, i = state->hover; n < int(items.size()); ++n) {
                i = (i + step + int(items.size())) % int(items.size());
                if (!items[size_t(i)].separator && !items[size_t(i)].disabled) { state->hover = i; break; }
            }
        };
        if (wp == VK_DOWN) move(1);
        else if (wp == VK_UP) move(state->hover < 0 ? 0 : -1);
        else if (wp == VK_ESCAPE && !state->nested()) state->done = true;
        else if ((wp == VK_LEFT || wp == VK_BACK || wp == VK_ESCAPE) && state->nested()) {
            state->levels.pop_back(); state->titles.pop_back(); state->hover = -1;
        } else if ((wp == VK_RETURN || wp == VK_SPACE || wp == VK_RIGHT) && state->hover >= 0 && state->hover < int(items.size())) {
            const auto& item = items[size_t(state->hover)];
            if (item.disabled || item.separator) return 0;
            if (!item.children.empty()) { state->levels.push_back(&item.children); state->titles.push_back(item.label); state->hover = -1; }
            else if (wp != VK_RIGHT) { state->chosen = item.id; state->done = true; }
        } else return 0;
        // Resize for the level now open, keeping the top-left corner in place.
        RECT bounds{}; GetWindowRect(window, &bounds);
        HDC dc = GetDC(window); const SIZE size = menuSize(dc, *state); ReleaseDC(window, dc);
        MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor);
        GetMonitorInfoW(MonitorFromRect(&bounds, MONITOR_DEFAULTTONEAREST), &monitor);
        const int top = std::min(bounds.top, monitor.rcWork.bottom - size.cy), left = std::min(bounds.left, monitor.rcWork.right - size.cx);
        state->rest = {left, top};
        SetWindowPos(window, nullptr, left, top, size.cx, size.cy, SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    }
    return DefWindowProcW(window, message, wp, lp);
}
// Shows the menu at a screen point (opening upward or leftward to stay on
// screen) and returns the chosen command, or 0. Runs its own message loop.
int showMenu(const std::vector<MenuItem>& items, POINT at, HWND owner) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = menuProc; wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = PopupClass; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.style = CS_DROPSHADOW;
        registered = RegisterClassExW(&wc) != 0;
    }
    MenuState state; state.levels.push_back(&items); state.titles.push_back(L"");
    HDC screen = GetDC(nullptr); const SIZE size = menuSize(screen, state); ReleaseDC(nullptr, screen);
    MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST), &monitor);
    const RECT work = monitor.rcWork;
    int x = at.x, y = at.y;
    if (x + size.cx > work.right) x = std::max(work.left, at.x - size.cx);
    if (y + size.cy > work.bottom) y = std::max(work.top, at.y - size.cy);
    const bool animate = canAnimate();
    HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | (animate ? WS_EX_LAYERED : 0), PopupClass, L"", WS_POPUP, x, y, size.cx, size.cy, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window) return 0;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));
    state.rest = {x, y};
    if (animate) {
        state.openedAt = motionNow();
        SetLayeredWindowAttributes(window, 0, 0, LWA_ALPHA);
        SetWindowPos(window, nullptr, x, y + sp(6), 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        SetTimer(window, 2, USER_TIMER_MINIMUM, nullptr);
    }
    if (!pixelStyle) { DWORD corners = 2; DwmSetWindowAttribute(window, 33, &corners, sizeof(corners)); }
    ShowWindow(window, SW_SHOW); SetForegroundWindow(window); SetFocus(window); SetCapture(window);
    MSG message;
    while (!state.done && GetMessageW(&message, nullptr, 0, 0)) {
        // A click anywhere else closes the menu without passing the click on.
        if ((message.message == WM_LBUTTONDOWN || message.message == WM_RBUTTONDOWN || message.message == WM_NCLBUTTONDOWN) && message.hwnd != window) { state.done = true; break; }
        if (message.message == WM_KEYDOWN && message.hwnd != window) message.hwnd = window;
        TranslateMessage(&message); DispatchMessageW(&message);
        if (message.message == WM_QUIT) { PostQuitMessage(int(message.wParam)); break; }
    }
    SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    if (GetCapture() == window) ReleaseCapture();
    DestroyWindow(window);
    return state.chosen;
}
// The same rendering into a bitmap file, for screenshots and checks.
bool captureMenu(const std::vector<MenuItem>& items, const std::wstring& path, int open) {
    MenuState state; state.levels.push_back(&items); state.titles.push_back(L"");
    if (open >= 0 && open < int(items.size()) && !items[size_t(open)].children.empty()) {
        state.levels.push_back(&items[size_t(open)].children); state.titles.push_back(items[size_t(open)].label);
    }
    state.hover = 1;
    HDC screen = GetDC(nullptr); const SIZE size = menuSize(screen, state);
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(info.bmiHeader); info.bmiHeader.biWidth = size.cx; info.bmiHeader.biHeight = size.cy;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    void* pixels = nullptr; HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HGDIOBJ old = SelectObject(dc, bitmap);
    paintMenu(dc, state, {0, 0, size.cx, size.cy}); GdiFlush();
    BITMAPFILEHEADER file{}; file.bfType = 0x4d42; file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER); file.bfSize = file.bfOffBits + size.cx * size.cy * 4;
    std::ofstream output(std::filesystem::path(path), std::ios::binary);
    output.write(reinterpret_cast<char*>(&file), sizeof(file)); output.write(reinterpret_cast<char*>(&info.bmiHeader), sizeof(info.bmiHeader));
    output.write(static_cast<char*>(pixels), size.cx * size.cy * 4);
    const bool ok = output.good();
    SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc); ReleaseDC(nullptr, screen);
    return ok;
}
// ----- Chart pages -----
// Pages built from blocks instead of paragraphs: meters against a goal, a live
// series, ranked bars (optionally stacked, with a ± range), status chips, a
// colour legend and short notes. One renderer draws them in the panel's style
// (pixel or smooth, dark or light) in a scrollable child window.
struct VSegment { double amount = 0; COLORREF color = 0; };
struct VBar {
    std::wstring label, value;
    double amount = 0, whisker = 0;     // whisker: ± range drawn around the end of the bar
    COLORREF color = 0;                 // 0: accent
    std::vector<VSegment> segments;     // stacked parts instead of one fill
};
struct VBlock {
    enum class Kind { heading, note, meter, bars, series, chips, legend, action, segments } kind = Kind::note;
    std::wstring title, text, value;
    double amount = 0, goal = 0, scale = 0;
    std::vector<VBar> bars;
    std::vector<double> series;
    std::vector<std::pair<std::wstring, COLORREF>> items; // chips (colour 0 = neutral) and legend entries
    int toggle = -1;                    // action rows: -1 a command or menu, 0/1 a switch that is off/on; segments: the selected one
    std::vector<int> commands;          // segments: one command per choice
};
VBlock vHeading(const std::wstring& title) { VBlock b; b.kind = VBlock::Kind::heading; b.title = title; return b; }
VBlock vNote(const std::wstring& text) { VBlock b; b.kind = VBlock::Kind::note; b.text = text; return b; }
VBlock vMeter(const std::wstring& title, const std::wstring& value, double amount, double goal, double scale) {
    VBlock b; b.kind = VBlock::Kind::meter; b.title = title; b.value = value; b.amount = amount; b.goal = goal; b.scale = scale; return b;
}
VBlock vBars(std::vector<VBar> bars, double scale = 0) { VBlock b; b.kind = VBlock::Kind::bars; b.bars = std::move(bars); b.scale = scale; return b; }
VBlock vSeries(const std::wstring& title, std::vector<double> values, double goal, const std::wstring& value) {
    VBlock b; b.kind = VBlock::Kind::series; b.title = title; b.series = std::move(values); b.goal = goal; b.value = value; return b;
}
VBlock vChips(std::vector<std::pair<std::wstring, COLORREF>> chips) { VBlock b; b.kind = VBlock::Kind::chips; b.items = std::move(chips); return b; }
VBlock vLegend(std::vector<std::pair<std::wstring, COLORREF>> entries) { VBlock b; b.kind = VBlock::Kind::legend; b.items = std::move(entries); return b; }
// A row that does something: a command, or a menu of choices opened at the row.
// `highlight` colours the value (e.g. green for a saving state).
VBlock vAction(const std::wstring& title, const std::wstring& value, int command, COLORREF highlight = 0) {
    VBlock b; b.kind = VBlock::Kind::action; b.title = title; b.value = value; b.amount = command; b.goal = highlight; return b;
}
// A row of choices, one of them selected (-1: none), like the performance mode.
VBlock vSegments(std::vector<std::wstring> labels, std::vector<int> commands, int selected) {
    VBlock b; b.kind = VBlock::Kind::segments; b.toggle = selected; b.commands = std::move(commands);
    for (auto& label : labels) b.items.push_back({std::move(label), 0});
    return b;
}
// A switch: clicking it runs `command`, which turns the setting on or off. `detail` is a short
// line under the title (what it does, or what it saves).
VBlock vToggle(const std::wstring& title, const std::wstring& detail, bool on, int command) {
    VBlock b; b.kind = VBlock::Kind::action; b.title = title; b.text = detail; b.amount = command; b.toggle = on ? 1 : 0; return b;
}

HWND visualView = nullptr;
std::vector<VBlock> visualBlocks;
int visualScroll = 0, visualContent = 0, visualDragOffset = -1;
// Action rows as last painted (view coordinates) and the one under the mouse or keyboard focus.
struct VHit { RECT rect; int command; size_t block; };
std::vector<VHit> visualHits;
int visualHover = -1, visualFocus = -1;
POINT actionAnchor{}; bool actionAnchored = false; // where a menu opened by an action row appears
POINT menuAnchor() {
    if (actionAnchored) { actionAnchored = false; return actionAnchor; }
    POINT p{}; GetCursorPos(&p); return p;
}
int chipRowsHeight(HDC dc, const VBlock& b, int width) {
    int x = 0, rows = 1;
    for (const auto& [label, color] : b.items) {
        const int w = textWidth(dc, label, smallFont) + sp(20);
        if (x && x + w > width) { ++rows; x = 0; }
        x += w + sp(6);
    }
    return rows * sp(28);
}
int noteHeight(HDC dc, const std::wstring& text, int width) {
    RECT r{0, 0, width, 0};
    font(dc, fontFor(smallFont, text));
    DrawTextW(dc, text.c_str(), -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    return r.bottom + sp(8);
}
int blockHeight(HDC dc, const VBlock& b, int width, bool first) {
    switch (b.kind) {
    case VBlock::Kind::heading: return first ? sp(26) : sp(36);
    case VBlock::Kind::note: return noteHeight(dc, b.text, width);
    case VBlock::Kind::meter: return sp(48);
    case VBlock::Kind::bars: return int(b.bars.size()) * sp(36) + sp(2);
    case VBlock::Kind::series: return sp(112);
    case VBlock::Kind::chips: return chipRowsHeight(dc, b, width) + sp(4);
    case VBlock::Kind::legend: return sp(24);
    case VBlock::Kind::action: return b.text.empty() ? sp(44) : sp(56);
    case VBlock::Kind::segments: return sp(46);
    }
    return 0;
}
// ----- Chart page motion -----
// A page opening lets its blocks rise and fade in one after another while bars and meters
// grow to their values; a live refresh glides bars to their new values; switches slide their
// knob; rows light up and dim under the pointer. All of it stops under reduced motion.
constexpr UINT_PTR VisualTimer = 1;
double visualEnterAt = -1;                                // motionNow() when the page opened
std::map<std::wstring, Transition> visualValues;          // bar and meter fractions, by block and label
std::map<int, Transition> visualKnobs;                    // switches, by command
std::map<size_t, Transition> visualHoverFade, visualPress; // rows, by block index
bool visualAnimating = false, visualTimerOn = false;
double paintFade = 1;                                      // the block being painted, 0 (hidden) to 1
COLORREF faded(COLORREF c) { return paintFade >= 1 ? c : blend(Surface, c, paintFade); }
double easeOut(double t) { t = std::clamp(t, 0.0, 1.0); return 1 - (1 - t) * (1 - t) * (1 - t); }
void armVisualTimer() {
    if (visualView && !visualTimerOn) visualTimerOn = SetTimer(visualView, VisualTimer, USER_TIMER_MINIMUM, nullptr) != 0;
}
// How far block `index` has come in (0-1) while the page opens.
double blockAppear(size_t index, double now) {
    if (visualEnterAt < 0) return 1;
    const double t = (now - visualEnterAt - 32.0 * double(std::min<size_t>(index, 12))) / 300.0;
    if (t < 1) visualAnimating = true;
    return easeOut(t);
}
// A value that glides to `target` when it changes (bars and meters on a live page).
double glide(const std::wstring& key, double target, double now) {
    auto [it, fresh] = visualValues.try_emplace(key);
    auto& m = it->second;
    if (fresh) { m.from = m.to = target; m.duration = 0; }
    else if (std::abs(m.to - target) > .0005) m.move(target, now, canAnimate() ? 420 : 0);
    if (m.active(now)) visualAnimating = true;
    return m.value(now);
}
double fadeValue(std::map<size_t, Transition>& map, size_t key, double now) {
    auto it = map.find(key);
    if (it == map.end()) return 0;
    if (it->second.active(now)) visualAnimating = true;
    return it->second.value(now);
}
void setFade(std::map<size_t, Transition>& map, size_t key, double target, double ms) {
    auto& m = map[key];
    m.move(target, motionNow(), canAnimate() ? ms : 0);
    armVisualTimer();
}
// A switch: a track with a knob, in the panel's style. `on` runs from 0 (off) to 1 (on).
void drawSwitch(HDC dc, RECT r, double on) {
    const COLORREF off = blend(Surface, Border, .9), lit = Green;
    const COLORREF track = faded(blend(off, lit, on));
    const int h = r.bottom - r.top, pad = std::max(2, h / 7), knob = h - pad * 2;
    const int x = r.left + pad + int(std::lround((r.right - r.left - pad * 2 - knob) * on));
    const COLORREF knobColor = faded(blend(blend(Surface, Ink, .55), theme.dark ? Ink : Surface, on));
    if (pixelStyle) {
        ui::pixelFill(dc, r, track, pixelUnit(), 1);
        ui::pixelFill(dc, {x, r.top + pad, x + knob, r.top + pad + knob}, knobColor, pixelUnit(), 1);
    } else {
        ui::fillRound(dc, r, track, float(h) / 2);
        ui::fillRound(dc, {x, r.top + pad, x + knob, r.top + pad + knob}, knobColor, float(knob) / 2);
    }
}
COLORREF barColor(COLORREF color) { return color ? faded(color) : Green; }
void paintVisual(HDC dc, RECT client) {
    fill(dc, client, Surface);
    const int width = client.right - sp(10); // room for the scroll indicator
    const double now = motionNow();
    visualAnimating = false;
    if (visualEnterAt >= 0 && now - visualEnterAt > 32.0 * 12 + 300) visualEnterAt = -1;
    int y = -visualScroll;
    visualHits.clear();
    // Each block draws with the shared colours faded by how far it has come in.
    const COLORREF inkBase = Ink, mutedBase = Muted, greenBase = Green, borderBase = Border, selectedBase = Selected, warnBase = theme.warn;
    for (size_t i = 0; i < visualBlocks.size(); ++i) {
        const auto& b = visualBlocks[i];
        const int h = blockHeight(dc, b, width, i == 0);
        const double appear = blockAppear(i, now);
        const int rise = int(std::lround((1 - appear) * sp(14)));
        if (y + h < 0 || y > client.bottom) {
            if (b.kind == VBlock::Kind::action) visualHits.push_back({{0, y + sp(3), width, y + h - sp(3)}, int(b.amount), i});
            if (b.kind == VBlock::Kind::segments)
                for (size_t k = 0; k < b.commands.size(); ++k) visualHits.push_back({{0, y + sp(4), width, y + h - sp(4)}, b.commands[k], i});
            y += h; continue;
        }
        paintFade = appear;
        Ink = faded(inkBase); Muted = faded(mutedBase); Green = faded(greenBase); Border = faded(borderBase); Selected = faded(selectedBase); theme.warn = faded(warnBase);
        const int top = y + rise;
        const COLORREF track = blend(Surface, Border, .75);
        switch (b.kind) {
        case VBlock::Kind::heading: {
            // A short accent mark leads each section.
            const int mid = top + h - sp(14);
            rounded(dc, {0, mid - sp(6), sp(4), mid + sp(6)}, Green, 2);
            drawText(dc, b.title, {sp(10), top + h - sp(24), width, top + h - sp(4)}, Ink, strongFont);
            break;
        }
        case VBlock::Kind::note: {
            RECT r{0, top, width, top + h};
            drawText(dc, b.text, r, Muted, smallFont, DT_LEFT | DT_WORDBREAK);
            break;
        }
        case VBlock::Kind::meter: {
            // "In use   12.4 W" with a bar to scale and a marker at the goal.
            drawText(dc, b.title, {0, top, width / 2, top + sp(20)}, Ink, normalFont);
            const bool over = b.goal > 0 && b.amount > b.goal;
            drawText(dc, b.value, {width / 2, top, width, top + sp(20)}, b.amount <= 0 ? Muted : over ? theme.warn : Green, normalFont, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
            const RECT bar{0, top + sp(25), width, top + sp(37)};
            rounded(dc, bar, track, 6);
            const double scale = b.scale > 0 ? b.scale : std::max(b.goal * 1.6, b.amount * 1.1);
            if (b.amount > 0 && scale > 0) {
                const double fraction = glide(std::to_wstring(i) + L"|" + b.title, std::min(1.0, b.amount / scale), now) * appear;
                const int end = int(fraction * (bar.right - bar.left));
                if (end > 0) rounded(dc, {bar.left, bar.top, bar.left + std::max(sp(6), end), bar.bottom}, over ? theme.warn : Green, 6);
            }
            if (b.goal > 0 && scale > 0) {
                const int x = bar.left + int(std::min(1.0, b.goal / scale) * (bar.right - bar.left));
                fill(dc, {x - sp(1), bar.top - sp(4), x + sp(1), bar.bottom + sp(4)}, Ink);
            }
            break;
        }
        case VBlock::Kind::bars: {
            double scale = b.scale;
            for (const auto& bar : b.bars) {
                double total = bar.amount;
                if (!bar.segments.empty()) { total = 0; for (const auto& s : bar.segments) total += s.amount; }
                scale = std::max(scale, total + bar.whisker);
            }
            int row = top;
            for (const auto& bar : b.bars) {
                const int valueWidth = std::min(width / 3, textWidth(dc, bar.value, smallFont) + sp(4));
                drawText(dc, bar.label, {0, row, width - valueWidth - sp(6), row + sp(18)}, Ink, smallFont);
                drawText(dc, bar.value, {width - valueWidth, row, width, row + sp(18)}, Muted, smallFont, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
                const RECT track_{0, row + sp(21), width, row + sp(30)};
                rounded(dc, track_, track, 4);
                const double span = double(track_.right - track_.left);
                if (scale > 0) {
                    if (bar.segments.empty()) {
                        const double fraction = glide(std::to_wstring(i) + L"|" + bar.label, std::clamp(bar.amount / scale, 0.0, 1.0), now) * appear;
                        const int end = int(fraction * span);
                        if (end > 0) rounded(dc, {track_.left, track_.top, track_.left + std::max(sp(4), end), track_.bottom}, barColor(bar.color), 4);
                        if (bar.whisker > 0 && appear >= 1) {
                            const int a = track_.left + int(std::clamp((bar.amount - bar.whisker) / scale, 0.0, 1.0) * span);
                            const int z = track_.left + int(std::clamp((bar.amount + bar.whisker) / scale, 0.0, 1.0) * span);
                            const int mid = (track_.top + track_.bottom) / 2;
                            fill(dc, {a, mid - 1, z, mid + 1}, Ink);
                            fill(dc, {a, track_.top - sp(2), a + sp(2), track_.bottom + sp(2)}, Ink);
                            fill(dc, {z - sp(2), track_.top - sp(2), z, track_.bottom + sp(2)}, Ink);
                        }
                    } else {
                        int x = track_.left;
                        for (const auto& s : bar.segments) {
                            const int w = int(std::clamp(s.amount / scale, 0.0, 1.0) * span * appear);
                            if (w > 0) fill(dc, {x, track_.top, std::min<int>(track_.right, x + w), track_.bottom}, faded(s.color));
                            x += w;
                        }
                    }
                }
                row += sp(36);
            }
            break;
        }
        case VBlock::Kind::series: {
            drawText(dc, b.title, {0, top, width / 2, top + sp(20)}, Ink, normalFont);
            drawText(dc, b.value, {width / 2, top, width, top + sp(20)}, Muted, smallFont, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
            const RECT plot{0, top + sp(26), width, top + sp(104)};
            rounded(dc, plot, blend(Surface, Background, .5), 8);
            double peak = b.goal * 1.5;
            for (double v : b.series) peak = std::max(peak, v * 1.1);
            if (b.series.size() >= 2 && peak > 0) {
                const int left = plot.left + sp(6), right = plot.right - sp(6), bottom = plot.bottom - sp(6), height = plot.bottom - plot.top - sp(12);
                // Columns, one per sample, coloured by whether they are over the goal; on opening they
                // rise from left to right.
                const double step = double(right - left) / double(b.series.size());
                for (size_t k = 0; k < b.series.size(); ++k) {
                    const double sweep = std::clamp(appear * 1.6 - double(k) / double(b.series.size()) * .6, 0.0, 1.0);
                    const int x0 = left + int(k * step), x1 = std::max(x0 + 1, left + int((k + 1) * step) - (step > sp(3) ? 1 : 0));
                    const int hgt = int(std::clamp(b.series[k] / peak, 0.0, 1.0) * height * easeOut(sweep));
                    fill(dc, {x0, bottom - hgt, x1, bottom}, b.goal > 0 && b.series[k] > b.goal ? blend(theme.warn, Surface, .15) : blend(Green, Surface, .15));
                }
                if (b.goal > 0) {
                    const int gy = bottom - int(std::clamp(b.goal / peak, 0.0, 1.0) * height);
                    for (int x = left; x < right; x += sp(8)) fill(dc, {x, gy, std::min(right, x + sp(4)), gy + std::max(1, sp(1))}, Ink);
                }
            } else drawText(dc, tr(L"等待資料"), plot, Muted, smallFont, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            break;
        }
        case VBlock::Kind::chips: {
            int x = 0, row = top;
            for (const auto& [label, color] : b.items) {
                const int w = textWidth(dc, label, smallFont) + sp(20);
                if (x && x + w > width) { x = 0; row += sp(28); }
                const RECT pill{x, row, x + w, row + sp(24)};
                const COLORREF tone = faded(color);
                rounded(dc, pill, color ? blend(tone, Surface, .72) : blend(Surface, Border, .6), 8);
                drawText(dc, label, pill, color ? (theme.dark ? Ink : blend(tone, Ink, .5)) : Muted, smallFont, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
                x += w + sp(6);
            }
            break;
        }
        case VBlock::Kind::action: {
            const RECT row{0, top + sp(3), width, top + h - sp(3)};
            const int index = int(visualHits.size());
            visualHits.push_back({{0, y + sp(3), width, y + h - sp(3)}, int(b.amount), i});
            const double hot = std::max(fadeValue(visualHoverFade, i, now), index == visualFocus ? 1.0 : 0.0);
            const double press = fadeValue(visualPress, i, now);
            const COLORREF rest = blend(Surface, Background, .55), lit = blend(Surface, Selected, theme.highContrast ? 1.0 : .6);
            RECT face = row;
            if (press > 0) InflateRect(&face, -int(std::lround(press * sp(2))), -int(std::lround(press * sp(1))));
            rounded(dc, face, blend(blend(rest, lit, hot), Selected, press * .35), 8);
            if (index == visualFocus && GetFocus() == visualView) outline(dc, face, Green, 8);
            const COLORREF ink = hot > .5 && theme.highContrast ? theme.selectedInk : Ink;
            int right = row.right - sp(12);
            if (b.toggle >= 0) {
                // The switch, its knob gliding to the new state.
                auto [it, fresh] = visualKnobs.try_emplace(int(b.amount));
                if (fresh) { it->second.from = it->second.to = b.toggle; it->second.duration = 0; }
                else if (std::abs(it->second.to - b.toggle) > .01) it->second.move(b.toggle, now, canAnimate() ? 200 : 0);
                if (it->second.active(now)) visualAnimating = true;
                const int switchHeight = sp(20), switchWidth = sp(38), mid = (row.top + row.bottom) / 2;
                drawSwitch(dc, {right - switchWidth, mid - switchHeight / 2, right, mid + switchHeight / 2}, it->second.value(now));
                right -= switchWidth + sp(10);
            } else {
                drawMenuArrow(dc, right - sp(6), (row.top + row.bottom) / 2 - sp(5), sp(10), Muted, false);
                right -= sp(18);
                if (!b.value.empty()) {
                    const int pillWidth = std::min(width / 2, textWidth(dc, b.value, smallFont) + sp(16));
                    const RECT pill{right - pillWidth, row.top + sp(8), right, row.bottom - sp(8)};
                    const COLORREF tone = faded(COLORREF(b.goal));
                    rounded(dc, pill, b.goal ? blend(tone, Surface, .72) : blend(Surface, Border, .6), 6);
                    drawText(dc, b.value, pill, b.goal ? (theme.dark ? Ink : blend(tone, Ink, .5)) : Muted, smallFont, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                    right = pill.left - sp(8);
                }
            }
            if (b.text.empty()) drawText(dc, b.title, {row.left + sp(12), row.top, right, row.bottom}, ink, normalFont);
            else {
                const int mid = (row.top + row.bottom) / 2;
                drawText(dc, b.title, {row.left + sp(12), mid - sp(19), right, mid + sp(1)}, ink, normalFont);
                drawText(dc, b.text, {row.left + sp(12), mid + sp(1), right, mid + sp(19)}, Muted, smallFont);
            }
            break;
        }
        case VBlock::Kind::segments: {
            // A track with one cell per choice; the selected one's highlight slides to a new choice.
            const int n = int(b.commands.size());
            const RECT bar{0, top + sp(4), width, top + h - sp(4)};
            rounded(dc, bar, blend(Surface, Background, .6), 10);
            if (!n) break;
            const int pad = sp(3), cell = (bar.right - bar.left - pad * 2) / n;
            if (b.toggle >= 0) {
                auto [it, fresh] = visualKnobs.try_emplace(100000 + int(i));
                if (fresh) { it->second.from = it->second.to = b.toggle; it->second.duration = 0; }
                else if (std::abs(it->second.to - b.toggle) > .01) it->second.move(b.toggle, now, canAnimate() ? 240 : 0);
                if (it->second.active(now)) visualAnimating = true;
                const int left = bar.left + pad + int(std::lround(it->second.value(now) * cell));
                rounded(dc, {left, bar.top + pad, left + cell, bar.bottom - pad}, theme.highContrast ? Selected : faded(theme.accentSoft), 8);
            }
            for (int k = 0; k < n; ++k) {
                const RECT face{bar.left + pad + k * cell, bar.top + pad, bar.left + pad + (k + 1) * cell, bar.bottom - pad};
                const int index = int(visualHits.size());
                visualHits.push_back({{face.left, y + sp(4) + pad, face.right, y + h - sp(4) - pad}, b.commands[size_t(k)], i});
                const bool on = k == b.toggle;
                if ((index == visualHover || index == visualFocus) && !on) rounded(dc, face, blend(Surface, Selected, .5), 8);
                if (index == visualFocus && GetFocus() == visualView) outline(dc, face, Green, 8);
                drawText(dc, b.items[size_t(k)].first, face, on ? (theme.highContrast ? theme.selectedInk : Green) : Ink, on ? strongFont : normalFont,
                    DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            }
            break;
        }
        case VBlock::Kind::legend: {
            int x = 0;
            for (const auto& [label, color] : b.items) {
                fill(dc, {x, top + sp(7), x + sp(10), top + sp(17)}, faded(color));
                const int w = textWidth(dc, label, smallFont);
                drawText(dc, label, {x + sp(14), top, x + sp(14) + w + sp(2), top + sp(24)}, Muted, smallFont);
                x += sp(14) + w + sp(14);
            }
            break;
        }
        }
        Ink = inkBase; Muted = mutedBase; Green = greenBase; Border = borderBase; Selected = selectedBase; theme.warn = warnBase;
        paintFade = 1;
        y += h;
    }
    visualContent = y + visualScroll;
    // A thin scroll indicator on the right when the page is taller than the view.
    if (visualContent > client.bottom) {
        const int trackHeight = int(client.bottom), thumb = std::max(sp(24), trackHeight * trackHeight / visualContent);
        const int top = (trackHeight - thumb) * visualScroll / std::max(1, visualContent - trackHeight);
        rounded(dc, {client.right - sp(5), top, client.right - sp(1), top + thumb}, blend(Muted, Surface, .45), 4);
    }
    if (visualAnimating) armVisualTimer();
}
void scrollVisual(int to) {
    RECT r{}; GetClientRect(visualView, &r);
    const int limit = std::max(0, visualContent - int(r.bottom));
    const int next = std::clamp(to, 0, limit);
    if (next != visualScroll) { visualScroll = next; InvalidateRect(visualView, nullptr, FALSE); }
}
// The wheel and page keys glide instead of jumping; dragging the indicator stays direct.
Transition visualScrollMotion;
void scrollVisualSmooth(int to) {
    RECT r{}; GetClientRect(visualView, &r);
    const int target = std::clamp(to, 0, std::max(0, visualContent - int(r.bottom)));
    if (!canAnimate()) { visualScrollMotion.duration = 0; scrollVisual(target); return; }
    const double now = motionNow();
    if (!visualScrollMotion.active(now)) { visualScrollMotion.from = visualScrollMotion.to = visualScroll; visualScrollMotion.duration = 0; }
    visualScrollMotion.move(target, now, 190);
    armVisualTimer();
}
int visualScrollTarget() { return visualScrollMotion.active(motionNow()) ? int(std::lround(visualScrollMotion.to)) : visualScroll; }
LRESULT CALLBACK visualProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_TIMER:
        if (wp == VisualTimer) {
            bool scrolling = false;
            if (visualScrollMotion.duration) {
                const double now = motionNow();
                scrollVisual(int(std::lround(visualScrollMotion.value(now))));
                if (visualScrollMotion.active(now)) scrolling = true; else visualScrollMotion.duration = 0;
            }
            InvalidateRect(window, nullptr, FALSE); UpdateWindow(window);
            if ((!visualAnimating && !scrolling) || hidden) { KillTimer(window, VisualTimer); visualTimerOn = false; }
            return 0;
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps{}; HDC target = BeginPaint(window, &ps); RECT r{}; GetClientRect(window, &r);
        HDC dc = CreateCompatibleDC(target); HBITMAP bitmap = CreateCompatibleBitmap(target, r.right, std::max<LONG>(1, r.bottom));
        HGDIOBJ old = SelectObject(dc, bitmap);
        paintVisual(dc, r);
        BitBlt(target, 0, 0, r.right, r.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc); EndPaint(window, &ps);
        return 0;
    }
    case WM_MOUSEWHEEL: scrollVisualSmooth(visualScrollTarget() - GET_WHEEL_DELTA_WPARAM(wp) * sp(56) / WHEEL_DELTA); return 0;
    case WM_LBUTTONDOWN: {
        SetFocus(window);
        RECT r{}; GetClientRect(window, &r);
        if (visualContent > r.bottom && GET_X_LPARAM(lp) >= r.right - sp(12)) { // drag the indicator
            visualDragOffset = GET_Y_LPARAM(lp); SetCapture(window);
            scrollVisual(GET_Y_LPARAM(lp) * (visualContent - r.bottom) / std::max<LONG>(1, r.bottom));
            return 0;
        }
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        for (const auto& hit : visualHits) if (PtInRect(&hit.rect, pt)) { setFade(visualPress, hit.block, 1, 70); break; } // the row gives a little under the press
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (visualDragOffset >= 0 && GetCapture() == window) {
            RECT r{}; GetClientRect(window, &r);
            scrollVisual(GET_Y_LPARAM(lp) * (visualContent - r.bottom) / std::max<LONG>(1, r.bottom));
            return 0;
        }
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int hover = -1;
        for (size_t i = 0; i < visualHits.size(); ++i) if (PtInRect(&visualHits[i].rect, pt)) hover = int(i);
        if (hover != visualHover) {
            if (visualHover >= 0 && visualHover < int(visualHits.size())) setFade(visualHoverFade, visualHits[size_t(visualHover)].block, 0, 180);
            if (hover >= 0) setFade(visualHoverFade, visualHits[size_t(hover)].block, 1, 110);
            visualHover = hover; InvalidateRect(window, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0}; TrackMouseEvent(&tracking);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && visualHover >= 0) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }
        break;
    case WM_MOUSELEAVE:
        if (visualHover >= 0) {
            if (visualHover < int(visualHits.size())) setFade(visualHoverFade, visualHits[size_t(visualHover)].block, 0, 180);
            visualHover = -1; InvalidateRect(window, nullptr, FALSE);
        }
        for (auto& [block, press] : visualPress) if (press.to > 0) setFade(visualPress, block, 0, 200);
        return 0;
    case WM_LBUTTONUP: case WM_CAPTURECHANGED:
        for (auto& [block, press] : visualPress) if (press.to > 0) setFade(visualPress, block, 0, 220);
        if (message == WM_LBUTTONUP && visualDragOffset < 0) {
            const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            for (size_t i = 0; i < visualHits.size(); ++i)
                if (PtInRect(&visualHits[i].rect, pt)) {
                    visualFocus = int(i);
                    POINT anchor{visualHits[i].rect.left, visualHits[i].rect.bottom}; ClientToScreen(window, &anchor);
                    actionAnchor = anchor; actionAnchored = true;
                    PostMessageW(GetParent(window), WM_COMMAND, visualHits[i].command, 0);
                    break;
                }
        }
        visualDragOffset = -1; if (message == WM_LBUTTONUP) ReleaseCapture(); return 0;
    case WM_SETFOCUS: case WM_KILLFOCUS: InvalidateRect(window, nullptr, FALSE); return 0;
    case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTMESSAGE;
    case WM_KEYDOWN: {
        RECT r{}; GetClientRect(window, &r);
        // Arrows move between action rows (scrolling them into view); past the rows they scroll.
        auto focusRow = [&](int index) {
            visualFocus = index;
            const RECT& row = visualHits[size_t(index)].rect;
            if (row.top < 0) scrollVisual(visualScroll + row.top - sp(8));
            else if (row.bottom > r.bottom) scrollVisual(visualScroll + row.bottom - r.bottom + sp(8));
            InvalidateRect(window, nullptr, FALSE);
        };
        if ((wp == VK_RETURN || wp == VK_SPACE) && visualFocus >= 0 && visualFocus < int(visualHits.size())) {
            POINT anchor{visualHits[size_t(visualFocus)].rect.left, visualHits[size_t(visualFocus)].rect.bottom}; ClientToScreen(window, &anchor);
            actionAnchor = anchor; actionAnchored = true;
            PostMessageW(GetParent(window), WM_COMMAND, visualHits[size_t(visualFocus)].command, 0);
        }
        else if (wp == VK_ESCAPE) PostMessageW(GetParent(window), WM_COMMAND, IDCANCEL, 0);
        else if (wp == VK_DOWN && !visualHits.empty() && visualFocus + 1 < int(visualHits.size())) focusRow(visualFocus + 1);
        else if (wp == VK_UP && !visualHits.empty() && visualFocus > 0) focusRow(visualFocus - 1);
        else if (wp == VK_DOWN) scrollVisual(visualScroll + sp(40));
        else if (wp == VK_UP) scrollVisual(visualScroll - sp(40));
        else if (wp == VK_NEXT) scrollVisualSmooth(visualScrollTarget() + r.bottom - sp(40));
        else if (wp == VK_PRIOR) scrollVisualSmooth(visualScrollTarget() - r.bottom + sp(40));
        else if (wp == VK_HOME) scrollVisualSmooth(0);
        else if (wp == VK_END) scrollVisualSmooth(visualContent);
        else break;
        return 0;
    }
    case WM_SIZE: scrollVisual(visualScroll); return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}
// Shows chart blocks for the open page; `keepScroll` keeps the position on a live refresh.
void showVisual(std::vector<VBlock> blocks, bool keepScroll) {
    visualBlocks = std::move(blocks);
    if (!keepScroll) {
        visualScroll = 0; visualFocus = -1; visualHover = -1; visualScrollMotion.duration = 0;
        // A new page comes in: its blocks rise into place, bars grow, switches start where they are.
        visualValues.clear(); visualKnobs.clear(); visualHoverFade.clear(); visualPress.clear();
        visualEnterAt = canAnimate() ? motionNow() : -1;
        if (visualEnterAt >= 0) armVisualTimer();
    }
    if (!visualView) return;
    ShowWindow(inlineText, SW_HIDE);
    ShowWindow(visualView, SW_SHOW);
    InvalidateRect(visualView, nullptr, FALSE);
}

void showChartPage(InlinePage page, const std::wstring& title, const std::wstring& text, std::vector<VBlock> blocks);
void showHistoryPage();
std::vector<VBlock> ledgerBlocks();
std::wstring ledgerText();
std::wstring candidatesText();
// ----- Tabs -----
// The panel's sections along the bottom: the ranking (home), saving, battery,
// ASUS hardware (on ASUS laptops), the lab and settings. Each section is one page
// with live charts on top and its controls as rows below; the ⋯ menu keeps only
// quick switches and tools.
enum class Tab { ranking, saving, battery, hardware, lab, settings, performance };
int tabBarHeight() { return sp(54); }
std::vector<Tab> visibleTabs() {
    // Home is the performance dashboard; the battery tab holds the battery and the programs using it.
    std::vector<Tab> tabs{Tab::performance, Tab::saving, Tab::ranking};
    if (asusAvailable) tabs.push_back(Tab::hardware);
    tabs.push_back(Tab::lab); tabs.push_back(Tab::settings);
    return tabs;
}
const wchar_t* tabLabel(Tab tab) {
    // Short words of their own: a tab is a sixth of the panel wide.
    switch (tab) {
    case Tab::performance: return english ? L"Perf" : L"效能";
    case Tab::saving: return english ? L"Saving" : L"省電";
    case Tab::hardware: return english ? L"ASUS" : L"硬體";
    case Tab::lab: return english ? L"Lab" : L"實驗";
    case Tab::settings: return english ? L"Setup" : L"設定";
    default: return english ? L"Battery" : L"電池";
    }
}
bool sectionPage(InlinePage page) {
    return page == InlinePage::goal || page == InlinePage::history || page == InlinePage::ledger || page == InlinePage::hardware ||
        page == InlinePage::lab || page == InlinePage::settings || page == InlinePage::performance;
}
Tab activeTab() {
    switch (inlinePage) {
    case InlinePage::goal: case InlinePage::dynamic: case InlinePage::powerMode: return Tab::saving;
    case InlinePage::performance: return Tab::performance;
    case InlinePage::history: case InlinePage::ledger: case InlinePage::power: case InlinePage::candidates: return Tab::ranking;
    case InlinePage::hardware: return Tab::hardware;
    case InlinePage::lab: return Tab::lab;
    case InlinePage::settings: case InlinePage::perfApps: return Tab::settings;
    default: return Tab::ranking;
    }
}
int tabCommand(Tab tab) {
    switch (tab) {
    case Tab::saving: return GoalPageId;
    case Tab::performance: return PerfPageId;
    case Tab::battery: return HistoryId;
    case Tab::hardware: return HardwarePageId;
    case Tab::lab: return LabPageId;
    case Tab::settings: return SettingsPageId;
    default: return InlineBackId;
    }
}
RECT tabRect(size_t index, size_t count, RECT client) {
    const int left = sp(12), right = client.right - sp(12), top = client.bottom - tabBarHeight() + sp(4), bottom = client.bottom - sp(10);
    const int width = (right - left) / int(std::max<size_t>(1, count));
    return {left + int(index) * width, top, index + 1 == count ? right : left + int(index + 1) * width, bottom};
}
int tabHover = -1;
// The selected tab's highlight slides to the new tab (in tab positions), and labels ease
// between their plain and selected colours as it passes.
Transition tabSlide; bool tabSlideKnown = false;
void drawTabs(HDC dc, RECT client) {
    const auto tabs = visibleTabs();
    const RECT bar{sp(8), client.bottom - tabBarHeight(), client.right - sp(8), client.bottom - sp(6)};
    rounded(dc, bar, Surface, 12);
    if (theme.highContrast) outline(dc, bar, Border, 12);
    const Tab active = activeTab();
    size_t activeIndex = 0;
    for (size_t i = 0; i < tabs.size(); ++i) if (tabs[i] == active) activeIndex = i;
    const double now = motionNow();
    if (!tabSlideKnown) { tabSlide.from = tabSlide.to = double(activeIndex); tabSlide.duration = 0; tabSlideKnown = true; }
    else if (std::abs(tabSlide.to - double(activeIndex)) > .01) {
        tabSlide.move(double(activeIndex), now, canAnimate() && !hidden ? 260 : 0);
        if (tabSlide.duration) armMotionTimer();
    }
    const double at = std::clamp(tabSlide.value(now), 0.0, double(tabs.size() - 1));
    for (size_t i = 0; i < tabs.size(); ++i) {
        RECT r = tabRect(i, tabs.size(), client);
        InflateRect(&r, -sp(3), -sp(3));
        if (int(i) == tabHover && i != activeIndex) rounded(dc, r, blend(Surface, Selected, .45), 9);
    }
    {
        // The highlight between the two tabs it is passing.
        const size_t a = size_t(std::floor(at)), b = std::min(tabs.size() - 1, a + 1);
        RECT ra = tabRect(a, tabs.size(), client), rb = tabRect(b, tabs.size(), client);
        InflateRect(&ra, -sp(3), -sp(3)); InflateRect(&rb, -sp(3), -sp(3));
        const double f = at - double(a);
        const RECT pill{ra.left + int(std::lround((rb.left - ra.left) * f)), ra.top, ra.right + int(std::lround((rb.right - ra.right) * f)), ra.bottom};
        rounded(dc, pill, theme.highContrast ? Selected : theme.accentSoft, 9);
    }
    for (size_t i = 0; i < tabs.size(); ++i) {
        RECT r = tabRect(i, tabs.size(), client);
        InflateRect(&r, -sp(3), -sp(3));
        const double on = std::max(0.0, 1 - std::abs(at - double(i))), hot = int(i) == tabHover ? 1.0 : 0.0;
        const COLORREF selectedInk = theme.highContrast ? theme.selectedInk : Green;
        const COLORREF ink = blend(blend(Muted, Ink, hot), selectedInk, on);
        drawText(dc, tabLabel(tabs[i]), r, ink, on > .5 ? strongFont : normalFont, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
}
int tabAt(POINT point, RECT client) {
    const auto tabs = visibleTabs();
    for (size_t i = 0; i < tabs.size(); ++i) { const RECT r = tabRect(i, tabs.size(), client); if (PtInRect(&r, point)) return int(i); }
    return -1;
}
// "About 1 h 5 min"; empty when there is no usable estimate.
std::wstring durationText(Value hours) {
    if (!hours || *hours >= 100) return {};
    const int minutes = roundedMinutes(*hours);
    if (!minutes) return {};
    return tr(L"約 ") + (minutes >= 60 ? std::to_wstring(minutes / 60) + tr(L" 小時 ") : L"") + std::to_wstring(minutes % 60) + tr(L" 分");
}
// "11/02" from yyyymmdd.
std::wstring dayLabel(int date) { return std::to_wstring(date / 100 % 100) + L"/" + (date % 100 < 10 ? L"0" : L"") + std::to_wstring(date % 100); }
// A range when the 5-minute and longer means disagree by more than 15%.
std::wstring remainingText(const PowerMetrics& m) {
    if (m.hoursRemainingLow && m.hoursRemainingHigh && *m.hoursRemainingHigh > *m.hoursRemainingLow * 1.15 && *m.hoursRemainingHigh < 100) {
        const int low = roundedMinutes(*m.hoursRemainingLow), high = roundedMinutes(*m.hoursRemainingHigh);
        if (high < 60) return tr(L"約 ") + std::to_wstring(low) + L"–" + std::to_wstring(high) + tr(L" 分");
        return tr(L"約 ") + number(*m.hoursRemainingLow, 1) + L"–" + number(*m.hoursRemainingHigh, 1) + tr(L" 小時");
    }
    return durationText(m.hoursRemaining);
}
// Time to full (or to a detected charge limit): the learned curve when it covers
// the way, else the at-current-rate estimate. Both need 60 s of steady charging.
struct ChargeEta { Value hours; int target = 100; bool curve = false; };
ChargeEta chargeEta(const ChargingMetrics& c, const PowerSample& p) {
    ChargeEta eta;
    if (!p.charging || !p.percent || !p.fullWh || !p.remainingWh) { eta.hours = c.hoursToFull; return eta; }
    if (chargeLimit && *p.percent < *chargeLimit - .5) eta.target = *chargeLimit;
    const bool stable = c.coverageSeconds >= 60 && c.averageWatts && c.watts && *c.averageWatts > .5 && *c.watts > .5;
    if (stable) {
        if (auto curve = chargeCurve.hoursTo(*p.percent, eta.target, *p.fullWh, std::min(*c.averageWatts, *c.watts))) {
            eta.hours = curve; eta.curve = true; return eta;
        }
    }
    if (eta.target == 100) eta.hours = c.hoursToFull;
    else if (c.hoursToFull) {
        const double energy = *p.fullWh * eta.target / 100 - *p.remainingWh;
        if (energy > 0) eta.hours = energy / std::min(*c.averageWatts, *c.watts);
    }
    return eta;
}
std::wstring etaLabel(const ChargeEta& eta) {
    if (eta.target < 100) return tr(L"至 ") + std::to_wstring(eta.target) + tr(L"% 上限") + (eta.curve ? tr(L" · 依充電曲線") : tr(L" · 依目前速度"));
    return eta.curve ? tr(L"至充滿 · 依充電曲線") : tr(L"至充滿 · 依目前速度");
}
// The panel's layout by the height above the tab bar: compact panels drop the 30-minute
// chart (and minimal ones more). Painting and the list position must agree on it, or the
// list sits below an empty gap where the chart would be.
bool compactLayout(int height) { return height < px(600) - tabBarHeight(); }
bool minimalLayout(int height) { return height < px(480) - tabBarHeight(); }
int listTop() {
    RECT r{}; GetClientRect(mainWindow, &r);
    const int h = r.bottom - tabBarHeight();
    return sp(minimalLayout(h) ? 270 : compactLayout(h) ? 308 : 366);
}
// ----- Popups: the key display, notices in the panel and notifications -----
// Small layered windows painted in the panel's style (pixel or smooth, dark or light).
// Each rises and fades in, stays while the pointer is on it, then sinks and fades out.
//  - osd: a key's result (brightness, backlight, Fn lock...) at the bottom centre of the screen;
//  - notice: the answer to something done in the panel, as a card above its tabs;
//  - notification: battery and background alerts at the bottom right, by the clock, opening
//    the page they are about when clicked (Windows' own notification while a game or a
//    presentation is on screen, or in quiet hours).
enum class PopupKind { osd, notice, notification };
struct Popup {
    PopupKind kind;
    HWND window = nullptr;
    std::wstring title, body;
    int level = -1;                 // osd: a 0-100 bar, -1 none
    COLORREF color = 0;             // accent; 0 is the theme's
    BalloonTarget target = BalloonTarget::none;
    Transition shown;               // 0 hidden, 1 resting in place
    uint64_t hideAt = 0;            // GetTickCount64() when it starts to leave
    bool hover = false, closeHot = false, timer = false;
    RECT rest{};                    // screen position when shown
    explicit Popup(PopupKind k) : kind(k) {}
};
Popup osdPopup{PopupKind::osd}, noticePopup{PopupKind::notice}, notifyPopup{PopupKind::notification};
const wchar_t* PopupClassName = L"BatteryScope.Popup.v1";
constexpr UINT_PTR PopupTimer = 1;
int wrappedHeight(const std::wstring& text, HFONT face, int width) {
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, fontFor(face, text));
    RECT r{0, 0, width, 0};
    DrawTextW(dc, text.c_str(), -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old); ReleaseDC(nullptr, dc);
    return r.bottom;
}
// A notice card's height for its text at this card width: up to three lines.
int noticeHeight(const std::wstring& text, int width) { return std::max(sp(40), std::min(wrappedHeight(text, smallFont, width - sp(46)), sp(54)) + sp(20)); }
RECT popupClose(RECT client) { return {client.right - sp(30), client.top + sp(8), client.right - sp(10), client.top + sp(28)}; }
void paintPopup(HDC dc, RECT r, const Popup& p) {
    fill(dc, r, Background);
    card(dc, r, Surface, 12);
    if (!pixelStyle) outline(dc, r, blend(Border, Surface, .2), 12);
    const COLORREF accent = p.color ? p.color : Green;
    const int unit = pixelUnit();
    switch (p.kind) {
    case PopupKind::osd: {
        const bool bar = p.level >= 0;
        drawText(dc, p.body, {r.left + sp(14), r.top + sp(6), r.right - sp(14), bar ? r.top + sp(32) : r.bottom - sp(6)}, Ink, strongFont,
            DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        if (!bar) break;
        const RECT track{r.left + sp(18), r.top + sp(38), r.right - sp(18), r.top + sp(38) + unit * 3};
        const double level = std::clamp(p.level / 100.0, 0.0, 1.0);
        const COLORREF off = blend(Surface, Border, .35);
        if (pixelStyle) ui::pixelSegments(dc, track, level, accent, off, unit);
        else {
            rounded(dc, track, off, 3);
            const int filled = int((track.right - track.left) * level);
            if (filled > 0) rounded(dc, {track.left, track.top, track.left + std::max(filled, sp(4)), track.bottom}, accent, 3);
        }
        break;
    }
    case PopupKind::notice: {
        // A marker in the accent colour, then the message.
        const int mid = (r.top + r.bottom) / 2;
        rounded(dc, {r.left + sp(14), mid - sp(4), r.left + sp(22), mid + sp(4)}, accent, 4);
        const int width = r.right - r.left - sp(46);
        const int height = std::min(wrappedHeight(p.body, smallFont, width), sp(54));
        drawText(dc, p.body, {r.left + sp(32), mid - height / 2, r.right - sp(14), mid - height / 2 + height}, Ink, smallFont, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
        break;
    }
    case PopupKind::notification: {
        // An accent stripe, the app's name, the title and the message; a close button on hover.
        rounded(dc, {r.left + sp(10), r.top + sp(12), r.left + sp(14), r.bottom - sp(12)}, accent, 2);
        const int left = r.left + sp(26), right = r.right - sp(16);
        const RECT icon{left, r.top + sp(11), left + sp(18), r.top + sp(23)};
        if (pixelStyle) ui::pixelBattery(dc, icon, .7, Muted, accent, blend(Surface, Border, .5), false, 0, 0, std::max(1, unit / 2), 3);
        else ui::drawBattery(dc, icon, .7, Muted, accent, false, 0, 0);
        drawText(dc, L"BatteryScope", {left + sp(22), r.top + sp(8), right - sp(24), r.top + sp(26)}, Muted, smallFont);
        if (p.hover) {
            const RECT close = popupClose(r);
            if (p.closeHot) rounded(dc, close, blend(Surface, Selected, .7), 6);
            const int c = sp(4), cx = (close.left + close.right) / 2, cy = (close.top + close.bottom) / 2;
            for (int k = -c; k <= c; k += std::max(1, unit / 2)) {
                fill(dc, {cx + k, cy + k, cx + k + std::max(1, unit / 2), cy + k + std::max(1, unit / 2)}, Ink);
                fill(dc, {cx + k, cy - k, cx + k + std::max(1, unit / 2), cy - k + std::max(1, unit / 2)}, Ink);
            }
        }
        drawText(dc, p.title, {left, r.top + sp(30), right, r.top + sp(52)}, Ink, strongFont);
        drawText(dc, p.body, {left, r.top + sp(54), right, r.bottom - sp(12)}, Muted, smallFont, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
        break;
    }
    }
}
void placePopup(Popup& p, double now) {
    const double v = p.shown.value(now);
    const int rise = int(std::lround((1 - v) * sp(p.kind == PopupKind::notice ? 10 : 18)));
    SetLayeredWindowAttributes(p.window, 0, BYTE(std::clamp(int(std::lround(v * 248)), 0, 248)), LWA_ALPHA);
    SetWindowPos(p.window, HWND_TOPMOST, p.rest.left, p.rest.top + rise, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}
void armPopup(Popup& p, UINT ms) { SetTimer(p.window, PopupTimer, ms, nullptr); p.timer = true; }
void hidePopup(Popup& p) {
    if (!p.window || !IsWindowVisible(p.window) || p.shown.to == 0) return;
    p.shown.move(0, motionNow(), canAnimate() ? 170 : 0);
    armPopup(p, USER_TIMER_MINIMUM);
}
Popup* popupOf(HWND window) { return reinterpret_cast<Popup*>(GetWindowLongPtrW(window, GWLP_USERDATA)); }
LRESULT CALLBACK popupProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    Popup* p = popupOf(window);
    switch (message) {
    case WM_NCHITTEST: return p && p->kind == PopupKind::osd ? HTTRANSPARENT : HTCLIENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_TIMER: {
        if (!p || wp != PopupTimer) break;
        const double now = motionNow();
        placePopup(*p, now);
        if (p->shown.active(now)) return 0;
        if (p->shown.to == 0) { KillTimer(window, PopupTimer); p->timer = false; ShowWindow(window, SW_HIDE); return 0; }
        if (!p->hover && GetTickCount64() >= p->hideAt) { hidePopup(*p); return 0; }
        armPopup(*p, 200); // resting: check now and then whether it is time to leave
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!p) break;
        RECT client{}; GetClientRect(window, &client);
        const RECT close = popupClose(client); const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const bool closeHot = p->kind == PopupKind::notification && PtInRect(&close, pt);
        if (!p->hover || closeHot != p->closeHot) { p->hover = true; p->closeHot = closeHot; InvalidateRect(window, nullptr, FALSE); }
        if (p->shown.to == 0) { p->shown.move(1, motionNow(), canAnimate() ? 140 : 0); armPopup(*p, USER_TIMER_MINIMUM); } // caught on its way out
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0}; TrackMouseEvent(&tracking);
        SetCursor(LoadCursorW(nullptr, p->kind == PopupKind::notification && !closeHot ? IDC_HAND : IDC_ARROW));
        return 0;
    }
    case WM_MOUSELEAVE:
        if (p) { p->hover = p->closeHot = false; p->hideAt = std::max(p->hideAt, GetTickCount64() + 2500); InvalidateRect(window, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONUP: {
        if (!p) break;
        RECT client{}; GetClientRect(window, &client);
        const RECT close = popupClose(client); const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (p->kind == PopupKind::notification && !PtInRect(&close, pt) && p->target != BalloonTarget::none)
            PostMessageW(mainWindow, PopupClickMessage, WPARAM(p->target), 0);
        hidePopup(*p);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps{}; HDC target = BeginPaint(window, &ps); RECT r{}; GetClientRect(window, &r);
        HDC dc = CreateCompatibleDC(target); HBITMAP bitmap = CreateCompatibleBitmap(target, r.right, std::max<LONG>(1, r.bottom));
        HGDIOBJ old = SelectObject(dc, bitmap);
        if (p) paintPopup(dc, r, *p);
        BitBlt(target, 0, 0, r.right, r.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc); EndPaint(window, &ps);
        return 0;
    }
    }
    return DefWindowProcW(window, message, wp, lp);
}
// Shows (or updates, when already up) a popup at `rest` for `ms` milliseconds.
void showPopup(Popup& p, RECT rest, uint64_t ms) {
    if (!p.window) {
        const DWORD clickThrough = p.kind == PopupKind::osd ? WS_EX_TRANSPARENT : 0;
        p.window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE | clickThrough, PopupClassName, L"BatteryScope",
            WS_POPUP, rest.left, rest.top, rest.right - rest.left, rest.bottom - rest.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!p.window) return;
        SetWindowLongPtrW(p.window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&p));
        if (!pixelStyle) { DWORD corners = 2; DwmSetWindowAttribute(p.window, 33, &corners, sizeof(corners)); }
    }
    const double now = motionNow();
    const bool showing = IsWindowVisible(p.window) && p.shown.to > 0;
    p.rest = rest; p.hideAt = GetTickCount64() + ms;
    SetWindowPos(p.window, HWND_TOPMOST, rest.left, rest.top, rest.right - rest.left, rest.bottom - rest.top, SWP_NOACTIVATE);
    if (!showing) {
        p.shown = {}; p.shown.move(1, now, canAnimate() ? 230 : 0);
        placePopup(p, now);
        ShowWindow(p.window, SW_SHOWNOACTIVATE);
    }
    InvalidateRect(p.window, nullptr, FALSE);
    armPopup(p, USER_TIMER_MINIMUM);
}
void showOsd(const std::wstring& text, int level = -1, COLORREF color = 0) {
    osdPopup.body = text; osdPopup.level = level; osdPopup.color = color;
    // Bottom centre of the screen in use, where Windows shows its own volume display.
    MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY), &monitor);
    const RECT work = monitor.rcWork;
    const int width = sp(250), height = level >= 0 ? sp(60) : sp(42);
    const int x = (work.left + work.right - width) / 2, y = work.bottom - height - sp(80);
    showPopup(osdPopup, {x, y, x + width, y + height}, 1600);
}
// The answer to something done in the panel, as a card resting above its tabs. Nothing shows
// while the panel is hidden.
void showNoticeCard(const std::wstring& text) {
    if (!mainWindow || hidden || !IsWindowVisible(mainWindow) || text.empty()) return;
    RECT panel{}; GetWindowRect(mainWindow, &panel);
    const int width = panel.right - panel.left - sp(36);
    const int height = noticeHeight(text, width);
    const int x = panel.left + sp(18), y = panel.bottom - tabBarHeight() - height - sp(10);
    noticePopup.body = text; noticePopup.color = 0;
    showPopup(noticePopup, {x, y, x + width, y + height}, std::clamp<uint64_t>(2600 + text.size() * 45, 3200, 9000));
}
// A notification by the clock; Windows' own (which respects quiet hours) while the user is in
// a full-screen game, a presentation, or quiet hours.
bool showNotificationCard(const std::wstring& title, const std::wstring& body, BalloonTarget target, COLORREF color) {
    QUERY_USER_NOTIFICATION_STATE state{};
    if (FAILED(SHQueryUserNotificationState(&state)) || state != QUNS_ACCEPTS_NOTIFICATIONS) return false;
    notifyPopup.title = title; notifyPopup.body = body; notifyPopup.target = target; notifyPopup.color = color;
    MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor);
    const RECT work = monitor.rcWork;
    const int width = sp(340), bodyHeight = std::min(wrappedHeight(body, smallFont, width - sp(42)), sp(64));
    const int height = sp(66) + bodyHeight;
    const int x = work.right - width - sp(16), y = work.bottom - height - sp(16);
    showPopup(notifyPopup, {x, y, x + width, y + height}, 9000);
    return true;
}
// The three kinds of popup in one picture, for screenshots: --demo --popups-bmp <path>.
bool capturePopups(const std::wstring& path) {
    Popup osd(PopupKind::osd), note(PopupKind::notice), alert(PopupKind::notification);
    osd.body = tr(L"螢幕亮度 ") + std::wstring(L"60%"); osd.level = 60; osd.color = theme.accent;
    note.body = tr(L"用電池時自動切到 60 Hz，插電回到最高更新率。");
    alert.title = tr(L"背景程式持續耗電"); alert.color = theme.warn; alert.hover = true;
    alert.body = L"OneDrive.exe" + std::wstring(tr(L" 已在背景持續使用約 ")) + L"1.2 W" + tr(L" CPU 電力（") + L"12" + tr(L" 分鐘）。點此查看。");
    const int width = sp(340), gap = sp(12);
    const RECT r1{0, 0, sp(250), sp(60)}, r2{0, 0, width, noticeHeight(note.body, width)}, r3{0, 0, width, sp(66) + std::min(wrappedHeight(alert.body, smallFont, width - sp(42)), sp(64))};
    const int height = gap * 4 + r1.bottom + r2.bottom + r3.bottom;
    HDC screen = GetDC(nullptr); HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(info.bmiHeader); info.bmiHeader.biWidth = width + gap * 2; info.bmiHeader.biHeight = height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    void* pixels = nullptr; HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HGDIOBJ old = SelectObject(dc, bitmap);
    fill(dc, {0, 0, width + gap * 2, height}, blend(Background, Ink, .08));
    int y = gap;
    for (auto [popup, rect] : {std::pair<Popup*, RECT>{&osd, r1}, {&note, r2}, {&alert, r3}}) {
        const RECT at{gap, y, gap + rect.right, y + rect.bottom};
        HRGN clip = CreateRectRgnIndirect(&at); SelectClipRgn(dc, clip);
        SetViewportOrgEx(dc, at.left, at.top, nullptr);
        paintPopup(dc, rect, *popup);
        SetViewportOrgEx(dc, 0, 0, nullptr); SelectClipRgn(dc, nullptr); DeleteObject(clip);
        y += rect.bottom + gap;
    }
    GdiFlush();
    BITMAPFILEHEADER file{}; file.bfType = 0x4d42; file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
    file.bfSize = file.bfOffBits + DWORD(info.bmiHeader.biWidth * height * 4);
    std::ofstream output(std::filesystem::path(path), std::ios::binary);
    output.write(reinterpret_cast<char*>(&file), sizeof(file)); output.write(reinterpret_cast<char*>(&info.bmiHeader), sizeof(info.bmiHeader));
    output.write(static_cast<char*>(pixels), info.bmiHeader.biWidth * height * 4);
    const bool ok = output.good();
    SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc); ReleaseDC(nullptr, screen);
    return ok;
}
void closePopups() {
    for (Popup* p : {&osdPopup, &noticePopup, &notifyPopup}) if (p->window) { DestroyWindow(p->window); p->window = nullptr; }
}
void setNotice(std::wstring text) {
    notice = std::move(text); noticeUntil = GetTickCount64() + 15000;
    SetTimer(mainWindow, NoticeTimer, 15000, nullptr);
    showNoticeCard(notice);
    InvalidateRect(mainWindow, nullptr, FALSE);
}
// Pixel font size snapped to whole multiples of its 12 px design grid when the
// DPI-scaled size is within 25%; otherwise the scaled size (uneven pixels).
int pixelEm(int multiple) {
    const int target = px(12 * multiple), snapped = std::max(1, (target + 6) / 12) * 12;
    return std::abs(snapped - target) * 4 <= target ? snapped : target;
}
void makeFonts() {
    for (auto f : {normalFont, smallFont, strongFont, titleFont, valueFont, metricFont}) if (f) DeleteObject(f);
    for (auto f : fallbackOwned) DeleteObject(f);
    fallbackOwned.clear(); fallbackFonts.clear();
    auto make = [](int height, int weight, const wchar_t* face, DWORD quality) {
        return CreateFontW(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, quality, DEFAULT_PITCH, face);
    };
    const wchar_t* systemFace = english ? L"Segoe UI" : L"Microsoft JhengHei UI";
    auto system = [&](int height, int weight) { return make(px(height), weight, systemFace, CLEARTYPE_QUALITY); };
    if (pixelStyle && ui::pixelFontLoaded()) {
        auto pixel = [&](int multiple, int weight, int fallbackHeight, int fallbackWeight) {
            HFONT face = make(pixelEm(multiple), weight, ui::PixelFontFace, NONANTIALIASED_QUALITY);
            HFONT fallback = system(fallbackHeight, fallbackWeight);
            fallbackOwned.push_back(fallback); fallbackFonts[face] = fallback;
            return face;
        };
        normalFont = pixel(1, FW_NORMAL, 12, FW_NORMAL); smallFont = pixel(1, FW_NORMAL, 11, FW_NORMAL);
        strongFont = pixel(1, FW_BOLD, 12, FW_SEMIBOLD); titleFont = pixel(2, FW_NORMAL, 16, FW_SEMIBOLD);
        valueFont = pixel(2, FW_NORMAL, 29, FW_SEMIBOLD); metricFont = pixel(1, FW_BOLD, 16, FW_SEMIBOLD);
        systemFont = system(12, FW_NORMAL); fallbackOwned.push_back(systemFont);
    } else {
        normalFont = system(12, FW_NORMAL); smallFont = system(11, FW_NORMAL); strongFont = system(12, FW_SEMIBOLD);
        titleFont = system(16, FW_SEMIBOLD); valueFont = system(29, FW_SEMIBOLD); metricFont = system(16, FW_SEMIBOLD);
        systemFont = normalFont;
    }
    for (auto window : {list, closeButton, killButton, serviceButton, detailsButton, backgroundCheck, allCheck,
        hideButton, menuButton, powerButton, inlineBack, inlineConfirm, restoreModesButton, energySettingsButton, ecoButton, perfAppButton,
        modeButtons[0], modeButtons[1], modeButtons[2], modeButtons[3], modeButtons[4], modeButtons[5]})
        if (window) SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(normalFont), TRUE);
    for (auto window : {inlineText, tooltip})
        if (window) SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(systemFont), TRUE);
}
void applyTheme() {
    theme = ui::palette(themeChoice, previewContrast);
    Ink = theme.ink; Muted = theme.muted; Green = theme.accent; Background = theme.background;
    Surface = theme.surface; Border = theme.border; Selected = theme.selected; Danger = theme.danger;
    if (!mainWindow) return;
    const wchar_t* controls = theme.highContrast ? nullptr : theme.dark ? L"DarkMode_Explorer" : L"Explorer";
    for (HWND control : {viewport, list, inlineText, settingsList}) if (control) SetWindowTheme(control, controls, nullptr);
    if (list) { ListView_SetBkColor(list, Background); ListView_SetTextBkColor(list, Background); ListView_SetTextColor(list, Ink); }
    if (tooltip) { SendMessageW(tooltip, TTM_SETTIPBKCOLOR, Surface, 0); SendMessageW(tooltip, TTM_SETTIPTEXTCOLOR, Ink, 0); }
    BOOL dark = theme.dark; DwmSetWindowAttribute(mainWindow, 20, &dark, sizeof(dark));
    COLORREF edge = theme.highContrast ? Ink : Border; DwmSetWindowAttribute(mainWindow, 34, &edge, sizeof(edge));
    RedrawWindow(mainWindow, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
    if (miniWindow) InvalidateRect(miniWindow, nullptr, FALSE);
}
bool refreshIcons() {
    HINSTANCE module = GetModuleHandleW(nullptr);
    HICON loadedLargeIcon = static_cast<HICON>(LoadImageW(module, MAKEINTRESOURCEW(101), IMAGE_ICON,
        GetSystemMetricsForDpi(SM_CXICON, dpi), GetSystemMetricsForDpi(SM_CYICON, dpi), LR_DEFAULTCOLOR));
    HICON loadedSmallIcon = static_cast<HICON>(LoadImageW(module, MAKEINTRESOURCEW(101), IMAGE_ICON,
        GetSystemMetricsForDpi(SM_CXSMICON, dpi), GetSystemMetricsForDpi(SM_CYSMICON, dpi), LR_DEFAULTCOLOR));
    if (!loadedLargeIcon || !loadedSmallIcon) { if (loadedLargeIcon) DestroyIcon(loadedLargeIcon); if (loadedSmallIcon) DestroyIcon(loadedSmallIcon); return false; }
    SendMessageW(mainWindow, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(loadedLargeIcon));
    SendMessageW(mainWindow, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(loadedSmallIcon));
    HICON oldLarge = appIcon, oldSmall = smallIcon;
    appIcon = loadedLargeIcon; smallIcon = loadedSmallIcon;
    // Replace the shell's copy before releasing the old handles.
    if (trayAdded) updateTray(false);
    if (oldLarge) DestroyIcon(oldLarge);
    if (oldSmall) DestroyIcon(oldSmall);
    if (headerIcon) DestroyIcon(headerIcon);
    headerIcon = static_cast<HICON>(LoadImageW(module, MAKEINTRESOURCEW(101), IMAGE_ICON, sp(22), sp(22), LR_DEFAULTCOLOR));
    processIcons.clear();
    return true;
}
ui::Mood currentMood() {
    if (current.history.empty() || !current.history.back().percent) return ui::Mood::calm;
    const auto& p = current.history.back();
    const bool plugged = p.acKnown && (p.onAc || p.charging);
    if (!plugged && *p.percent <= 10) return ui::Mood::critical;
    if (!plugged && *p.percent <= 20) return ui::Mood::tired;
    if (p.charging) return ui::Mood::charging;
    if (plugged) return *p.percent >= 99.5 ? ui::Mood::full : ui::Mood::calm;
    const auto& m = current.power;
    if (m.averageWatts && (*m.averageWatts >= 25 || (m.longAverageWatts && *m.averageWatts > *m.longAverageWatts * 1.8))) return ui::Mood::busy;
    if (m.averageWatts && *m.averageWatts < 6) return ui::Mood::sleepy;
    return ui::Mood::calm;
}
ui::MascotColors mascotColors(ui::Mood mood) {
    const COLORREF level = mood == ui::Mood::critical ? Danger : mood == ui::Mood::tired ? theme.warn : Green;
    return {blend(Muted, Surface, .1), blend(Surface, Border, .6), level, theme.highContrast ? theme.selectedInk : RGB(14, 19, 26),
        Ink, blend(Danger, Surface, .2), RGB(120, 180, 255), theme.warn};
}
std::wstring mascotLine() {
    const auto mood = currentMood();
    if (current.history.empty()) return tr(L"我正在讀取電池資料…");
    const auto& p = current.history.back();
    const std::wstring percent = valueText(p.percent, L"%", 0), left = remainingText(current.power);
    switch (mood) {
    case ui::Mood::critical: return tr(L"只剩 ") + percent + tr(L"，快幫我接上電源！");
    case ui::Mood::tired: return tr(L"有點累了…剩 ") + percent + (left.empty() ? L"" : tr(L"，") + left);
    case ui::Mood::charging: {
        const auto eta = chargeEta(calculateCharging(current.history), p);
        const auto time = durationText(eta.hours);
        return tr(L"充電中～") + (time.empty() ? std::wstring(tr(L"正在學習充電速度")) : time + (eta.target < 100 ? tr(L"到上限") : tr(L"充滿")));
    }
    case ui::Mood::full: return tr(L"已充滿！可以拔掉電源了。");
    case ui::Mood::busy: {
        std::wstring top;
        for (const auto& row : current.processes) if (row.averageCpuWatts && row.pid > 4) { top = row.name; break; }
        return tr(L"好忙！現在 ") + valueText(current.power.averageWatts, L" W") + (top.empty() ? L"" : tr(L"，最耗電的是 ") + top);
    }
    case ui::Mood::sleepy: return tr(L"好安靜… 只用 ") + valueText(current.power.averageWatts, L" W") + (left.empty() ? L"" : tr(L"，") + left);
    default:
        if (p.acKnown && p.onAc) return tr(L"插著電，一切正常。");
        return tr(L"一切正常：") + valueText(current.power.averageWatts, L" W") + (left.empty() ? L"" : tr(L"，") + left);
    }
}
std::wstring trayText() {
    std::wstring text = L"BatteryScope";
    if (current.history.empty()) return text;
    const auto& p = current.history.back();
    text += L" · " + valueText(p.percent, L"%", 0);
    if (p.charging) {
        const auto charge = calculateCharging(current.history);
        text += tr(L" · 充電 ") + valueText(charge.watts, L" W");
        const auto eta = chargeEta(charge, p);
        if (auto full = durationText(eta.hours); !full.empty()) {
            const std::wstring goal = eta.target < 100 ? std::to_wstring(eta.target) + L"%" : L"";
            text += english ? L" · " + full + L" to " + (goal.empty() ? L"full" : goal) : L" · " + full + (goal.empty() ? L"充滿" : L"到 " + goal);
        }
    } else if (p.onAc) text += tr(L" · 插電中");
    else {
        text += L" · " + valueText(current.power.currentWatts, L" W");
        if (auto left = remainingText(current.power); !left.empty()) text += english ? L" · " + left + L" left" : L" · 續航" + left;
    }
    return text;
}
// The percentage icon for the latest sample, regenerated only when its
// number, color state, or size changes. Falls back to the app icon.
HICON currentTrayIcon(HICON* retired) {
    if (!trayPercent || current.history.empty() || !current.history.back().present || !current.history.back().percent) return smallIcon;
    const auto& p = current.history.back();
    const int percent = int(std::lround(std::clamp(*p.percent, 0.0, 100.0)));
    const bool plugged = p.acKnown && (p.onAc || p.charging);
    const int state = plugged ? 1 : percent <= 10 ? 3 : percent <= 20 ? 2 : 0;
    const int size = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
    const std::wstring key = std::to_wstring(size) + L":" + std::to_wstring(percent) + L":" + std::to_wstring(state);
    if (trayIcon && key == trayIconKey) return trayIcon;
    // Fixed colors: the badge must read on both light and dark taskbars.
    static const COLORREF badges[] = {RGB(36, 46, 58), RGB(72, 196, 156), RGB(242, 180, 64), RGB(226, 82, 82)};
    static const COLORREF edges[] = {RGB(120, 134, 148), RGB(28, 110, 86), RGB(150, 100, 20), RGB(140, 36, 36)};
    static const COLORREF digits[] = {RGB(255, 255, 255), RGB(6, 32, 24), RGB(32, 22, 4), RGB(255, 255, 255)};
    HICON next = ui::makePercentIcon(size, percent, badges[state], edges[state], digits[state]);
    if (!next) return smallIcon;
    if (retired) *retired = trayIcon; else if (trayIcon) DestroyIcon(trayIcon);
    trayIcon = next; trayIconKey = key;
    return trayIcon;
}
bool updateTray(bool add) {
    if (isolatedPreview) return false;
    NOTIFYICONDATAW data{}; data.cbSize = sizeof(data); data.hWnd = mainWindow; data.uID = 1;
    HICON retired = nullptr;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP; data.uCallbackMessage = TrayMessage; data.hIcon = currentTrayIcon(&retired);
    wcsncpy_s(data.szTip, trayText().c_str(), _TRUNCATE);
    const bool shown = Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &data) != FALSE;
    if (retired) DestroyIcon(retired); // the shell keeps its own copy of the new icon
    if (!shown) return false;
    if (add) { data.uVersion = NOTIFYICON_VERSION_4; trayVersion4 = Shell_NotifyIconW(NIM_SETVERSION, &data) != FALSE; }
    return true;
}
void showBalloon(const std::wstring& title, const std::wstring& body, BalloonTarget target) {
    if (!demo && !isolatedPreview && showNotificationCard(title, body, target, target == BalloonTarget::update ? Green : theme.warn)) return;
    NOTIFYICONDATAW data{}; data.cbSize = sizeof(data); data.hWnd = mainWindow; data.uID = 1;
    data.uFlags = NIF_INFO; data.dwInfoFlags = NIIF_WARNING | NIIF_RESPECT_QUIET_TIME;
    wcsncpy_s(data.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(data.szInfo, body.c_str(), _TRUNCATE);
    if (Shell_NotifyIconW(NIM_MODIFY, &data)) balloonTarget = target;
}
// One Windows notification when the battery reaches the chosen threshold.
void showLowBatteryNotice() {
    if (!trayAdded || current.history.empty()) return;
    const auto& p = current.history.back();
    std::wstring body = tr(L"目前耗電 ") + valueText(current.power.currentWatts, L" W");
    if (auto left = remainingText(current.power); !left.empty()) body += english ? L" · " + left + L" left" : L" · 續航" + left;
    for (const auto& row : current.processes) {
        if (!row.averageCpuWatts || *row.averageCpuWatts < .3 || row.pid <= 4) continue;
        body += tr(L"\n耗電最高：") + (row.services.size() == 1 ? row.services.front().display : row.name) +
            L" (" + valueText(row.averageCpuWatts, L" W", 1) + L")";
        break;
    }
    showBalloon(tr(L"電量偏低 · ") + valueText(p.percent, L"%", 0), body, BalloonTarget::none);
}
// One drain notification per process, at most every 10 minutes, on battery only.
void checkDrainAlert() {
    if (!drainAlerts || demo || isolatedPreview || !trayAdded || GetTickCount64() - lastDrainAlert < 600000) return;
    const auto index = drainAlertCandidate(current, drainNotified);
    if (!index) return;
    const auto& row = current.processes[*index];
    drainNotified.insert({row.pid, row.created});
    lastDrainAlert = GetTickCount64();
    showBalloon(tr(L"背景程式持續耗電"), row.name + tr(L" 已在背景持續使用約 ") + valueText(row.averageCpuWatts, L" W", 1) +
        tr(L" CPU 電力（") + std::to_wstring(int(row.busySeconds / 60)) + tr(L" 分鐘）。點此查看。"), BalloonTarget::candidates);
}
std::wstring sleepText(const SleepRecord& sleep) {
    return durationText(sleep.hours()).substr(std::wstring(tr(L"約 ")).size()) + tr(L"，電量 ") + number(sleep.before, 0) + L"% → " +
        number(sleep.after, 0) + L"%" + tr(L"（每小時 ") + number(std::max(0.0, sleep.ratePerHour()), 1) + tr(L"%）");
}
// After each detected sleep (a sampling gap of 10+ minutes on battery), warn
// when the drain was unusual; and remind once when charging reaches the target.
void checkBatteryNotices() {
    if (demo || isolatedPreview || !trayAdded || current.history.empty()) return;
    if (current.sleepCount != seenSleeps) {
        seenSleeps = current.sleepCount;
        if (sleepAlerts && current.lastSleep && unusualSleepDrain(*current.lastSleep))
            showBalloon(tr(L"睡眠期間耗電偏高"), tr(L"睡眠 ") + sleepText(*current.lastSleep) +
                tr(L"。可能有程式或裝置讓電腦無法進入深度睡眠。"), BalloonTarget::powerDetails);
    }
    const bool firmwareLimit = chargeLimit && *chargeLimit <= int(chargeReminderTarget) + 1;
    if (chargeReminderTarget && !firmwareLimit && chargeReminder.update(current.history.back(), chargeReminderTarget))
        showBalloon(tr(L"電量已達 ") + std::to_wstring(chargeReminderTarget) + L"%",
            tr(L"可以拔掉充電器了；長時間維持高電量會加速電池老化。"), BalloonTarget::none);
}
void refreshList();
void positionPanel() {
    NOTIFYICONIDENTIFIER id{}; id.cbSize = sizeof(id); id.hWnd = mainWindow; id.uID = 1;
    RECT anchor{}; POINT cursor{}; GetCursorPos(&cursor);
    bool located = trayAdded && SUCCEEDED(Shell_NotifyIconGetRect(&id, &anchor));
    HMONITOR screen = located ? MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST) : MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor); GetMonitorInfoW(screen, &monitor);
    // A move across monitors can synchronously change DPI. Recompute once afterwards.
    for (int pass = 0; pass < 2; ++pass) {
        const auto& work = monitor.rcWork;
        int pad = px(14), width = std::min(px(PanelWidth), int(work.right - work.left) - 2 * pad);
        int height = std::min(px(PanelHeight), int(work.bottom - work.top) - 2 * pad);
        panelOffset = {0, px(12)};
        int x = work.right - pad - width, y = work.bottom - pad - height;
        if (located) {
            x = (anchor.left + anchor.right - width) / 2;
            if (anchor.bottom <= work.top) { y = work.top + pad; panelOffset = {0, -px(12)}; }
            else if (anchor.right <= work.left) { x = work.left + pad; y = anchor.bottom - height; panelOffset = {-px(12), 0}; }
            else if (anchor.left >= work.right) { x = work.right - pad - width; y = anchor.bottom - height; panelOffset = {px(12), 0}; }
        }
        x = std::clamp(x, int(work.left) + pad, int(work.right) - pad - width);
        y = std::clamp(y, int(work.top) + pad, int(work.bottom) - pad - height);
        SetWindowPos(mainWindow, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
    }
    GetWindowRect(mainWindow, &panelRest);
    appliedPanelProgress = -1;
}
void stepMotion(double now) {
    if (recordMotion && motionTicks.size() < 128) motionTicks.push_back(now);
    bool active = panelMotion.active(now);
    if (panelHeightMotion.duration || (panelHeightMotion.to > 0 && !panelHeightMotion.active(now))) {
        RECT rect{}; GetWindowRect(mainWindow, &rect);
        int height = std::max(px(280), int(std::lround(panelHeightMotion.value(now))));
        if (height != rect.bottom - rect.top)
            SetWindowPos(mainWindow, nullptr, rect.left, rect.bottom - height, rect.right - rect.left, height,
                SWP_NOZORDER | SWP_NOACTIVATE);
        if (height != rect.bottom - rect.top) GetWindowRect(mainWindow, &panelRest);
        active |= panelHeightMotion.active(now);
        if (!panelHeightMotion.active(now)) {
            panelHeightMotion.duration = 0;
            if (inlineClosing) finishInlineClose();
        }
    }
    if (scrollMotion.duration) {
        applyScroll(scrollMotion.value(now));
        bool moving = scrollMotion.active(now) && !hidden;
        active |= moving;
        if (!moving) {
            scrollMotion.duration = 0;
            if (listRefreshPending && !deferListRefresh()) refreshList();
        }
    }
    double progress = panelMotion.value(now);
    if (IsWindowVisible(mainWindow) && (progress != appliedPanelProgress || (!active && !panelTargetShown))) {
        SetLayeredWindowAttributes(mainWindow, 0, BYTE(std::clamp(int(std::lround(progress * 255)), 0, 255)), LWA_ALPHA);
        SetWindowPos(mainWindow, nullptr, panelRest.left + int(panelOffset.x * (1 - progress)),
            panelRest.top + int(panelOffset.y * (1 - progress)), 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        if (!active && !panelTargetShown) ShowWindow(mainWindow, SW_HIDE);
        appliedPanelProgress = progress;
    }
    bool rowsActive = false, rowsDirty = false;
    for (auto& row : rowMotions) for (auto* motion : {&row.bar, &row.hover, &row.selected}) {
        if (motion->duration) {
            rowsDirty = true;
            rowsActive |= motion->active(now);
            if (!motion->active(now)) motion->duration = 0;
        }
    }
    if (!hidden && rowsDirty) InvalidateRect(list, nullptr, FALSE);
    active |= !hidden && rowsActive;
    for (auto& item : buttonMotions) {
        if (item.second.duration) {
            InvalidateRect(item.first, nullptr, FALSE);
            active |= !hidden && item.second.active(now);
            if (!item.second.active(now)) item.second.duration = 0;
        }
    }
    for (auto& item : pressMotions) if (item.second.duration) {
        InvalidateRect(item.first, nullptr, FALSE);
        active |= !hidden && item.second.active(now);
        if (!item.second.active(now)) item.second.duration = 0;
    }
    if (tabSlide.duration) {
        // The tab highlight on its way to the selected tab.
        RECT client{}; GetClientRect(mainWindow, &client);
        RECT tabBar{0, client.bottom - tabBarHeight(), client.right, client.bottom};
        if (!hidden) InvalidateRect(mainWindow, &tabBar, FALSE);
        active |= !hidden && tabSlide.active(now);
        if (!tabSlide.active(now)) tabSlide.duration = 0;
    }
    if (!active && motionTimerRunning) {
        KillTimer(mainWindow, MotionTimer); motionTimerRunning = false;
        if (!hidden) InvalidateRect(list, nullptr, FALSE);
    }
}
void showMain() {
    if (panelTargetShown && IsWindowVisible(mainWindow)) { SetForegroundWindow(mainWindow); return; }
    bool wasVisible = IsWindowVisible(mainWindow) != FALSE;
    hidden = false; panelTargetShown = true;
    refreshPowerButton();
    if (!wasVisible) { positionPanel(); panelMotion = {}; refreshList(); }
    double now = motionNow(); panelMotion.move(1, now, canAnimate() ? 220 : 0);
    SetLayeredWindowAttributes(mainWindow, 0, BYTE(panelMotion.value(now) * 255), LWA_ALPHA);
    ShowWindow(mainWindow, SW_SHOWNORMAL); stepMotion(now);
    SetForegroundWindow(mainWindow); wake.notify_one();
    if (panelMotion.active(now)) armMotionTimer();
    // Home is the performance dashboard: the first time the panel opens it starts there.
    static bool homeShown = false;
    if (!homeShown && !demo && !isolatedPreview) { homeShown = true; if (inlinePage == InlinePage::none) showPerformancePage(); }
}
void hideMain() {
    if (!panelTargetShown && hidden) return;
    if (!trayAdded && !isolatedPreview) trayAdded = updateTray(true);
    if (!trayAdded && !isolatedPreview) { setNotice(tr(L"系統匣無法使用，面板會保持開啟。")); return; }
    hidePopup(noticePopup); // a notice belongs to the panel
    closeInlinePage(true);
    stopScroll();
    hoveredRow = -1; pointerDown = false;
    for (auto& row : rowMotions) { row.hover = {}; row.selected.duration = row.bar.duration = 0; }
    for (auto& item : buttonMotions) item.second = {};
    for (auto& item : pressMotions) item.second = {};
    panelTargetShown = false; hidden = true; wake.notify_one();
    double now = motionNow(); panelMotion.move(0, now, canAnimate() ? 150 : 0);
    stepMotion(now); if (panelMotion.active(now)) armMotionTimer();
}
void toggleMain() {
    if (!IsWindowEnabled(mainWindow)) { SetForegroundWindow(GetLastActivePopup(mainWindow)); return; }
    if (panelTargetShown) hideMain(); else showMain();
}
std::optional<ProcessRow> selection() {
    int selected = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (selected < 0 || size_t(selected) >= displayed.size()) return {};
    return displayed[size_t(selected)];
}
void updateButtons() {
    auto row = selection(); bool ready = row.has_value() && !pendingAction && !demo;
    EnableWindow(closeButton, ready && row->actionable && row->window);
    EnableWindow(killButton, ready && row->actionable);
    EnableWindow(serviceButton, ready && std::any_of(row->services.begin(), row->services.end(), [](const ServiceInfo& s) { return s.stoppable; }));
    EnableWindow(detailsButton, row.has_value());
    InvalidateRect(mainWindow, nullptr, FALSE);
}
bool isModeButton(UINT id) { return id >= ModeAcEfficiencyId && id <= ModeDcPerformanceId; }
bool modeSelected(UINT id) {
    if (!isModeButton(id)) return false;
    const bool pluggedIn = id < ModeDcEfficiencyId;
    if (pluggedIn ? !powerModes.acKnown : !powerModes.dcKnown) return false;
    return (pluggedIn ? powerModes.ac : powerModes.dc) == PowerMode((id - ModeAcEfficiencyId) % 3);
}
const wchar_t* modeName(PowerMode mode) {
    return mode == PowerMode::efficiency ? tr(L"效率") : mode == PowerMode::balanced ? tr(L"平衡") :
        mode == PowerMode::performance ? tr(L"效能") : tr(L"其他");
}
const wchar_t* effectiveName(EffectiveMode mode) {
    switch (mode) {
    case EffectiveMode::energySaver: return tr(L"節能中");
    case EffectiveMode::efficiency: return tr(L"效率");
    case EffectiveMode::balanced: return tr(L"平衡");
    case EffectiveMode::performance:
    case EffectiveMode::maxPerformance: return tr(L"效能");
    case EffectiveMode::game: return tr(L"遊戲模式");
    case EffectiveMode::mixedReality: return tr(L"混合實境");
    default: return tr(L"電源模式");
    }
}
// Body fill for the mascot: the battery level, or full on a PC without a battery.
double mascotLevel() {
    if (current.history.empty()) return 0;
    const auto& p = current.history.back();
    return !p.present ? 1.0 : p.percent ? *p.percent / 100 : 0;
}
// CPU power from the energy counters over the last 30 minutes, charted on desktops.
std::deque<std::pair<uint64_t, Value>> cpuPowerHistory;
void recordCpuPower(uint64_t ms, const CpuEnergyReport& energy) {
    if (!cpuPowerHistory.empty() && ms <= cpuPowerHistory.back().first) return;
    const bool live = energy.ready && !energy.timingMismatch;
    cpuPowerHistory.emplace_back(ms, !live ? Value{} : energy.packageWatts ? energy.packageWatts : energy.coreWatts);
    while (!cpuPowerHistory.empty() && ms - cpuPowerHistory.front().first > 1800000) cpuPowerHistory.pop_front();
}
bool batteryAbsent() { return !current.history.empty() && !current.history.back().present; }
bool pluggedInNow() { return !current.history.empty() && current.history.back().acKnown && current.history.back().onAc; }
// The header pill shows the mode Windows applies right now and opens the power mode page.
void refreshPowerButton() {
    if (!powerButton) return;
    // Synthetic preview: Windows applies the mode chosen for the current power source.
    if (demo || isolatedPreview) effectiveMode = pluggedInNow() ? EffectiveMode::performance : EffectiveMode::efficiency;
    powerSaving = effectiveMode == EffectiveMode::energySaver || effectiveMode == EffectiveMode::efficiency;
    const std::wstring dynamic = dynamicPill();
    SetWindowTextW(powerButton, dynamic.empty() ? effectiveName(effectiveMode) : dynamic.c_str());
    EnableWindow(powerButton, TRUE);
    InvalidateRect(powerButton, nullptr, FALSE);
}
void updateRowFeedback() {
    if (refreshingList) return;
    const double now = motionNow();
    const int selected = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    for (size_t i = 0; i < rowMotions.size(); ++i) {
        auto& row = rowMotions[i];
        const std::pair<Transition*, bool> feedback[] = {{&row.hover, int(i) == hoveredRow}, {&row.selected, int(i) == selected}};
        for (auto pair : feedback) {
            if (pair.first->to != double(pair.second)) {
                pair.first->move(double(pair.second), now, canAnimate() && !hidden ? 140 : 0);
                if (pair.first->duration) armMotionTimer();
            }
        }
    }
    InvalidateRect(list, nullptr, FALSE);
}
std::wstring selectionHint() {
    if (pendingAction) return tr(L"操作處理中，請稍候…");
    auto row = selection();
    if (!row) return tr(L"選取程序查看詳情或關閉 · Enter 查看");
    if (demo) return tr(L"示範模式 · 可查看詳情，關閉已停用");
    if (!row->services.empty()) return std::any_of(row->services.begin(), row->services.end(), [](const ServiceInfo& s) { return s.stoppable; })
        ? tr(L"服務宿主 · 僅停止選定服務") : tr(L"此服務目前不接受停止要求。");
    if (!row->actionable) return tr(L"受保護或權限受限 · 可查看詳情");
    if (!row->window) return tr(L"無可關閉視窗 · 可選擇強制結束");
    return tr(L"建議先正常關閉，保留儲存機會");
}
bool deferListRefresh() {
    return pointerDown || scrollingThumb || scrollMotion.active(motionNow()) ||
        inlinePage != InlinePage::none || !IsWindowEnabled(mainWindow);
}
void releaseInteraction() {
    pointerDown = false;
    if (listRefreshPending) PostMessageW(mainWindow, RefreshMessage, 0, 0);
}
void fitListColumn() {
    RECT r{}; GetClientRect(viewport, &r);
    const int width = std::max(1, int(r.right) - px(2));
    if (ListView_GetColumnWidth(list, 0) != width) ListView_SetColumnWidth(list, 0, width);
}
double scrollMaximum() {
    RECT r{}; GetClientRect(viewport, &r);
    return std::max(0, int(displayed.size()) * px(RowHeight) - int(r.bottom));
}
void applyScroll(double position) {
    if (!list || !viewport || applyingScroll) return;
    applyingScroll = true;
    scrollPosition = std::clamp(position, 0.0, scrollMaximum());
    RECT r{}; GetClientRect(viewport, &r);
    SCROLLINFO info{}; info.cbSize = sizeof(info); info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    info.nMax = std::max(0, int(displayed.size()) * px(RowHeight) - 1);
    info.nPage = UINT(r.bottom); info.nPos = int(std::lround(scrollPosition));
    SetScrollInfo(viewport, SB_VERT, &info, TRUE);
    GetClientRect(viewport, &r);
    int offset = int(std::lround(scrollPosition)), row = offset / px(RowHeight), remainder = offset % px(RowHeight);
    // Native report lists only scroll whole rows. Move a clipped viewport by the
    // fractional remainder; native selection and hit testing move with the HWND.
    SetWindowPos(list, nullptr, 0, -remainder, r.right + GetSystemMetricsForDpi(SM_CXVSCROLL, dpi),
        r.bottom + remainder, SWP_NOZORDER | SWP_NOACTIVATE);
    fitListColumn();
    int top = ListView_GetTopIndex(list);
    if (top != row) ListView_Scroll(list, 0, (row - top) * px(RowHeight));
    applyingScroll = false;
}
void stopScroll() {
    scrollMotion.from = scrollMotion.to = scrollPosition; scrollMotion.duration = 0;
    scrollingThumb = false;
}
void scrollTo(double target, bool immediate = false) {
    target = std::clamp(target, 0.0, scrollMaximum());
    if (!immediate && scrollMotion.active(motionNow()) && std::abs(target - scrollMotion.to) < .01) return;
    const double duration = !immediate && canAnimate() && !hidden && std::abs(target - scrollPosition) > .5 ? 180 : 0;
    scrollMotion.move(target, motionNow(), duration);
    scrollMotion.from = scrollPosition;
    hoveredRow = -1; updateRowFeedback();
    if (scrollMotion.duration) armMotionTimer(); else applyScroll(target);
}
void wheelScroll(WPARAM wp) {
    UINT lines = 3; SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    RECT r{}; GetClientRect(viewport, &r);
    double distance = lines == WHEEL_PAGESCROLL ? r.bottom : double(lines) * px(RowHeight);
    double delta = -double(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA * distance;
    double pending = scrollMotion.to - scrollPosition;
    double base = scrollMotion.active(motionNow()) && delta * pending > 0 ? scrollMotion.to : scrollPosition;
    scrollTo(base + delta);
}
void refreshList() {
    listRefreshPending = false;
    refreshingList = true;
    auto selected = selection();
    const int top = int(scrollPosition) / px(RowHeight);
    double preserved = scrollPosition;
    std::optional<ProcessRow> anchor = top > 0 && size_t(top) < displayed.size() ? std::optional<ProcessRow>(displayed[top]) : std::nullopt;
    std::vector<ProcessRow> next;
    const bool background = backgroundOnly;
    const bool all = showAll;
    for (const auto& row : current.processes) {
        if (background && row.window && row.services.empty()) continue;
        next.push_back(row); if (!all && next.size() == 10) break;
    }
    double now = motionNow();
    std::map<std::pair<uint32_t, uint64_t>, RowMotion> previous;
    for (const auto& motion : rowMotions) previous[{motion.pid, motion.created}] = motion;
    rowMotions.clear();
    double maximum = next.empty() ? 0 : next.front().averageCpuWatts.value_or(0);
    for (const auto& row : next) {
        auto motion = previous[{row.pid, row.created}];
        Transition& bar = motion.bar;
        double target = maximum > 0 && row.averageCpuWatts ? std::clamp(*row.averageCpuWatts / maximum, 0.0, 1.0) : 0;
        bool changed = std::abs(bar.to - target) > .001;
        bar.move(target, now, changed && canAnimate() && !hidden ? 260 : 0);
        if (bar.active(now)) armMotionTimer();
        motion.pid = row.pid; motion.created = row.created;
        rowMotions.push_back(motion);
    }
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    const size_t oldCount = displayedLabels.size();
    for (size_t i = oldCount; i > next.size(); --i) { ListView_DeleteItem(list, int(i - 1)); ++listStructuralChanges; }
    ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    displayed = std::move(next);
    displayedLabels.resize(displayed.size());
    for (size_t i = 0; i < displayed.size(); ++i) {
        const auto& row = displayed[i];
        std::wstring displayName = row.services.size() == 1 ? row.services.front().display : tr(row.name.c_str());
        if (row.services.size() > 1) displayName += tr(L"（") + std::to_wstring(row.services.size()) + tr(L" 項服務）");
        std::wstring accessible = displayName + L", PID " + std::to_wstring(row.pid) + L", " + tr(row.kind.c_str()) +
            tr(L", 估計 CPU ") + valueText(row.averageCpuWatts, L" W", 2);
        LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = int(i); item.pszText = accessible.data();
        if (i >= oldCount) { ListView_InsertItem(list, &item); ++listStructuralChanges; }
        else if (displayedLabels[i] != accessible) ListView_SetItem(list, &item);
        displayedLabels[i] = std::move(accessible);
        if (selected && selected->pid == row.pid && selected->created == row.created)
            ListView_SetItemState(list, int(i), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    if (anchor) for (size_t i = 0; i < displayed.size(); ++i) {
        if (displayed[i].pid == anchor->pid && displayed[i].created == anchor->created) {
            preserved = i * px(RowHeight) + std::fmod(preserved, px(RowHeight)); break;
        }
    }
    applyScroll(preserved); stopScroll();
    fitListColumn();
    SendMessageW(list, WM_SETREDRAW, TRUE, 0); InvalidateRect(list, nullptr, TRUE);
    refreshingList = false;
    hoveredRow = -1; updateRowFeedback();
    updateButtons();
}
void layout() {
    RECT r{}; GetClientRect(mainWindow, &r);
    const int w = r.right, h = r.bottom - tabBarHeight(), margin = sp(18), top = listTop();
    const int bottom = h - sp(144), inner = w - margin * 2, gap = sp(8), half = (inner - gap) / 2;
    MoveWindow(viewport, margin, top, inner, std::max(sp(40), bottom - top), TRUE);
    applyScroll(scrollPosition); stopScroll();
    fitListColumn();
    MoveWindow(detailsButton, margin, bottom + sp(10), half, sp(32), TRUE);
    MoveWindow(closeButton, margin + half + gap, bottom + sp(10), half, sp(32), TRUE);
    MoveWindow(serviceButton, margin, bottom + sp(50), half, sp(32), TRUE);
    MoveWindow(killButton, margin + half + gap, bottom + sp(50), half, sp(32), TRUE);
    MoveWindow(backgroundCheck, margin, top - sp(38), half, sp(28), TRUE);
    MoveWindow(allCheck, margin + half + gap, top - sp(38), half, sp(28), TRUE);
    MoveWindow(powerButton, w - sp(108), sp(47), sp(88), sp(22), TRUE);
    MoveWindow(menuButton, w - sp(92), sp(16), sp(32), sp(30), TRUE);
    MoveWindow(hideButton, w - sp(52), sp(16), sp(32), sp(30), TRUE);
    const bool fullPage = inlinePage == InlinePage::process || inlinePage == InlinePage::powerMode || inlinePage == InlinePage::history || inlinePage == InlinePage::lab || inlinePage == InlinePage::ledger || inlinePage == InlinePage::hardware ||
        inlinePage == InlinePage::settings || inlinePage == InlinePage::dynamic || inlinePage == InlinePage::goal || inlinePage == InlinePage::performance ||
        inlinePage == InlinePage::confirm || inlinePage == InlinePage::error;
    const bool section = sectionPage(inlinePage);
    const int textTop = inlinePage == InlinePage::powerMode ? sp(318) : section ? sp(146) : fullPage ? sp(128) : top - sp(37);
    {
        const int pageLeft = margin + sp(12), pageWidth = inner - sp(24), modeGap = sp(6);
        const int third = (pageWidth - 2 * modeGap) / 3, pageHalf = (pageWidth - modeGap) / 2;
        for (int i = 0; i < 6; ++i)
            MoveWindow(modeButtons[i], pageLeft + (i % 3) * (third + modeGap), i < 3 ? sp(176) : sp(234), third, sp(30), TRUE);
        MoveWindow(restoreModesButton, pageLeft, sp(276), pageHalf, sp(30), TRUE);
        MoveWindow(energySettingsButton, pageLeft + pageHalf + modeGap, sp(276), pageWidth - pageHalf - modeGap, sp(30), TRUE);
    }
    const int textBottom = inlinePage == InlinePage::process ? h - sp(140) : section ? h - sp(24) : h - sp(94);
    MoveWindow(inlineText, margin + sp(12), textTop, inner - sp(24), std::max(sp(55), textBottom - textTop), TRUE);
    if (visualView) MoveWindow(visualView, margin + sp(12), textTop, inner - sp(24), std::max(sp(55), textBottom - textTop), TRUE);
    if (settingsList) MoveWindow(settingsList, margin + sp(8), textTop - sp(4), inner - sp(16), std::max(sp(55), textBottom - textTop + sp(4)), TRUE);
    {
        const int pageLeft = margin + sp(12), pageWidth = inner - sp(24), pageHalf = (pageWidth - sp(8)) / 2;
        MoveWindow(ecoButton, pageLeft, h - sp(103), pageHalf, sp(32), TRUE);
        MoveWindow(perfAppButton, pageLeft + pageHalf + sp(8), h - sp(103), pageWidth - pageHalf - sp(8), sp(32), TRUE);
    }
    SendMessageW(inlineText, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(sp(4), sp(4)));
    const int inlineWidth = inner - sp(24);
    MoveWindow(inlineBack, margin + sp(12), h - sp(59),
        inlinePage == InlinePage::confirm ? (inlineWidth - sp(8)) / 2 : inlineWidth, sp(34), TRUE);
    MoveWindow(inlineConfirm, margin + sp(12) + (inlineWidth - sp(8)) / 2 + sp(8), h - sp(59),
        (inlineWidth - sp(8)) / 2, sp(34), TRUE);
    InvalidateRect(mainWindow, nullptr, FALSE);
}
void resizeForInlinePage(InlinePage page) {
    RECT rect{}; GetWindowRect(mainWindow, &rect);
    if (page != InlinePage::none && inlinePage == InlinePage::none) expandedPanelHeight = rect.bottom - rect.top;
    int target = page == InlinePage::confirm ? px(320) : page == InlinePage::error ? px(360) :
        page == InlinePage::none ? expandedPanelHeight : expandedPanelHeight;
    if (target <= 0) return;
    double now = motionNow();
    if (!panelHeightMotion.duration) panelHeightMotion.to = rect.bottom - rect.top;
    panelHeightMotion.move(target, now, canAnimate() && panelTargetShown ? 190 : 0);
    stepMotion(now);
    if (panelHeightMotion.active(now)) armMotionTimer();
}
void showInlinePage(InlinePage page, const std::wstring& title, const std::wstring& content) {
    resizeForInlinePage(page);
    inlinePage = page; inlineTitle = title;
    std::wstring lines; lines.reserve(content.size() + 32);
    for (size_t i = 0; i < content.size(); ++i) {
        if (content[i] == L'\n' && (i == 0 || content[i - 1] != L'\r')) lines += L'\r';
        lines += content[i];
    }
    SetWindowTextW(inlineText, lines.c_str());
    ShowScrollBar(inlineText, SB_VERT, page == InlinePage::process || page == InlinePage::power || page == InlinePage::ledger ||
        page == InlinePage::powerMode || page == InlinePage::history || page == InlinePage::lab || page == InlinePage::dynamic || page == InlinePage::goal ||
        (page == InlinePage::candidates && lines.size() > 500));
    SendMessageW(inlineText, EM_SETSEL, 0, 0);
    SendMessageW(inlineText, EM_SCROLLCARET, 0, 0);
    for (auto control : {viewport, closeButton, killButton, serviceButton, detailsButton, backgroundCheck, allCheck}) ShowWindow(control, SW_HIDE);
    ShowWindow(inlineText, page == InlinePage::settings ? SW_HIDE : SW_SHOW);
    ShowWindow(inlineBack, sectionPage(page) ? SW_HIDE : SW_SHOW); // sections are left through the tabs (or Esc)
    if (visualView) ShowWindow(visualView, SW_HIDE); // a chart page shows it again with showVisual
    if (settingsList) ShowWindow(settingsList, page == InlinePage::settings ? SW_SHOW : SW_HIDE);
    ShowWindow(inlineConfirm, page == InlinePage::confirm ? SW_SHOW : SW_HIDE);
    for (HWND control : modeButtons) ShowWindow(control, page == InlinePage::powerMode ? SW_SHOW : SW_HIDE);
    for (HWND control : {ecoButton, perfAppButton}) ShowWindow(control, page == InlinePage::process ? SW_SHOW : SW_HIDE);
    for (HWND control : {restoreModesButton, energySettingsButton}) ShowWindow(control, page == InlinePage::powerMode ? SW_SHOW : SW_HIDE);
    SetWindowTextW(inlineBack, page == InlinePage::confirm ? tr(L"取消") : tr(L"返回排行"));
    layout();
    HWND focus = page == InlinePage::confirm ? inlineBack : page == InlinePage::settings ? settingsList : inlineText;
    if (page == InlinePage::powerMode) {
        focus = inlineBack;
        for (HWND control : modeButtons) if (IsWindowEnabled(control)) { focus = control; break; }
    }
    SetFocus(focus);
}
void finishInlineClose() {
    inlinePage = InlinePage::none; inlineTitle.clear(); inlineProcess.reset(); pendingForceRow.reset();
    inlineClosing = false;
    ShowWindow(inlineText, SW_HIDE); ShowWindow(inlineBack, SW_HIDE); ShowWindow(inlineConfirm, SW_HIDE);
    if (visualView) ShowWindow(visualView, SW_HIDE);
    if (settingsList) ShowWindow(settingsList, SW_HIDE);
    for (HWND control : modeButtons) ShowWindow(control, SW_HIDE);
    ShowWindow(restoreModesButton, SW_HIDE); ShowWindow(energySettingsButton, SW_HIDE);
    ShowWindow(ecoButton, SW_HIDE); ShowWindow(perfAppButton, SW_HIDE);
    for (auto control : {viewport, closeButton, killButton, serviceButton, detailsButton, backgroundCheck, allCheck}) ShowWindow(control, SW_SHOW);
    layout(); updateButtons(); SetFocus(list);
    if (listRefreshPending) PostMessageW(mainWindow, RefreshMessage, 0, 0);
}
void closeInlinePage(bool immediate) {
    if (inlinePage == InlinePage::none) return;
    if (!immediate && canAnimate() && panelTargetShown &&
        (inlinePage == InlinePage::confirm || inlinePage == InlinePage::error)) {
        inlineClosing = true;
        resizeForInlinePage(InlinePage::none);
        return;
    }
    if (immediate) {
        inlineClosing = false;
        bool oldSuppress = suppressMotion; suppressMotion = true;
        resizeForInlinePage(InlinePage::none);
        suppressMotion = oldSuppress;
    } else resizeForInlinePage(InlinePage::none);
    finishInlineClose();
}
void drawButton(const DRAWITEMSTRUCT& item) {
    RECT r = item.rcItem; HDC dc = item.hDC;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const UINT id = item.CtlID;
    const bool inlineButton = id == InlineBackId || id == InlineConfirmId || isModeButton(id) || id == RestoreModesId || id == EnergySaverSettingsId ||
        id == EcoToggleId || id == PerfAppToggleId;
    const COLORREF behind = inlineButton ? Surface : Background;
    fill(dc, r, behind);
    const double hover = disabled ? 0 : buttonMotions[item.hwndItem].value(motionNow());
    const double press = pressMotions[item.hwndItem].value(motionNow());
    const int inset = int(std::lround(px(1) * press));
    InflateRect(&r, -inset, -inset);
    wchar_t title[128]{}; GetWindowTextW(item.hwndItem, title, int(std::size(title)));
    const float radius = id == PowerModeId ? (r.bottom - r.top) / 2.0f : float(sp(9));
    COLORREF face = theme.raised, edgeColor = CLR_INVALID, text = Ink;
    HFONT faceFont = normalFont;
    if (id == MenuId || id == HideId) {
        face = hover > .01 || pressed ? blend(behind, theme.raised, std::max(hover, pressed ? 1.0 : 0.0)) : behind;
        if (face != behind) rounded(dc, r, face, 9);
        if (theme.highContrast && hover > .5) outline(dc, r, Border, 9);
        const COLORREF glyph = disabled ? theme.faint : blend(Muted, Ink, hover);
        if (pixelStyle) {
            const int glyphUnit = std::max(pixelUnit(), px(3));
            if (id == MenuId) ui::pixelDots(dc, r, glyph, glyphUnit); else ui::pixelChevron(dc, r, glyph, glyphUnit);
        } else if (id == MenuId) ui::drawDots(dc, r, glyph, std::max(1.2f, sp(2) * .8f));
        else ui::drawChevron(dc, r, glyph, std::max(1.5f, float(sp(2))));
    } else {
        const bool checked = (id == BackgroundId && backgroundOnly) || (id == AllId && showAll) || (id == PowerModeId && powerSaving) || modeSelected(id) ||
            (id == EcoToggleId && inlineProcess && isEfficient(*inlineProcess)) || (id == PerfAppToggleId && inlineProcess && performanceListed(*inlineProcess));
        if (disabled && checked && isModeButton(id)) {
            face = theme.accentSoft; edgeColor = blend(Green, behind, .55); text = blend(Green, behind, .25);
            if (theme.highContrast) { face = theme.selected; text = theme.selectedInk; edgeColor = CLR_INVALID; }
        } else if (disabled) {
            face = blend(Surface, behind, .35); text = theme.faint;
            if (theme.highContrast) edgeColor = theme.faint;
        } else if (theme.highContrast && (id == CloseId || id == InlineConfirmId || checked)) {
            face = theme.selected; text = theme.selectedInk; faceFont = id == CloseId || id == InlineConfirmId ? strongFont : normalFont;
        } else if (id == CloseId) {
            face = blend(Green, Ink, hover * .14); text = theme.accentInk; faceFont = strongFont;
        } else if (id == InlineConfirmId) {
            face = blend(Danger, Ink, hover * .14); text = theme.dark ? Background : RGB(255, 255, 255); faceFont = strongFont;
        } else if (id == KillId) {
            face = blend(Surface, theme.raised, hover); edgeColor = blend(Danger, behind, .45); text = Danger;
        } else if (id == BackgroundId || id == AllId || id == PowerModeId || isModeButton(id) || id == EcoToggleId || id == PerfAppToggleId) {
            if (checked) {
                face = id == PowerModeId ? Green : theme.accentSoft;
                edgeColor = id == PowerModeId ? CLR_INVALID : blend(Green, behind, .35);
                text = id == PowerModeId ? theme.accentInk : Green;
                if (id == PowerModeId) face = blend(Green, Ink, hover * .14);
            } else {
                face = blend(id == PowerModeId ? theme.accentSoft : Surface, theme.raised, hover);
                edgeColor = id == PowerModeId ? CLR_INVALID : Border;
                text = id == PowerModeId ? Green : Muted;
            }
        } else {
            face = blend(theme.raised, Border, hover * .6);
        }
        if (pressed) face = blend(face, Ink, .1);
        if (pixelStyle) {
            // Raised pixel button: one-unit outline and drop shadow; pressing sinks it into the shadow.
            const int u = pixelUnit();
            const bool down = pressed || press > .5;
            RECT body = item.rcItem; body.bottom -= u;
            if (down) OffsetRect(&body, 0, u);
            const COLORREF edge = edgeColor != CLR_INVALID ? edgeColor : theme.highContrast ? Border
                : blend(face, theme.dark ? RGB(0, 0, 0) : Ink, theme.dark ? .45 : .3);
            if (!down && !disabled) ui::pixelFill(dc, {body.left, body.top + u, body.right, body.bottom + u}, blend(edge, behind, .25), u, 2);
            ui::pixelFill(dc, body, face, u, 2);
            ui::pixelFrame(dc, body, edge, u, 2);
            drawText(dc, title, body, text, faceFont, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        } else {
            ui::fillRound(dc, r, face, radius);
            if (theme.highContrast && edgeColor == CLR_INVALID) edgeColor = Border;
            if (edgeColor != CLR_INVALID) ui::strokeRound(dc, r, edgeColor, radius, 1.0f);
            drawText(dc, title, r, text, faceFont, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        }
    }
    if ((item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT)) {
        RECT ring = item.rcItem;
        const COLORREF color = theme.highContrast ? Ink : blend(Green, behind, .15);
        if (pixelStyle) ui::pixelFrame(dc, ring, color, pixelUnit(), 2);
        else ui::strokeRound(dc, ring, color, radius + 1, float(std::max(2, px(2))));
    }
}
void drawProcess(const DRAWITEMSTRUCT& item) {
    if (item.itemID >= displayed.size()) return;
    const auto& row = displayed[item.itemID]; HDC dc = item.hDC; RECT r = item.rcItem;
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    fill(dc, r, Background);
    const double focus = item.itemID < rowMotions.size() ? rowMotions[item.itemID].selected.value(motionNow()) : double(selected);
    const double hover = item.itemID < rowMotions.size() ? rowMotions[item.itemID].hover.value(motionNow()) : 0;
    RECT tile = r; tile.bottom -= sp(4);
    const COLORREF tileColor = blend(blend(Surface, theme.raised, hover), Selected, focus);
    if (pixelStyle) ui::pixelBox(dc, tile, tileColor, selected ? Green : theme.highContrast ? Border : blend(Border, tileColor, .3), pixelUnit(), 2);
    else {
        rounded(dc, tile, tileColor, 10);
        if (theme.highContrast) outline(dc, tile, selected ? Green : Border, 10, selected ? 2.0f : 1.0f);
    }
    if (focus > .01) rounded(dc, {r.left + sp(3), r.top + sp(14), r.left + sp(6), tile.bottom - sp(14)}, blend(tileColor, Green, focus), 2);
    const bool inverted = theme.highContrast && selected;
    const COLORREF ink = inverted ? theme.selectedInk : Ink, sub = inverted ? theme.selectedInk : Muted, accent = inverted ? theme.selectedInk : Green;
    // Application icon, or a letter tile for processes without a readable image path.
    const int iconSize = sp(24), iconX = r.left + sp(12), iconY = tile.top + (tile.bottom - tile.top - iconSize) / 2 - sp(2);
    std::wstring name = row.services.size() == 1 ? row.services.front().display : tr(row.name.c_str());
    if (HICON icon = processIcons.get(row.path, iconSize)) DrawIconEx(dc, iconX, iconY, icon, iconSize, iconSize, 0, nullptr, DI_NORMAL);
    else {
        RECT avatar{iconX, iconY, iconX + iconSize, iconY + iconSize};
        rounded(dc, avatar, blend(tileColor, Green, .2), 6);
        wchar_t letter = L'?';
        for (wchar_t c : name) if (iswalnum(c)) { letter = wchar_t(towupper(c)); break; }
        drawText(dc, std::wstring(1, letter), avatar, accent, strongFont, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
    const int left = iconX + iconSize + sp(10), right = r.right - sp(12), y = r.top;
    if (row.services.size() > 1) name += L" · " + std::to_wstring(row.services.size()) + tr(L" 項服務");
    drawText(dc, name, {left, y + sp(7), right - sp(90), y + sp(28)}, ink, normalFont);
    drawText(dc, valueText(row.averageCpuWatts, L" W", 2), {right - sp(90), y + sp(6), right, y + sp(29)},
        accent, metricFont, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    const std::wstring kind = std::wstring(tr(row.kind.c_str())) + (isEfficient(row) ? tr(L" · 效率") : L"");
    RECT chip{left, y + sp(31), left + textWidth(dc, kind, smallFont) + sp(12), y + sp(45)};
    rounded(dc, chip, inverted ? tileColor : blend(tileColor, Ink, theme.dark ? .08 : .06), 5);
    if (theme.highContrast) outline(dc, chip, sub, 5);
    drawText(dc, kind, chip, sub, smallFont, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    drawText(dc, L"PID " + std::to_wstring(row.pid), {chip.right + sp(6), y + sp(29), right - sp(158), y + sp(47)}, sub, smallFont);
    // CPU share is easier to read at a glance; the watt range is on the details page.
    std::wstring range = L"CPU " + (row.measured ? number(row.cpu) + L"%" : L"—");
    if (row.gpuPercent && *row.gpuPercent >= 1) range += L" · GPU " + number(*row.gpuPercent, 0) + L"%";
    drawText(dc, range, {right - sp(156), y + sp(29), right, y + sp(47)}, sub, smallFont, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    if (row.averageCpuWatts && !displayed.empty() && displayed.front().averageCpuWatts && *displayed.front().averageCpuWatts > 0) {
        const double ratio = item.itemID < rowMotions.size() ? rowMotions[item.itemID].bar.value(motionNow()) : 0;
        const int barTop = y + sp(49), barBottom = barTop + std::max(2, sp(3));
        if (pixelStyle) ui::pixelSegments(dc, {left, barTop, right, barTop + pixelUnit() * 2}, ratio,
            selected ? accent : theme.accentDim, blend(tileColor, Border, .9), pixelUnit());
        else {
            rounded(dc, {left, barTop, right, barBottom}, blend(tileColor, Border, .9), 2);
            const int filled = int((right - left) * std::clamp(ratio, 0.0, 1.0));
            if (filled > 0) rounded(dc, {left, barTop, left + std::max(filled, sp(3)), barBottom}, selected ? accent : theme.accentDim, 2);
        }
    }
    if ((item.itemState & ODS_FOCUS) && GetFocus() == list && !(SendMessageW(list, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
        if (!pixelStyle) InflateRect(&tile, -sp(1), -sp(1));
        outline(dc, tile, theme.highContrast ? ink : blend(Green, tileColor, .25), 10, float(std::max(2, px(2))));
    }
}
void paint(HDC dc, RECT bounds) {
    fill(dc, bounds, Background);
    drawTabs(dc, bounds);
    // Everything else sits above the tab bar.
    const int w = bounds.right, h = bounds.bottom - tabBarHeight(), margin = sp(18), right = w - margin;
    const bool compact = compactLayout(h), minimal = minimalLayout(h);
    const int top = listTop();
    if (headerIcon) DrawIconEx(dc, margin, sp(15), headerIcon, sp(22), sp(22), 0, nullptr, DI_NORMAL);
    drawText(dc, L"BatteryScope", {margin + sp(30), sp(11), w - sp(98), sp(40)}, Ink, titleFont);
    mascotRect = {};
    if (showMascot) {
        const int unit = std::max(2, px(2)), left = margin + sp(30) + textWidth(dc, L"BatteryScope", titleFont) + sp(10);
        if (left + ui::MascotCells * unit <= w - sp(112)) {
            const auto mood = currentMood();
            const double level = mascotLevel();
            ui::drawMascot(dc, left, sp(9), unit, mood, level, sampleTick % 5 == 4 ? 1 : 0, mascotColors(mood));
            mascotRect = {left, sp(9), left + ui::MascotCells * unit, sp(9) + ui::MascotRows * unit};
        }
    }
    drawText(dc, demo ? tr(L"示範資料 · 關閉操作已停用") : tr(L"電池與背景活動"), {margin + sp(30), sp(39), w - sp(96), sp(60)}, Muted, smallFont);
    if (inlinePage == InlinePage::process || inlinePage == InlinePage::powerMode || inlinePage == InlinePage::history || inlinePage == InlinePage::ledger || inlinePage == InlinePage::hardware ||
        inlinePage == InlinePage::lab || inlinePage == InlinePage::settings || inlinePage == InlinePage::dynamic || inlinePage == InlinePage::goal ||
        inlinePage == InlinePage::performance || inlinePage == InlinePage::confirm || inlinePage == InlinePage::error) {
        card(dc, {margin, sp(74), right, h - sp(14)}, Surface, 14);
        drawText(dc, inlineTitle, {margin + sp(14), sp(84), right - sp(14), sp(114)}, Ink, titleFont);
        fill(dc, {margin + sp(14), sp(119), right - sp(14), sp(119) + 1}, Border);
        if (inlinePage == InlinePage::powerMode) {
            const bool plugged = pluggedInNow();
            const std::wstring now = effectiveMode == EffectiveMode::unknown ? tr(L"目前生效：無法讀取") :
                tr(L"目前生效：") + std::wstring(effectiveName(effectiveMode)) + (plugged ? tr(L"（插電中）") : tr(L"（用電池）"));
            drawText(dc, now, {margin + sp(14), sp(126), right - sp(14), sp(148)}, Ink, strongFont);
            drawText(dc, std::wstring(tr(L"插電時")) + (plugged ? tr(L" · 目前") : L""), {margin + sp(14), sp(154), right - sp(14), sp(174)},
                plugged ? Green : Muted, smallFont);
            drawText(dc, std::wstring(tr(L"用電池時")) + (!plugged ? tr(L" · 目前") : L""), {margin + sp(14), sp(212), right - sp(14), sp(232)},
                !plugged ? Green : Muted, smallFont);
        }
        return; // notices show as a card above the tabs
    }
    PowerSample p; if (!current.history.empty()) p = current.history.back();
    const auto& m = current.power;
    const auto charge = calculateCharging(current.history);
    const bool plugged = p.acKnown && (p.onAc || p.charging);
    // Without a battery (desktops), the hero shows CPU power from the energy counters instead.
    const bool desktop = !current.history.empty() && !p.present;
    const bool energyLive = current.energy.ready && !current.energy.timingMismatch;
    Value desktopWatts;
    if (energyLive) desktopWatts = current.energy.packageWatts ? current.energy.packageWatts : current.energy.coreWatts;
    std::wstring currentW = current.history.empty() ? tr(L"讀取中") : desktop ? (desktopWatts ? valueText(desktopWatts, L" W") : tr(L"無電池")) : p.charging ? valueText(charge.watts, L" W") : plugged ? tr(L"插電中") : valueText(m.currentWatts, L" W");
    // Plugged in without charging, the useful fact is which power mode is in effect.
    std::wstring rateLabel = desktop ? (desktopWatts ? (current.energy.packageWatts ? tr(L"整顆 CPU，硬體量測") : tr(L"CPU 核心，硬體量測"))
        : tr(L"仍可監測程序 CPU")) : !p.present ? tr(L"仍可監測程序 CPU") : p.charging ? tr(L"電池實際吸收的功率") :
        plugged ? (effectiveMode != EffectiveMode::unknown ? tr(L"電源模式：") + std::wstring(effectiveName(effectiveMode)) : tr(L"未充電")) :
        m.estimated ? tr(L"依電量變化估算") : tr(L"整台電腦目前的耗電");
    if (minimal && m.percentPerHour) rateLabel = number(*m.percentPerHour) + tr(L" 百分點 / 小時");
    // Hero card: power on the left, battery level on the right.
    card(dc, {margin, sp(72), right, sp(180)}, Surface, 16);
    drawText(dc, desktop ? tr(L"CPU 功率") : p.charging ? tr(L"充電功率") : plugged ? tr(L"外部電源") : tr(L"目前耗電"), {margin + sp(16), sp(84), right - sp(122), sp(103)}, Muted, smallFont);
    const int digitUnit = std::max(3, px(4));
    if (!pixelStyle || !ui::drawPixelDigits(dc, currentW, margin + sp(16), (sp(102) + sp(150)) / 2 - digitUnit * 7 / 2, digitUnit, Ink))
        drawText(dc, currentW, {margin + sp(14), sp(102), right - sp(119), sp(150)}, Ink, valueFont);
    drawText(dc, rateLabel, {margin + sp(16), sp(152), right - sp(150), sp(172)}, Muted, smallFont);
    const bool low = p.percent && !plugged && *p.percent <= 20;
    const COLORREF level = !p.percent ? Muted : p.charging ? Green : low && *p.percent <= 10 ? Danger : low ? theme.warn : Green;
    const std::wstring percentText = desktop ? std::wstring() : valueText(p.percent, L"%", 0);
    const int percentUnit = pixelUnit(), percentWidth = ui::pixelDigitsWidth(percentText, percentUnit);
    if (!pixelStyle || !ui::drawPixelDigits(dc, percentText, right - sp(16) - percentWidth, (sp(84) + sp(116)) / 2 - percentUnit * 7 / 2,
        percentUnit, p.percent ? Ink : Muted))
        drawText(dc, percentText, {right - sp(112), sp(84), right - sp(16), sp(116)},
            p.percent ? Ink : Muted, titleFont, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    if (desktop) {} // no battery to draw
    else if (pixelStyle) ui::pixelBattery(dc, {right - sp(16) - sp(64), sp(120), right - sp(16), sp(144)}, p.percent ? *p.percent / 100 : 0,
        blend(Muted, Surface, .25), level, blend(Surface, Border, .7), p.charging, Ink, Surface, pixelUnit());
    else ui::drawBattery(dc, {right - sp(16) - sp(60), sp(121), right - sp(16), sp(143)}, p.percent ? *p.percent / 100 : 0,
        blend(Muted, Surface, .25), level, p.charging, Ink, Surface);
    const bool atLimit = plugged && !p.charging && chargeLimit && p.percent && std::abs(*p.percent - *chargeLimit) <= 1.5;
    drawText(dc, desktop ? tr(L"桌上型電腦") : p.charging ? tr(L"充電中") : atLimit ? tr(L"充電上限 ") + std::to_wstring(*chargeLimit) + L"%" :
        plugged ? (p.percent && *p.percent >= 99.5 ? tr(L"已充滿") : tr(L"未充電")) : tr(L"剩餘電量"),
        {right - sp(146), sp(151), right - sp(16), sp(172)}, Muted, smallFont, DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    if (!minimal) {
        const int gap = sp(8), half = (right - margin - gap) / 2;
        const RECT rateCard{margin, sp(187), margin + half, sp(228)}, timeCard{margin + half + gap, sp(187), right, sp(228)};
        card(dc, rateCard, Surface, 12); card(dc, timeCard, Surface, 12);
        batteryCardsRect = desktop ? RECT{} : RECT{rateCard.left, rateCard.top, timeCard.right, timeCard.bottom};
        if (!desktop) drawMenuArrow(dc, timeCard.right - sp(16), (timeCard.top + timeCard.bottom) / 2 - sp(4), sp(8), Muted, false); // opens the details
        // Plugged in without charging, battery health is more useful than a rate.
        const auto health = batteryHealth(p);
        const bool showHealth = plugged && !p.charging && health;
        std::wstring healthText = showHealth ? number(*health, 0) + L"%" : L"";
        if (showHealth && p.cycleCount) healthText += L" · " + std::to_wstring(*p.cycleCount) + tr(L" 次循環");
        if (desktop) { // no battery: show the CPU energy split instead
            drawText(dc, tr(L"CPU 核心"), {rateCard.left + sp(12), rateCard.top + sp(4), rateCard.right - sp(8), rateCard.top + sp(20)}, Muted, smallFont);
            drawText(dc, energyLive ? valueText(current.energy.coreWatts, L" W") : std::wstring(L"—"),
                {rateCard.left + sp(12), rateCard.top + sp(19), rateCard.right - sp(8), rateCard.bottom - sp(3)}, Ink, strongFont);
            drawText(dc, tr(L"未歸因給程式"), {timeCard.left + sp(12), timeCard.top + sp(4), timeCard.right - sp(22), timeCard.top + sp(20)}, Muted, smallFont);
            drawText(dc, energyLive ? valueText(current.energy.unassignedCpuWatts, L" W") : std::wstring(L"—"),
                {timeCard.left + sp(12), timeCard.top + sp(19), timeCard.right - sp(22), timeCard.bottom - sp(3)}, Ink, strongFont);
        } else {
        drawText(dc, p.charging ? tr(L"充電速度") : showHealth ? tr(L"電池健康度") : plugged ? tr(L"電池狀態") : tr(L"耗電速度"),
            {rateCard.left + sp(12), rateCard.top + sp(4), rateCard.right - sp(8), rateCard.top + sp(20)}, Muted, smallFont);
        drawText(dc, showHealth ? healthText : plugged && !p.charging ? (p.percent && *p.percent >= 99.5 ? tr(L"已充滿") : tr(L"已接電，暫未充電")) : valueText(p.charging ? charge.percentPerHour : m.percentPerHour, tr(L" 百分點 / 小時")),
            {rateCard.left + sp(12), rateCard.top + sp(19), rateCard.right - sp(8), rateCard.bottom - sp(3)}, Ink, strongFont);
        std::wstring remaining = tr(L"估計續航 —");
        if (plugged) remaining = p.charging ? tr(L"等待穩定充電資料") : valueText(p.remainingWh, L"", 1) + L" / " + valueText(p.fullWh, L" Wh", 1);
        const auto eta = chargeEta(charge, p);
        if (auto text = p.charging ? durationText(eta.hours) : remainingText(m); !text.empty()) remaining = text;
        // Plugged in without charging there is no time to show, so show stored energy instead.
        drawText(dc, plugged ? (p.charging ? etaLabel(eta) : tr(L"剩餘容量")) : tr(L"近 5 分鐘 ") + valueText(m.averageWatts, L" W"),
            {timeCard.left + sp(12), timeCard.top + sp(4), timeCard.right - sp(22), timeCard.top + sp(20)}, Muted, smallFont);
        drawText(dc, remaining, {timeCard.left + sp(12), timeCard.top + sp(19), timeCard.right - sp(22), timeCard.bottom - sp(3)}, Ink, strongFont);
        }
    }
    if (!compact) {
        RECT chart{margin, sp(234), right, sp(278)}; // leaves room above the ranking's heading
        card(dc, chart, Surface, 12);
        const bool chargingChart = p.charging;
        auto chartPower = [chargingChart](const PowerSample& point) -> Value {
            if (!point.present || !point.acKnown || !point.watts || !std::isfinite(*point.watts)) return {};
            if (chargingChart) return point.onAc && point.charging && *point.watts < 0 ? Value(-*point.watts) : Value{};
            return !point.onAc && !point.charging && *point.watts >= 0 ? point.watts : Value{};
        };
        // Desktops chart CPU power from the energy counters; laptops chart the battery.
        std::vector<std::pair<uint64_t, Value>> series;
        if (desktop) series.assign(cpuPowerHistory.begin(), cpuPowerHistory.end());
        else for (const auto& point : current.history) series.emplace_back(point.ms, chartPower(point));
        drawText(dc, desktop ? tr(L"30 分鐘 · CPU W") : chargingChart ? tr(L"30 分鐘 · 充電 W") : tr(L"30 分鐘 · 放電 W"), {chart.left + sp(12), chart.top + sp(5), chart.right - sp(90), chart.top + sp(21)}, Muted, smallFont);
        RECT plot{chart.left + sp(12), chart.top + sp(24), chart.right - sp(12), chart.bottom - sp(6)};
        double maximum = 10;
        uint64_t last = current.history.empty() ? GetTickCount64() : current.history.back().ms;
        if (desktop && !series.empty()) last = std::max(last, series.back().first);
        for (const auto& point : series) if (const auto& watts = point.second) maximum = std::max(maximum, std::ceil(*watts / 5) * 5);
        const int middle = (plot.top + plot.bottom) / 2;
        if (pixelStyle) ui::pixelDottedLine(dc, {plot.left, middle}, {plot.right, middle}, blend(Border, Surface, .1), pixelUnit());
        else ui::drawDashedLine(dc, {plot.left, middle}, {plot.right, middle}, blend(Border, Surface, .2), 1.0f);
        fill(dc, {plot.left, plot.bottom, plot.right, plot.bottom + (pixelStyle ? pixelUnit() / 2 : 1)}, Border);
        std::vector<std::vector<POINT>> segments;
        uint64_t previous = 0; bool connected = false;
        for (const auto& [ms, watts] : series) {
            if (!watts || ms > last) { connected = false; continue; }
            POINT at{plot.right - int((last - ms) / 1800000.0 * (plot.right - plot.left)),
                plot.bottom - int(std::min(*watts, maximum) / maximum * (plot.bottom - plot.top))};
            if (!connected || ms - previous > 30000) segments.emplace_back();
            segments.back().push_back(at);
            connected = true; previous = ms;
        }
        bool any = false;
        if (pixelStyle) {
            // Pixel bar chart: two-unit columns with a one-unit gap, each the mean of its time slice.
            const int u = pixelUnit(), pitch = u * 3, columns = std::max(1, int(plot.right - plot.left) / pitch);
            const int start = plot.right - columns * pitch;
            std::vector<double> sum(size_t(columns), 0), count(size_t(columns), 0);
            for (const auto& [ms, watts] : series) {
                if (!watts || ms > last || last - ms > 1800000) continue;
                const int column = std::clamp(columns - 1 - int((last - ms) / 1800000.0 * columns), 0, columns - 1);
                sum[size_t(column)] += std::min(*watts, maximum); count[size_t(column)] += 1;
            }
            for (int column = 0; column < columns; ++column) {
                if (!count[size_t(column)]) continue;
                any = true;
                const int height = std::max(u, int(sum[size_t(column)] / count[size_t(column)] / maximum * (plot.bottom - plot.top)) / u * u);
                const int x = start + column * pitch;
                fill(dc, {x, plot.bottom - height, x + 2 * u, plot.bottom}, blend(Green, Surface, theme.highContrast ? 0 : .55));
                fill(dc, {x, plot.bottom - height, x + 2 * u, plot.bottom - height + u}, Green);
            }
        } else {
            for (const auto& segment : segments) any |= segment.size() > 1;
            if (any) ui::drawSeries(dc, plot, segments, Green, std::max(1.5f, float(sp(2))), Green);
        }
        for (auto time : actionMarks) if (!chargingChart && !desktop && time <= last && last - time <= 1800000) {
            int x = plot.right - int((last - time) / 1800000.0 * (plot.right - plot.left));
            if (pixelStyle) ui::pixelDottedLine(dc, {x, plot.top}, {x, plot.bottom}, theme.warn, pixelUnit());
            else ui::drawDashedLine(dc, {x, plot.top}, {x, plot.bottom}, theme.warn, 1.0f);
        }
        if (!any) drawText(dc, desktop ? tr(L"沒有 CPU 能源資料") : chargingChart ? tr(L"等待充電資料") : plugged ? tr(L"插電期間暫停放電曲線") : tr(L"等待放電資料"), plot, Muted, smallFont, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        drawText(dc, L"0–" + number(maximum, 0) + L" W", {chart.right - sp(90), chart.top + sp(5), chart.right - sp(12), chart.top + sp(21)},
            Muted, smallFont, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    }
    const bool energyReady = current.energy.ready && !current.energy.timingMismatch;
    if (inlinePage != InlinePage::none) {
        card(dc, {margin, top - sp(77), right, h - sp(14)}, Surface, 14);
        drawText(dc, inlineTitle, {margin + sp(14), top - sp(72), right - sp(14), top - sp(45)}, Ink, titleFont);
        fill(dc, {margin + sp(14), top - sp(41), right - sp(14), top - sp(40)}, Border);
        return;
    }
    const std::wstring sortLabel = energyReady ? tr(L"估計 W ↓") : tr(L"活動排序 ↓");
    const int chipWidth = textWidth(dc, sortLabel, smallFont) + sp(16);
    drawText(dc, tr(L"CPU 耗電排行"), {margin, top - sp(78), right - chipWidth - sp(8), top - sp(56)}, Ink, strongFont);
    const RECT sortChip{right - chipWidth, top - sp(76), right, top - sp(58)};
    rounded(dc, sortChip, theme.accentSoft, 9);
    if (theme.highContrast) outline(dc, sortChip, Green, 9);
    drawText(dc, sortLabel, sortChip, Green, smallFont, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    drawText(dc, energyReady ? tr(L"30 秒平均 · 已觀測 ") + number(current.energy.observedCpuCoverage * 100, 0) + tr(L"% 的 CPU 時間") : tr(L"能源暫不可用，依 CPU / I/O 活動排序"),
        {margin, top - sp(56), right, top - sp(40)}, Muted, smallFont);
    if (displayed.empty()) {
        drawText(dc, backgroundOnly ? tr(L"目前沒有符合篩選的程序") : tr(L"正在讀取程序…"),
            {margin, top + sp(12), right, top + sp(44)}, Muted, normalFont, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
    int footer = h - sp(51);
    std::wstring status = current.processes.empty() ? notice : selectionHint();
    drawText(dc, status, {margin, footer, right, footer + sp(19)}, Muted, smallFont);
    drawText(dc, tr(L"個別瓦數僅估計 CPU，非程式總耗電"),
        {margin, footer + sp(21), right, footer + sp(39)}, Muted, smallFont);
}
std::wstring powerDetailsText() {
    PowerSample p; if (!current.history.empty()) p = current.history.back();
    const auto charge = calculateCharging(current.history);
    std::wstring text = tr(L"電源狀態：") + std::wstring(!p.acKnown ? tr(L"未知") : p.onAc ? tr(L"外部電源") : tr(L"電池供電"));
    text += tr(L"\nWindows 節能狀態：") + std::wstring(!p.saverKnown ? tr(L"未知") : p.saverOn ? tr(L"已啟用") : tr(L"未啟用"));
    text += tr(L"\n接頭類型：未提供（圓孔／USB-C 無法確認）");
    text += tr(L"\n\n電池端充電功率：") + valueText(charge.watts, L" W") +
        tr(L"\n近 2 分鐘充電平均：") + valueText(charge.averageWatts, L" W") +
        tr(L"\n剩餘容量：") + valueText(p.remainingWh, L" Wh") + L" / " + valueText(p.fullWh, L" Wh");
    const auto health = batteryHealth(p);
    text += tr(L"\n電池健康度：") + (health ? number(*health, 0) + tr(L"%（滿充 ") + valueText(p.fullWh, L" Wh") +
        tr(L" / 設計 ") + valueText(p.designWh, L" Wh") + tr(L"）") : std::wstring(tr(L"未提供")));
    text += tr(L"\n充電循環：") + (p.cycleCount ? std::to_wstring(*p.cycleCount) + tr(L" 次") : std::wstring(tr(L"未提供")));
    const auto eta = chargeEta(charge, p);
    text += L"\n" + etaLabel(eta) + tr(L"：") + valueText(eta.hours, tr(L" 小時"), 2);
    text += tr(L"\n偵測到的充電上限：") + (chargeLimit ? tr(L"約 ") + std::to_wstring(*chargeLimit) + tr(L"%（插電時電量長時間停在此處，可能是電池保護設定）")
        : std::wstring(tr(L"未偵測到")));
    int learned = 0;
    for (int band = 0; band < ChargeCurve::Bands; ++band) learned += chargeCurve.samples(band) >= 6;
    text += tr(L"\n已學習的充電曲線：") + std::to_wstring(learned * 5) + tr(L"% 的電量區間");
    text += tr(L"\n上次睡眠：") + (current.lastSleep ? sleepText(*current.lastSleep) : std::wstring(tr(L"尚未偵測到（BatteryScope 執行中且用電池時才會記錄）")));
    if (auto trend = healthTrendText(); !trend.empty()) text += tr(L"\n健康度趨勢：") + trend;
    text += tr(L"\n\n健康度是目前滿充容量占原廠設計容量的比例，會隨使用逐漸下降；新電池可能略高於 100%。");
    text += tr(L"\n\n充電功率是電池吸收的淨功率，不是插座功率、充電器額定功率或 USB PD 合約瓦數。");
    text += tr(L"\n充滿時間需至少 60 秒連續穩定充電資料。BatteryScope 會記錄這台電腦在各電量區間的實際充電速度；學到的區間涵蓋剩餘路程時，會依充電曲線預估接近滿電時的減速，否則依目前速度估算。偵測到充電上限時改預估到上限。這些都不是保證時間。");
    text += tr(L"\n\nWindows 通用電源資訊只回報是否接上外部電源，不能區分圓孔變壓器與 USB-C。偵測到 USB-C 控制器也不能證明目前由它供電。");
    return text;
}
// Battery details are part of the battery and history page.
void showPowerDetails() { showHistoryPage(); }
DWORD readSetting(const wchar_t* name, DWORD fallback) {
    DWORD value = 0, bytes = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\BatteryScope", name, RRF_RT_REG_DWORD, nullptr, &value, &bytes) == ERROR_SUCCESS
        ? value : fallback;
}
// Preview and test modes never persist preferences.
bool writeSetting(const wchar_t* name, DWORD value) {
    if (demo || isolatedPreview) return true;
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\BatteryScope", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    auto result = RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key); return result == ERROR_SUCCESS;
}
bool writeBinarySetting(const wchar_t* name, const std::vector<uint8_t>& bytes) {
    if (demo || isolatedPreview) return true;
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\BatteryScope", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    auto result = RegSetValueExW(key, name, 0, REG_BINARY, bytes.data(), DWORD(bytes.size()));
    RegCloseKey(key); return result == ERROR_SUCCESS;
}
std::set<std::wstring> readNames(const wchar_t* name) {
    std::set<std::wstring> names;
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\BatteryScope", name, RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes > 65536) return names;
    std::wstring buffer(bytes / sizeof(wchar_t) + 2, L'\0');
    bytes = DWORD(buffer.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\BatteryScope", name, RRF_RT_REG_MULTI_SZ, nullptr, buffer.data(), &bytes) != ERROR_SUCCESS) return names;
    for (const wchar_t* item = buffer.c_str(); *item; item += wcslen(item) + 1) names.insert(item);
    return names;
}
bool writeNames(const wchar_t* name, const std::set<std::wstring>& names) {
    if (demo || isolatedPreview) return true;
    std::wstring block;
    for (const auto& item : names) { block += item; block += L'\0'; }
    block += L'\0';
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\BatteryScope", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    auto result = RegSetValueExW(key, name, 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(block.data()), DWORD(block.size() * sizeof(wchar_t)));
    RegCloseKey(key); return result == ERROR_SUCCESS;
}
// Overrides held by the automatic rules are saved so a crash or forced exit can
// be undone at the next start.
void saveAutoOverrides() {
    std::vector<uint8_t> bytes(3 + 2 * (1 + sizeof(GUID)));
    bytes[0] = 1; bytes[1] = uint8_t(autoApplied); bytes[2] = 0;
    size_t at = 3;
    for (const AutoOverride* side : {&autoAc, &autoDc}) {
        bytes[at++] = side->active ? uint8_t(1 + int(side->mode)) : 0;
        std::memcpy(bytes.data() + at, &side->replaced, sizeof(GUID)); at += sizeof(GUID);
    }
    writeBinarySetting(L"AutoOverrides", bytes);
}
void undoAutoOverrides() {
    for (auto [side, pluggedIn] : {std::pair<AutoOverride*, bool>{&autoAc, true}, {&autoDc, false}}) {
        if (side->active) undoPowerModeOverride(pluggedIn, side->mode, side->replaced);
        *side = {};
    }
    autoApplied = AutoMode::none;
}
void loadAutoOverrides() {
    std::vector<uint8_t> bytes(64); DWORD size = DWORD(bytes.size());
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\BatteryScope", L"AutoOverrides", RRF_RT_REG_BINARY, nullptr, bytes.data(), &size) != ERROR_SUCCESS
        || size != 3 + 2 * (1 + sizeof(GUID)) || bytes[0] != 1) return;
    size_t at = 3;
    for (AutoOverride* side : {&autoAc, &autoDc}) {
        side->active = bytes[at] >= 1 && bytes[at] <= 3;
        if (side->active) side->mode = PowerMode(bytes[at] - 1);
        std::memcpy(&side->replaced, bytes.data() + at + 1, sizeof(GUID)); at += 1 + sizeof(GUID);
    }
    undoAutoOverrides(); // a previous run ended while holding overrides
    saveAutoOverrides();
}
// Re-evaluated on every sample: listed apps (with a 30 s grace after they exit)
// set both modes to Performance; low battery sets the on-battery mode to
// Efficiency. Each change is undone only if the user has not changed it since.
void applyAutoPowerRules() {
    if (demo || isolatedPreview || current.history.empty()) return;
    // One owner at a time: with dynamic saving on, it covers the performance list (as app rules)
    // and low battery (as extra saving); while the lab measures, the lab owns the settings.
    if (dynamicEnabled || (labController && labController->status().running)) {
        if (autoApplied != AutoMode::none) { undoAutoOverrides(); autoApplied = AutoMode::none; saveAutoOverrides(); }
        return;
    }
    const uint64_t now = GetTickCount64();
    for (const auto& row : current.processes) if (performanceListed(row)) { performanceSeenAt = now; break; }
    const bool performanceActive = performanceSeenAt && now - performanceSeenAt < 30000;
    const AutoMode desired = decideAutoMode(autoApplied, performanceActive, current.history.back(), autoSaverThreshold);
    if (desired == autoApplied) return;
    undoAutoOverrides();
    if (desired == AutoMode::saver) {
        autoDc.mode = PowerMode::efficiency; autoDc.active = overridePowerMode(false, PowerMode::efficiency, autoDc.replaced) == ERROR_SUCCESS;
    } else if (desired == AutoMode::performance) {
        autoAc.mode = autoDc.mode = PowerMode::performance;
        autoAc.active = overridePowerMode(true, PowerMode::performance, autoAc.replaced) == ERROR_SUCCESS;
        autoDc.active = overridePowerMode(false, PowerMode::performance, autoDc.replaced) == ERROR_SUCCESS;
    }
    autoApplied = autoAc.active || autoDc.active ? desired : AutoMode::none;
    saveAutoOverrides();
    setNotice(autoApplied == AutoMode::saver ? tr(L"電量偏低：已自動把「用電池時」切到效率。") :
        autoApplied == AutoMode::performance ? tr(L"效能清單中的程式正在執行：已切到效能模式。") : tr(L"已還原自動切換前的電源模式。"));
    if (inlinePage == InlinePage::powerMode) refreshPowerModeControls();
}
// Today's summary as an image: the mascot, time on battery, energy, grade,
// top app, and the 7-day discharge bars, in the current theme and style.
HBITMAP renderShareCard(int& width, int& height) {
    width = px(600); height = px(320);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(info.bmiHeader); info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap) { DeleteDC(dc); return nullptr; }
    HGDIOBJ old = SelectObject(dc, bitmap);
    fill(dc, {0, 0, width, height}, Background);
    const RECT frame{px(16), px(16), width - px(16), height - px(16)};
    card(dc, frame, Surface, 16);
    const int left = frame.left + px(24), right = frame.right - px(24);
    const auto mood = currentMood();
    const double level = mascotLevel();
    ui::drawMascot(dc, left, frame.top + px(22), px(5), mood, level, 0, mascotColors(mood));
    const int textLeft = left + ui::MascotCells * px(5) + px(20);
    const int today = localDate();
    const DayStats* day = nullptr;
    for (const auto& d : current.days) if (d.date == today) day = &d;
    drawText(dc, L"BatteryScope · " + dayLabel(today), {textLeft, frame.top + px(20), right, frame.top + px(48)}, Muted, strongFont);
    const std::wstring battery = day && day->batteryMinutes >= 1 ? durationText(day->batteryMinutes / 60).substr(std::wstring(tr(L"約 ")).size()) : L"0" + std::wstring(tr(L" 分"));
    drawText(dc, tr(L"今天用電池 ") + battery, {textLeft, frame.top + px(48), right, frame.top + px(84)}, Ink, titleFont);
    drawText(dc, tr(L"放電 ") + number(day ? day->dischargedWh : 0, 1) + L" Wh · " + tr(L"充電 ") + number(day ? day->chargedWh : 0, 1) + L" Wh",
        {textLeft, frame.top + px(86), right, frame.top + px(108)}, Muted, normalFont);
    std::wstring facts;
    if (auto grade = gradeDay(current.days, today)) facts += tr(L"評分 ") + std::wstring(1, grade->letter) + L"   ";
    if (day && !day->apps.empty()) facts += tr(L"最耗電：") + day->apps.front().first;
    drawText(dc, facts, {textLeft, frame.top + px(110), right, frame.top + px(132)}, Green, strongFont);
    // 7-day bars along the bottom.
    const RECT plot{left, frame.top + px(160), right, frame.bottom - px(46)};
    double peak = 1; int dates[7];
    for (int i = 0; i < 7; ++i) { dates[i] = localDate(6 - i); for (const auto& d : current.days) if (d.date == dates[i]) peak = std::max(peak, d.dischargedWh); }
    const int gap = px(10), slot = (int(plot.right - plot.left) - 6 * gap) / 7;
    for (int i = 0; i < 7; ++i) {
        double wh = 0; for (const auto& d : current.days) if (d.date == dates[i]) wh = d.dischargedWh;
        const int x = plot.left + i * (slot + gap), barHeight = wh > 0 ? std::max(px(4), int((plot.bottom - plot.top) * wh / peak)) : 0;
        fill(dc, {x, plot.bottom, x + slot, plot.bottom + px(2)}, Border);
        if (barHeight) rounded(dc, {x, plot.bottom - barHeight, x + slot, plot.bottom}, i == 6 ? Green : blend(Green, Surface, .45), 3);
        drawText(dc, dayLabel(dates[i]), {x - px(8), plot.bottom + px(6), x + slot + px(8), plot.bottom + px(26)}, i == 6 ? Ink : Muted, smallFont,
            DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
    std::wstring footer = tr(L"電量 ") + (current.history.empty() ? std::wstring(L"—") : valueText(current.history.back().percent, L"%", 0));
    if (!current.history.empty()) if (auto health = batteryHealth(current.history.back())) footer += tr(L" · 健康度 ") + number(*health, 0) + L"%";
    drawText(dc, footer, {left, frame.bottom - px(18) - px(4), right, frame.bottom - px(4)}, Muted, smallFont, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    GdiFlush();
    SelectObject(dc, old); DeleteDC(dc);
    return bitmap;
}
bool copyBitmapToClipboard(HBITMAP bitmap, int width, int height) {
    BITMAPINFOHEADER header{}; header.biSize = sizeof(header); header.biWidth = width; header.biHeight = -height;
    header.biPlanes = 1; header.biBitCount = 32; header.biCompression = BI_RGB;
    const size_t pixels = size_t(width) * size_t(height) * 4;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, sizeof(header) + pixels);
    if (!memory) return false;
    auto* target = static_cast<BYTE*>(GlobalLock(memory));
    std::memcpy(target, &header, sizeof(header));
    BITMAPINFO info{}; info.bmiHeader = header;
    HDC screen = GetDC(nullptr);
    const bool read = GetDIBits(screen, bitmap, 0, UINT(height), target + sizeof(header), &info, DIB_RGB_COLORS) == height;
    ReleaseDC(nullptr, screen);
    GlobalUnlock(memory);
    // Clipboard DIBs are bottom-up by convention; a negative height is also accepted by common apps.
    if (!read || !OpenClipboard(mainWindow)) { GlobalFree(memory); return false; }
    EmptyClipboard();
    const bool placed = SetClipboardData(CF_DIB, memory) != nullptr;
    CloseClipboard();
    if (!placed) GlobalFree(memory);
    return placed;
}
bool exportShareCard(const std::wstring& path, bool clipboard) {
    int width = 0, height = 0;
    HBITMAP bitmap = renderShareCard(width, height);
    if (!bitmap) return false;
    const bool saved = path.empty() || ui::saveBitmapPng(bitmap, path);
    if (clipboard) copyBitmapToClipboard(bitmap, width, height);
    DeleteObject(bitmap);
    return saved;
}
void chooseShareCard() {
    wchar_t file[MAX_PATH]{};
    swprintf_s(file, L"BatteryScope-%d.png", localDate());
    const std::wstring filter = std::wstring(tr(L"PNG 圖片")) + L" (*.png)" + L'\0' + L"*.png" + L'\0' + L'\0';
    OPENFILENAMEW dialog{}; dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = mainWindow;
    dialog.lpstrFilter = filter.c_str(); dialog.lpstrFile = file; dialog.nMaxFile = MAX_PATH; dialog.lpstrDefExt = L"png";
    dialog.lpstrTitle = tr(L"分享今日卡片");
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetSaveFileNameW(&dialog)) { releaseInteraction(); return; }
    setNotice(exportShareCard(file, true) ? tr(L"已存成圖片，也複製到剪貼簿了。") : tr(L"無法建立圖片。"));
    releaseInteraction();
}
// The mini window: battery level, watts, and the mascot, always on top.
void paintMini(HDC dc, RECT r) {
    fill(dc, r, Background);
    card(dc, r, Surface, 10);
    const int unit = std::max(2, px(2));
    const auto mood = currentMood();
    const PowerSample* p = current.history.empty() ? nullptr : &current.history.back();
    ui::drawMascot(dc, r.left + px(8), r.top + (r.bottom - r.top - ui::MascotRows * unit) / 2, unit, mood,
        mascotLevel(), sampleTick % 5 == 4 ? 1 : 0, mascotColors(mood));
    const int textLeft = r.left + px(8) + ui::MascotCells * unit + px(6);
    const std::wstring percent = p ? valueText(p->percent, L"%", 0) : L"—";
    const std::wstring watts = !p ? L"—" : p->charging ? valueText(calculateCharging(current.history).watts, L" W") :
        p->acKnown && p->onAc ? L"—" : valueText(current.power.currentWatts, L" W");
    if (!ui::drawPixelDigits(dc, percent, textLeft, r.top + px(9), unit, Ink)) drawText(dc, percent, {textLeft, r.top + px(5), r.right, r.top + px(25)}, Ink, strongFont);
    if (!ui::drawPixelDigits(dc, watts, textLeft, r.top + px(30), unit, p && p->charging ? Green : Muted))
        drawText(dc, watts, {textLeft, r.top + px(28), r.right, r.bottom - px(4)}, Muted, smallFont);
}
LRESULT CALLBACK miniProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_NCHITTEST: { LRESULT hit = DefWindowProcW(window, message, wp, lp); return hit == HTCLIENT ? HTCAPTION : hit; }
    case WM_NCLBUTTONDBLCLK: showMain(); return 0;
    case WM_NCRBUTTONUP: case WM_CONTEXTMENU: {
        POINT point{}; GetCursorPos(&point);
        const int chosen = showMenu({menuItem(MiniOpenId, tr(L"開啟面板")), menuItem(MiniToggleId, tr(L"隱藏迷你視窗"))}, point, window);
        if (chosen == MiniOpenId) showMain(); else if (chosen == MiniToggleId) PostMessageW(mainWindow, WM_COMMAND, MiniToggleId, 0);
        return 0;
    }
    case WM_EXITSIZEMOVE: {
        RECT r{}; GetWindowRect(window, &r);
        writeSetting(L"MiniX", DWORD(r.left)); writeSetting(L"MiniY", DWORD(r.top));
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{}; HDC target = BeginPaint(window, &ps); RECT r{}; GetClientRect(window, &r);
        HDC dc = CreateCompatibleDC(target); HBITMAP bitmap = CreateCompatibleBitmap(target, r.right, r.bottom);
        HGDIOBJ old = SelectObject(dc, bitmap); paintMini(dc, r); BitBlt(target, 0, 0, r.right, r.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc); EndPaint(window, &ps); return 0;
    }
    }
    return DefWindowProcW(window, message, wp, lp);
}
void setMiniWindow(bool enable) {
    if (!enable) { if (miniWindow) DestroyWindow(miniWindow); miniWindow = nullptr; return; }
    if (miniWindow) return;
    const int width = px(150), height = px(54);
    RECT work{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int x = int(readSetting(L"MiniX", DWORD(work.right - width - px(24)))), y = int(readSetting(L"MiniY", DWORD(work.bottom - height - px(24))));
    // Keep it on a visible monitor if the saved position is off-screen now.
    POINT corner{x + width / 2, y + height / 2};
    if (!MonitorFromPoint(corner, MONITOR_DEFAULTTONULL)) { x = work.right - width - px(24); y = work.bottom - height - px(24); }
    miniWindow = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE, MiniClassName, L"BatteryScope Mini",
        WS_POPUP, x, y, width, height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!miniWindow) return;
    SetLayeredWindowAttributes(miniWindow, 0, 240, LWA_ALPHA);
    ShowWindow(miniWindow, SW_SHOWNOACTIVATE);
}
void showPerformanceApps() {
    std::wstring text = tr(L"清單中的程式執行時，插電與用電池時都會自動切到效能模式；全部結束 30 秒後還原。在程序詳情頁按「加入效能清單」新增或移除。\n");
    if (performanceApps.empty()) text += tr(L"\n清單是空的。");
    for (const auto& name : performanceApps) text += L"\n• " + name;
    text += tr(L"\n\n低電量自動省電：") + (autoSaverThreshold ? std::to_wstring(autoSaverThreshold) + tr(L"% 以下把「用電池時」切到效率，接上電源或回升 10% 後還原。")
        : std::wstring(tr(L"關閉")));
    showInlinePage(InlinePage::perfApps, tr(L"自動電源模式"), text);
    releaseInteraction();
}
// Learns from each new battery sample: charging rate per band, and a charge limit
// when the level holds steady below full while plugged in.
void learnFromSample() {
    if (demo || isolatedPreview || current.history.empty()) return;
    const auto& p = current.history.back();
    if (p.ms <= curveLastMs) return;
    curveLastMs = p.ms;
    if (p.present && p.acKnown && p.onAc && p.charging && p.percent && p.watts && std::isfinite(*p.watts) && *p.watts < -.5) {
        chargeCurve.add(*p.percent, -*p.watts); curveDirty = true;
        if (chargeLimit && *p.percent > *chargeLimit + 2) { chargeLimit.reset(); writeSetting(L"ChargeLimit", 0); }
    }
    if (auto limit = detectChargeLimit(current.history); limit && limit != chargeLimit) { chargeLimit = limit; writeSetting(L"ChargeLimit", DWORD(*limit)); }
    if (curveDirty && p.ms - curveSavedAt >= 300000) { writeBinarySetting(L"ChargeCurve", chargeCurve.save()); curveSavedAt = p.ms; curveDirty = false; }
}
void loadSettings() {
    english = readSetting(L"Language", 0) == 1;
    const DWORD storedTheme = readSetting(L"Theme", 0);
    themeChoice = storedTheme <= 2 ? ui::ThemeChoice(storedTheme) : ui::ThemeChoice::system;
    const DWORD threshold = readSetting(L"LowBattery", 0);
    lowBatteryThreshold = threshold == 10 || threshold == 20 || threshold == 30 ? threshold : 0;
    hotkeyEnabled = readSetting(L"Hotkey", 0) == 1;
    pixelStyle = readSetting(L"Style", 1) != 0;
    trayPercent = readSetting(L"TrayPercent", 1) != 0;
    drainAlerts = readSetting(L"DrainAlert", 0) == 1;
    sleepAlerts = readSetting(L"SleepAlert", 1) != 0;
    const DWORD reminder = readSetting(L"ChargeReminder", 0);
    chargeReminderTarget = reminder == 80 || reminder == 85 || reminder == 90 || reminder == 100 ? reminder : 0;
    const DWORD saver = readSetting(L"AutoSaver", 0);
    autoSaverThreshold = saver == 20 || saver == 30 ? saver : 0;
    performanceApps = readNames(L"PerformanceApps");
    showMascot = readSetting(L"Mascot", 1) != 0;
    miniEnabled = readSetting(L"MiniWindow", 0) == 1;
    historyEnabled = readSetting(L"History", 1) != 0;
    updateChecks = readSetting(L"UpdateCheck", 1) != 0;
    updateAuto = readSetting(L"UpdateAuto", 1) != 0;
    gpuAuto = readSetting(L"GpuAuto", 0) != 0;
    keyboardAutoOff = readSetting(L"KeyboardAutoOff", 0) != 0;
    asusKeysHandled = readSetting(L"AsusKeysOff", 0) == 0;
    rogKeyAction = std::min<DWORD>(2, readSetting(L"RogKey", 0));
    fnCAction = std::min<DWORD>(1, readSetting(L"FnCKey", 0));
    dynamicEnabled = readSetting(L"Dynamic", 0) == 1;
    dynamicRefresh = readSetting(L"DynamicRefresh", 0) == 1;
    dynamicEco = readSetting(L"DynamicEco", 1) != 0;
    refreshAuto = readSetting(L"RefreshAuto", 1) != 0;
    dynamicTarget = readSetting(L"DynamicTarget", 0);
    goalUse = readSetting(L"GoalUse", 10); goalStill = readSetting(L"GoalStill", 7);
    if (dynamicTarget != 3 && dynamicTarget != 5 && dynamicTarget != 8) dynamicTarget = 0;
    std::vector<uint8_t> curve(512); DWORD curveBytes = DWORD(curve.size());
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\BatteryScope", L"ChargeCurve", RRF_RT_REG_BINARY, nullptr, curve.data(), &curveBytes) == ERROR_SUCCESS) {
        curve.resize(curveBytes); chargeCurve.load(curve);
    }
    const DWORD limit = readSetting(L"ChargeLimit", 0);
    if (limit >= 40 && limit <= 96) chargeLimit = int(limit);
}
std::wstring ledgerText() {
    std::wstring content = tr(L"自 BatteryScope 啟動以來，依 CPU 能源分攤累計各程式的耗能，含已結束的程序。");
    if (current.ledgerStartedMs && current.ms > current.ledgerStartedMs) {
        if (auto watched = durationText((current.ms - current.ledgerStartedMs) / 3600000.0); !watched.empty())
            content += tr(L"\n已監測：") + watched;
    }
    content += tr(L"\nCPU 核心總計：") + number(current.ledgerCoreMilliWh, 1) + L" mWh\n";
    if (current.ledger.empty()) content += tr(L"\n尚無資料：需要可用的 CPU 能源計數器與一段觀測時間。\n");
    for (size_t i = 0; i < current.ledger.size(); ++i) {
        const auto& entry = current.ledger[i];
        content += L"\n" + std::to_wstring(i + 1) + L". " + entry.name + L" · " + number(entry.milliWh, 1) + L" mWh";
        if (current.ledgerCoreMilliWh > 0) content += L" · " + number(entry.milliWh / current.ledgerCoreMilliWh * 100, 0) + L"%";
        if (entry.processes > 1) content += L" · " + std::to_wstring(entry.processes) + tr(L" 個程序");
        if (!entry.running) content += tr(L" · 已結束");
    }
    content += tr(L"\n\n百分比是占 CPU 核心總耗能的比例。這是 CPU 能源分攤估計，不含 GPU、螢幕與儲存；關閉程式不保證省下相同能量。");
    return content;
}
const wchar_t* kindToken(const std::wstring& kind) {
    return kind == L"Windows 核心" ? L"kernel" : kind == L"服務" ? L"service" : kind == L"應用程式" ? L"app" : L"background";
}
std::wstring csvNumber(Value value, int precision) { return value && std::isfinite(*value) ? number(*value, precision) : L""; }
bool writeUtf8(const std::wstring& path, const std::wstring& text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string bytes = "\xEF\xBB\xBF"; // BOM so spreadsheet apps detect UTF-8
    bytes.resize(3 + size_t(std::max(0, size)));
    if (size > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), bytes.data() + 3, size, nullptr, nullptr);
    std::ofstream out(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), std::streamsize(bytes.size()));
    return out.good();
}
// Writes the current process ranking to `path` and the 30-minute battery
// history next to it as "<name>-power.csv". Returns the power file path.
std::optional<std::wstring> exportCsv(const std::wstring& path) {
    std::wstring processes = L"rank,name,pid,kind,cpu_percent,avg_cpu_percent,cpu_watts,avg_cpu_watts,cpu_watts_low,cpu_watts_high,"
        L"session_cpu_mwh,memory_mb,io_mb_per_s,gpu_percent,services,path\r\n";
    for (size_t i = 0; i < current.processes.size(); ++i) {
        const auto& row = current.processes[i];
        std::wstring services;
        for (const auto& service : row.services) services += (services.empty() ? L"" : L"; ") + service.name;
        processes += std::to_wstring(i + 1) + L"," + csvField(row.name) + L"," + std::to_wstring(row.pid) + L"," + kindToken(row.kind) + L"," +
            (row.measured ? number(row.cpu, 2) : L"") + L"," + (row.measured ? number(row.averageCpu, 2) : L"") + L"," +
            csvNumber(row.cpuWatts, 3) + L"," + csvNumber(row.averageCpuWatts, 3) + L"," + csvNumber(row.cpuWattsLow, 3) + L"," +
            csvNumber(row.cpuWattsHigh, 3) + L"," + csvNumber(row.cpuMilliWh, 2) + L"," +
            (row.memoryKnown ? number(row.memoryMB, 1) : L"") + L"," + (row.ioKnown && row.measured ? number(row.ioMB, 3) : L"") + L"," +
            csvNumber(row.gpuPercent, 1) + L"," +
            csvField(services) + L"," + csvField(row.path) + L"\r\n";
    }
    std::wstring power = L"seconds_ago,percent,battery_watts_positive_discharge,remaining_wh,full_wh,on_ac,charging\r\n";
    const uint64_t last = current.history.empty() ? 0 : current.history.back().ms;
    for (const auto& sample : current.history) {
        power += number((last - sample.ms) / 1000.0, 0) + L"," + csvNumber(sample.percent, 1) + L"," + csvNumber(sample.watts, 3) + L"," +
            csvNumber(sample.remainingWh, 3) + L"," + csvNumber(sample.fullWh, 3) + L"," +
            (sample.acKnown ? std::to_wstring(int(sample.onAc)) : L"") + L"," + std::to_wstring(int(sample.charging)) + L"\r\n";
    }
    std::filesystem::path powerPath(path);
    powerPath.replace_filename(powerPath.stem().wstring() + L"-power" + powerPath.extension().wstring());
    if (!writeUtf8(path, processes) || !writeUtf8(powerPath.wstring(), power)) return std::nullopt;
    return powerPath.wstring();
}
void chooseCsvExport() {
    SYSTEMTIME now{}; GetLocalTime(&now);
    wchar_t file[MAX_PATH]{};
    swprintf_s(file, L"BatteryScope-%04u%02u%02u-%02u%02u.csv", unsigned(now.wYear), unsigned(now.wMonth), unsigned(now.wDay),
        unsigned(now.wHour), unsigned(now.wMinute));
    const std::wstring filter = std::wstring(tr(L"CSV 檔案")) + L" (*.csv)" + L'\0' + L"*.csv" + L'\0' + L'\0';
    OPENFILENAMEW dialog{}; dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = mainWindow;
    dialog.lpstrFilter = filter.c_str(); dialog.lpstrFile = file; dialog.nMaxFile = MAX_PATH; dialog.lpstrDefExt = L"csv";
    dialog.lpstrTitle = tr(L"匯出 CSV");
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetSaveFileNameW(&dialog)) { releaseInteraction(); return; }
    auto powerFile = exportCsv(file);
    setNotice(powerFile ? tr(L"已匯出程序排行與電量紀錄（-power.csv）。") : tr(L"無法寫入 CSV 檔案。"));
    releaseInteraction();
}
void setLowBatteryThreshold(unsigned threshold) {
    lowBatteryThreshold = threshold; lowBattery.reset();
    if (!writeSetting(L"LowBattery", threshold)) setNotice(tr(L"無法儲存設定。"));
    else setNotice(threshold ? tr(L"電量降到 ") + std::to_wstring(threshold) + tr(L"% 時會通知。") : tr(L"已關閉低電量提醒。"));
}
// Ctrl+Alt+B shows or hides the panel from anywhere when enabled.
bool registerPanelHotkey(bool enable) {
    if (hotkeyRegistered) { UnregisterHotKey(mainWindow, PanelHotkey); hotkeyRegistered = false; }
    if (enable) hotkeyRegistered = RegisterHotKey(mainWindow, PanelHotkey, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'B') != FALSE;
    return !enable || hotkeyRegistered;
}
void setStyle(bool pixel) {
    pixelStyle = pixel;
    makeFonts();
    RedrawWindow(mainWindow, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    if (!writeSetting(L"Style", pixel ? 1 : 0)) setNotice(tr(L"無法儲存設定。"));
}
void setTheme(ui::ThemeChoice choice) {
    themeChoice = choice;
    applyTheme();
    if (!writeSetting(L"Theme", DWORD(choice))) setNotice(tr(L"無法儲存設定。"));
}
void refreshPowerModeControls() {
    const bool editable = !demo && !isolatedPreview;
    if (editable) powerModes = queryPowerModes();
    else { // Synthetic preview: plugged in at Best performance, on battery at Best efficiency.
        powerModes = {}; powerModes.ac = PowerMode::performance; powerModes.dc = PowerMode::efficiency;
        powerModes.acKnown = powerModes.dcKnown = powerModes.acSettable = powerModes.dcSettable = true;
        refreshPowerButton();
    }
    const bool noBattery = batteryAbsent();
    for (int i = 0; i < 6; ++i) {
        const bool settable = i < 3 ? powerModes.acKnown && powerModes.acSettable : powerModes.dcKnown && powerModes.dcSettable && !noBattery;
        EnableWindow(modeButtons[i], editable && settable);
        InvalidateRect(modeButtons[i], nullptr, FALSE);
    }
    // With dynamic saving on, the second button hands a battery mode chosen here back to it.
    const bool handBack = powerModeHeld();
    SetWindowTextW(restoreModesButton, handBack ? tr(L"交給動態省電") : tr(L"還原原本設定"));
    EnableWindow(restoreModesButton, editable && (handBack || (powerModes.restorable && !dynamicEnabled)));
    EnableWindow(energySettingsButton, editable);
    InvalidateRect(mainWindow, nullptr, FALSE);
}
std::wstring powerModeText() {
    std::wstring text = tr(L"「插電時」與「用電池時」是 Windows 的兩個獨立設定，各自只在對應的供電狀態生效。");
    text += tr(L"\n效率：較省電、較安靜，效能較低。\n平衡：Windows 預設。\n效能：反應最快，較耗電、較熱。");
    if (!current.history.empty() && current.history.back().saverKnown)
        text += tr(L"\n\nWindows 節能工具：") + std::wstring(current.history.back().saverOn ? tr(L"已啟用") : tr(L"未啟用")) +
            tr(L"。節能工具會再限制背景活動，請在「Windows 節能設定」調整。");
    if (powerModes.error == ERROR_PROC_NOT_FOUND) text += tr(L"\n\n此 Windows 版本未提供電源模式設定，只能檢視。");
    else if (batteryAbsent()) text += tr(L"\n\n這台電腦沒有電池，只有「插電時」的設定會生效。");
    else if (powerModes.legacy) text += tr(L"\n\nWindows 10 只能變更目前供電來源的設定（另一項為灰色）；接上或拔除電源後即可變更另一項。");
    else if (!powerModes.acKnown || !powerModes.dcKnown) text += tr(L"\n\n灰色的選項表示此電腦未提供該設定。");
    text += tr(L"\n\nBatteryScope 第一次變更某一項時會記住原本的設定，可用「還原原本設定」恢復；不會結束任何程式。");
    return text;
}
// "89% → 87% over 30 days" from the oldest and newest days with a health reading.
std::wstring healthTrendText() {
    const DayStats* first = nullptr; const DayStats* last = nullptr;
    for (const auto& day : current.days) if (day.healthPercent) { if (!first) first = &day; last = &day; }
    if (!first || first == last) return {};
    return number(*first->healthPercent, 0) + L"% → " + number(*last->healthPercent, 0) + L"%" + tr(L"（") + dayLabel(first->date) +
        L" – " + dayLabel(last->date) + tr(L"）");
}
std::wstring historyText() {
    std::wstring text;
    if (current.days.empty()) text = tr(L"還沒有紀錄。BatteryScope 執行時會每天累計用電池的時間、放電與充電量，以及最耗電的程式。");
    const int today = localDate();
    if (!current.days.empty()) {
        if (auto grade = gradeDay(current.days, today)) {
            const int change = int(std::lround((grade->ratio - 1) * 100));
            text += tr(L"今日評分：") + std::wstring(1, grade->letter) + tr(L"（耗電比平常") + (change <= 0 ? tr(L"少 ") : tr(L"多 ")) +
                std::to_wstring(std::abs(change)) + L"%" + tr(L"）");
        } else text += tr(L"今日評分：資料不足（需要今天用電池 30 分鐘以上，以及 3 天以上的紀錄）");
        static const wchar_t* names[AchievementCount] = {L"初次紀錄", L"長跑者", L"省電達人", L"好眠", L"電池保養", L"一週紀錄"};
        const auto earned = achievements(current.days, chargeLimit.has_value() || chargeReminderTarget);
        text += tr(L"\n成就：");
        for (size_t i = 0; i < AchievementCount; ++i) text += (i ? L"  " : L"") + std::wstring(earned[i] ? L"★" : L"☆") + tr(names[i]);
        text += tr(L"\n長跑者＝單日用電池 4 小時；省電達人＝連續 3 天 S 或 A；好眠＝睡眠 4 小時以上且每小時掉電不到 0.5%；電池保養＝偵測到充電上限或開啟充電提醒；一週紀錄＝累積 7 天。");
        if (auto trend = healthTrendText(); !trend.empty()) text += tr(L"\n電池健康度：") + trend;
    }
    size_t listed = 0;
    for (auto it = current.days.rbegin(); it != current.days.rend() && listed < 7; ++it, ++listed) {
        const auto& day = *it;
        text += (text.empty() ? L"" : L"\n\n") + dayLabel(day.date) + (day.date == today ? tr(L"（今天）") : L"") +
            tr(L" · 用電池 ") + (day.batteryMinutes >= 1 ? durationText(day.batteryMinutes / 60).substr(std::wstring(tr(L"約 ")).size()) : L"0" + std::wstring(tr(L" 分"))) +
            tr(L" · 放電 ") + number(day.dischargedWh, 1) + L" Wh" + tr(L" · 充電 ") + number(day.chargedWh, 1) + L" Wh";
        if (day.sleepMinutes >= 10)
            text += tr(L"\n  睡眠 ") + durationText(day.sleepMinutes / 60).substr(std::wstring(tr(L"約 ")).size()) + tr(L"，掉 ") + number(day.sleepDropPercent, 0) + L"%";
        if (day.healthPercent) text += tr(L"\n  健康度 ") + number(*day.healthPercent, 0) + L"%" +
            (day.cycles ? L" · " + std::to_wstring(*day.cycles) + tr(L" 次循環") : L"");
        if (!day.apps.empty()) {
            text += tr(L"\n  CPU 最耗電：");
            for (size_t i = 0; i < day.apps.size() && i < 3; ++i)
                text += (i ? tr(L"、") : L"") + day.apps[i].first + L" " + number(day.apps[i].second, 0) + L" mWh";
        }
    }
    text += tr(L"\n\n紀錄只存在這台電腦（%LOCALAPPDATA%\\BatteryScope\\history.csv），保留 60 天；可在選單關閉或清除。BatteryScope 沒有執行的時間不會被記錄。");
    return text;
}
// Battery now, its health, and the last seven days as bars.
void showHistoryPage() {
    std::vector<VBlock> v;
    PowerSample p; if (!current.history.empty()) p = current.history.back();
    {
        std::vector<std::pair<std::wstring, COLORREF>> chips;
        if (p.percent) chips.push_back({tr(L"電量 ") + number(*p.percent, 0) + L"%", Green});
        chips.push_back({!p.acKnown ? tr(L"電源未知") : p.onAc ? (p.charging ? tr(L"充電中") : tr(L"插電")) : tr(L"用電池"), 0});
        if (const auto health = batteryHealth(p)) chips.push_back({tr(L"健康度 ") + number(*health, 0) + L"%", *health >= 80 ? Green : theme.warn});
        if (p.cycleCount) chips.push_back({std::to_wstring(*p.cycleCount) + tr(L" 次循環"), 0});
        if (chargeLimit) chips.push_back({tr(L"充電上限 ") + std::to_wstring(*chargeLimit) + L"%", 0});
        v.push_back(vChips(std::move(chips)));
    }
    if (p.remainingWh && p.fullWh && *p.fullWh > 0)
        v.push_back(vMeter(tr(L"剩餘容量"), number(*p.remainingWh, 1) + L" / " + number(*p.fullWh, 1) + L" Wh", *p.remainingWh, 0, *p.fullWh));
    if (asusAvailable && asusNow().chargeDevice) {
        const auto limit = asusNow().chargeLimit;
        v.push_back(vAction(tr(L"充電上限"), limit ? std::to_wstring(*limit) + L"%" : std::wstring(tr(L"未設定")), HwChargeMenuId, limit && *limit <= 80 ? Green : COLORREF(0)));
    }
    v.push_back(vHeading(tr(L"近 7 天")));
    {
        std::vector<VBar> bars;
        for (int i = 0; i < 7; ++i) {
            const int date = localDate(i);
            const DayStats* day = nullptr;
            for (const auto& d : current.days) if (d.date == date) day = &d;
            std::wstring label = dayLabel(date) + (i == 0 ? tr(L"（今天）") : L"");
            std::wstring value = tr(L"沒有紀錄");
            double wh = 0;
            if (day) {
                wh = day->dischargedWh;
                value = number(wh, wh < 10 ? 1 : 0) + L" Wh";
                if (day->batteryMinutes >= 1) {
                    label += tr(L" · 用電池 ") + durationText(day->batteryMinutes / 60).substr(std::wstring(tr(L"約 ")).size());
                    value += L" · " + number(wh / (day->batteryMinutes / 60), 1) + L" W";
                }
            }
            bars.push_back({label, value, wh, 0, i == 0 ? Green : blend(Green, Surface, .4), {}});
        }
        v.push_back(vBars(std::move(bars)));
        v.push_back(vNote(tr(L"每天用電池時的放電量，右邊是平均耗電。紀錄只存在這台電腦，保留 60 天。")));
    }
    for (auto& block : ledgerBlocks()) v.push_back(std::move(block)); // which programs used it
    v.push_back(vAction(tr(L"匯出 CSV"), tr(L"程序排行與 30 分鐘電量"), ExportCsvId));
    v.push_back(vAction(tr(L"分享今日卡片"), tr(L"圖片"), ShareCardId));
    v.push_back(vHeading(tr(L"詳細")));
    v.push_back(vNote(powerDetailsText()));
    showChartPage(InlinePage::history, tr(L"電池"), historyText() + L"\n\n" + ledgerText() + L"\n\n" + candidatesText() + L"\n\n" + powerDetailsText(), std::move(v));
    releaseInteraction();
}
// The power lab page: where battery power goes per activity, the running
// experiment, and every experiment's paired result so far.
std::wstring labExperimentTitle(const std::wstring& key);
const wchar_t* labKnobTitle(const std::wstring& key) {
    if (key == L"cpu-boost") return tr(L"CPU Boost");
    if (key == L"epp") return tr(L"處理器能源偏好");
    if (key == L"refresh") return tr(L"螢幕更新率");
    if (key == L"power-mode") return tr(L"電源模式");
    if (key == L"brightness") return tr(L"螢幕亮度");
    if (key == L"display") return tr(L"螢幕開關");
    if (key == L"background-eco") return tr(L"背景程式效率模式");
    if (key == L"dark-mode") return tr(L"深色模式");
    return L"";
}
// "cpu-boost@scroll" -> "CPU Boost · 捲動頁面".
std::wstring labExperimentTitle(const std::wstring& key) {
    const size_t at = key.find(L'@');
    std::wstring title = labKnobTitle(key.substr(0, at));
    if (at != std::wstring::npos)
        for (int i = 1; i < int(bs::lab::Workload::count); ++i)
            if (key.substr(at + 1) == bs::lab::workloadKey(bs::lab::Workload(i))) title += L" · " + std::wstring(tr(bs::lab::workloadName(bs::lab::Workload(i))));
    return title;
}
std::wstring signedNumber(double value, int precision) {
    return (value >= 0 ? L"+" : L"−") + number(std::abs(value), precision);
}
// Arm names come from the lab in Chinese; numbers in them stay as they are.
std::wstring labArm(const std::wstring& name) {
    const std::wstring prefix = L"能源偏好 ";
    if (name.rfind(prefix, 0) == 0) return tr(L"能源偏好 ") + name.substr(prefix.size());
    const std::wstring brightness = L"亮度 ";
    if (name.rfind(brightness, 0) == 0) return tr(L"亮度 ") + name.substr(brightness.size());
    return tr(name.c_str());
}
uint64_t labPageShownAt = 0;
std::wstring labPageText() {
    std::wstring c = tr(L"在你這台電腦上實際量測：耗電花在哪裡，以及每個設定能省多少電、會不會變卡。資料只存在本機。");
    if (!labController) return c + tr(L"\n\n示範與預覽模式不提供實驗室。");
    const auto st = labController->status();
    c += tr(L"\n\n記錄：") + std::wstring(st.recording ? tr(L"開啟") : tr(L"關閉"));
    if (st.recordedSeconds >= 60) c += tr(L"（已記錄用電池 ") + durationText(st.recordedSeconds / 3600) + tr(L"）");
    c += tr(L"\n資料夾：") + bs::lab::labFolder();
    if (st.ghelper) c += tr(L"\nG-Helper 執行中：它的設定變更時會另存一份；若它改動了實驗中的設定，那一段會排除並重做。");
    c += tr(L"\n\n用電池時的耗電拆解");
    bool any = false;
    for (int i = 0; i < int(bs::lab::Context::count); ++i) {
        const auto context = bs::lab::Context(i);
        const auto& t = st.breakdown.totals(context);
        if (t.seconds < 60) continue;
        any = true;
        const double battery = t.battery / t.seconds, core = t.core / t.seconds, package = t.package / t.seconds;
        c += L"\n" + std::wstring(tr(bs::lab::contextKey(context))) + L" " + durationText(t.seconds / 3600) + tr(L"：電池 ") + number(battery, 1) +
            tr(L" W ＝ CPU 核心 ") + number(core, 1) + tr(L" ＋ CPU 其他 ") + number(std::max(0.0, package - core), 1);
        if (t.restSeconds > 0) c += tr(L" ＋ 螢幕／獨顯／其他 ") + number(t.rest / t.restSeconds, 1);
        c += L" W";
    }
    if (!any) c += tr(L"\n還沒有資料：開啟記錄後用電池使用一段時間，每種情境至少 1 分鐘才會列出。");
    if (st.breakdown.batterySeconds() >= 60) {
        c += tr(L"\n獨顯醒著的時間：") + number(st.breakdown.dgpuAwakeSeconds() / st.breakdown.batterySeconds() * 100, 0) + L"%";
        const auto apps = st.breakdown.dgpuApps(5);
        if (!apps.empty()) {
            c += tr(L"；期間使用獨顯：");
            for (size_t i = 0; i < apps.size(); ++i) c += (i ? L"、" : L"") + apps[i].first;
        }
    }
    c += tr(L"\n\n實驗");
    if (st.running && st.plan) {
        c += tr(L"\n自動測試進行中：第 ") + std::to_wstring(st.step + 1) + L" / " + std::to_wstring(st.steps) + tr(L" 項 · ") +
            labKnobTitle(bs::lab::knobKey(st.knob)) + L" · " + tr(bs::lab::workloadName(st.workload)) +
            tr(L"\n剩約 ") + std::to_wstring(int(std::ceil(st.remainingSeconds / 60))) + tr(L" 分鐘（只計用電池的時間）");
        if (st.paused) c += tr(L"\n插電中，已暫停並還原；拔掉電源後繼續。");
    } else if (st.running) {
        c += L"\n" + std::wstring(labKnobTitle(bs::lab::knobKey(st.knob))) + L"：" + labArm(st.armNames[0]) + L" vs " + labArm(st.armNames[1]);
        if (st.paused) c += tr(L"\n插電中，已暫停並還原；拔掉電源後繼續。");
        else if (st.arm >= 0) c += tr(L"\n目前：") + labArm(st.armNames[size_t(st.arm)]) + tr(L" · 這段還剩 ") +
            std::to_wstring(int(std::max(0.0, bs::lab::Controller::BlockSeconds - st.blockElapsed))) + tr(L" 秒");
        c += tr(L"\n已完成 ") + std::to_wstring(st.pairsDone) + L" / " + std::to_wstring(bs::lab::Controller::TargetPairs) +
            tr(L" 組（每組是兩種設定各 2 分鐘，順序隨機）");
        if (st.externalChanges) c += tr(L"\n設定被其他程式改動 ") + std::to_wstring(st.externalChanges) + tr(L" 次，那幾段已排除並重做。");
    } else {
        if (st.finished) c += tr(L"\n上一個實驗已完成，設定已還原。");
        if (st.lastError) {
            c += tr(L"\n上次無法開始：") + errorText(st.lastError);
            if (st.lastError == ERROR_ACCESS_DENIED) c += tr(L"。這項設定需要系統管理員權限：以系統管理員身分執行 BatteryScope 後再開始。");
            if (st.lastError == ERROR_NOT_SUPPORTED) c += tr(L"。這台電腦或目前的設定不支援這項實驗。");
        }
        c += tr(L"\n從「⋯ → 省電實驗室」選一項開始。實驗只在用電池時進行，插電時自動暫停並還原；中途結束或當機，下次啟動時也會還原。");
    }
    if (!st.results.empty()) {
        c += tr(L"\n\n實驗結果（兩種設定成對比較，± 為 95% 範圍）");
        for (const auto& r : st.results) {
            if (r.arm0.empty() || r.arm1.empty()) continue; // an interrupted run from an older version
            c += L"\n" + labExperimentTitle(r.experiment) + L"：" + labArm(r.arm1) + tr(L" 相對於 ") + labArm(r.arm0);
            if (r.battery.pairs < 2) { c += tr(L"\n  資料不足：至少要 2 組完整的比較。"); continue; }
            c += tr(L"\n  電池 ") + signedNumber(r.battery.difference, 2) + L" W（±" + number(r.battery.halfWidth, 2) + L"，" +
                std::to_wstring(r.battery.pairs) + tr(L" 組）") + (r.battery.significant() ? tr(L" · 差異明確") : tr(L" · 差異不明確"));
            c += tr(L"\n  CPU 核心 ") + signedNumber(r.core.difference, 2) + tr(L" W · 螢幕／獨顯／其他 ") + signedNumber(r.rest.difference, 2) + L" W";
            c += tr(L"\n  CPU 使用率 ") + signedNumber(r.cpu.difference, 1) + L"%";
            if (r.frames) c += tr(L" · 延遲畫格 ") + signedNumber(r.drops.difference, 1) + tr(L" 次／分");
            if (r.bursts && r.burstMs.mean0 > 0) c += tr(L"\n  短暫運算耗時 ") + number(r.burstMs.mean0, 1) + L" → " + number(r.burstMs.mean1, 1) + L" ms";
            if (r.work && r.workRate.mean0 > 0 && r.energyPerWork.mean0 > 0)
                c += tr(L"\n  運算速度 ") + signedNumber(r.workRate.difference / r.workRate.mean0 * 100, 0) + tr(L"% · 每單位工作耗電 ") +
                    signedNumber(r.energyPerWork.difference / r.energyPerWork.mean0 * 100, 0) + L"%";
        }
    }
    if (!detectionTally.empty()) {
        c += tr(L"\n\n自動偵測準確率（自動測試期間，對照實際執行的情境）") + detectionTally.summary();
        c += tr(L"\n影片應判斷為「看影片」，高負載為「高負載」，瀏覽與文書為「閱讀」或「一般操作」，靜止畫面為「閒置」或「閱讀」。");
    }
    c += tr(L"\n\n說明：記錄時每 2 秒取樣一次，本身也會用一點電，但兩種設定都一樣，不影響比較。螢幕更新率實驗每 2 分鐘切換一次，畫面會短暫閃一下。掉格數來自 Windows 畫面合成器，部分全螢幕或可變更新率畫面可能不計入。");
    return c;
}
// One click: the lab folder plus a summary as a zip on the desktop, and the
// summary on the clipboard so it can be pasted straight into a chat.
void exportLab() {
    if (!labController) return;
    labController->flushNow();
    const std::wstring folder = bs::lab::labFolder();
    if (folder.empty()) { setNotice(tr(L"找不到實驗室資料夾。")); return; }
    flushDetectionLog();
    if (!detectionTally.empty()) {
        std::ofstream out(std::filesystem::path(folder) / L"detection.csv", std::ios::binary | std::ios::trunc);
        out << "\xEF\xBB\xBF" << utf8(detectionTally.csv());
    }
    std::wstring summary = L"BatteryScope " BS_VERSION_STRING L"\r\n" + labPageText();
    OSVERSIONINFOW os{}; os.dwOSVersionInfoSize = sizeof(os);
    using RtlGetVersion = LONG (WINAPI*)(OSVERSIONINFOW*);
    FARPROC versionSymbol = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    RtlGetVersion getVersion = nullptr; std::memcpy(&getVersion, &versionSymbol, sizeof(getVersion));
    if (getVersion) getVersion(&os);
    summary += L"\n\nWindows " + std::to_wstring(os.dwMajorVersion) + L"." + std::to_wstring(os.dwMinorVersion) + L"." + std::to_wstring(os.dwBuildNumber) +
        L" · " + std::to_wstring(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)) + L" logical processors";
    std::wstring crlf;
    for (size_t i = 0; i < summary.size(); ++i) { if (summary[i] == L'\n' && (i == 0 || summary[i - 1] != L'\r')) crlf += L'\r'; crlf += summary[i]; }
    {
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, crlf.c_str(), int(crlf.size()), nullptr, 0, nullptr, nullptr);
        std::string utf8(size_t(bytes), '\0');
        WideCharToMultiByte(CP_UTF8, 0, crlf.c_str(), int(crlf.size()), utf8.data(), bytes, nullptr, nullptr);
        std::ofstream out(std::filesystem::path(folder) / L"summary.txt", std::ios::binary | std::ios::trunc);
        out << "\xEF\xBB\xBF" << utf8;
    }
    // Copy the summary for pasting.
    if (OpenClipboard(mainWindow)) {
        EmptyClipboard();
        const size_t size = (crlf.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size)) {
            if (void* data = GlobalLock(memory)) { std::memcpy(data, crlf.c_str(), size); GlobalUnlock(memory); }
            if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
        }
        CloseClipboard();
    }
    // Zip with the tar.exe that ships with Windows 10 1803 and later.
    PWSTR desktop = nullptr;
    constexpr GUID DesktopFolder = {0xB4BFCC3A, 0xDB2C, 0x424C, {0xB0, 0x29, 0x7F, 0xE9, 0x9A, 0x87, 0xC6, 0x41}};
    std::wstring target;
    if (SUCCEEDED(SHGetKnownFolderPath(DesktopFolder, 0, nullptr, &desktop)) && desktop) target = desktop;
    CoTaskMemFree(desktop);
    SYSTEMTIME t{}; GetLocalTime(&t);
    wchar_t name[64]; swprintf(name, 64, L"\\BatteryScope-lab-%04u%02u%02u-%02u%02u.zip", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    target += name;
    wchar_t system[MAX_PATH]{}; GetSystemDirectoryW(system, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(system) + L"\\tar.exe\" -a -c -f \"" + target + L"\" -C \"" + folder + L"\" .";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    bool zipped = false;
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        DWORD code = 1;
        if (WaitForSingleObject(process.hProcess, 30000) == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &code);
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
        zipped = code == 0 && GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES;
    }
    if (zipped) {
        const std::wstring select = L"/select,\"" + target + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", select.c_str(), nullptr, SW_SHOWNORMAL);
        setNotice(tr(L"已匯出到桌面，摘要也已複製，可直接貼上。"));
    } else {
        ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        setNotice(tr(L"無法建立壓縮檔，已開啟資料夾；摘要已複製，可直接貼上。"));
    }
}
// Preview data for the lab charts (demo and screenshots), from the first real test.
bs::lab::Controller::Status demoLabStatus() {
    bs::lab::Controller::Status st;
    auto result = [](const wchar_t* key, const wchar_t* a0, const wchar_t* a1, std::vector<std::pair<double, double>> battery, std::vector<std::pair<double, double>> core) {
        bs::lab::ExperimentResult r; r.experiment = key; r.arm0 = a0; r.arm1 = a1;
        r.battery = bs::lab::comparePairs(battery); r.core = bs::lab::comparePairs(core); return r;
    };
    st.results = {
        result(L"power-mode@browse", L"效能", L"效率", {{15.6, 13.0}, {16.0, 13.8}, {15.4, 13.7}}, {{6.1, 2.5}, {5.8, 2.5}, {5.4, 2.5}}),
        result(L"power-mode@idle", L"效能", L"效率", {{17.6, 13.5}, {16.2, 13.9}, {15.1, 13.3}}, {{4.9, 2.5}, {5.3, 3.6}, {5.0, 2.2}}),
        result(L"cpu-boost@office", L"Boost 開", L"Boost 關", {{19.1, 15.2}, {18.7, 13.9}, {21.8, 14.8}}, {{9.7, 4.2}, {10.2, 4.3}, {10.5, 4.2}}),
        result(L"refresh@video", L"120 Hz", L"60 Hz", {{16.1, 15.2}, {16.2, 14.9}, {16.5, 15.5}}, {{6.2, 5.6}, {6.2, 5.4}, {6.2, 5.7}}),
        result(L"epp@browse", L"能源偏好 60", L"能源偏好 85", {{15.5, 18.3}, {16.2, 15.8}, {15.0, 15.4}}, {{5.5, 6.0}, {6.0, 5.9}, {5.3, 5.5}}),
    };
    return st;
}
std::vector<VBlock> labBlocks(const bs::lab::Controller::Status& st) {
    std::vector<VBlock> v;
    // Tests first: one row starts (or stops) each, then the results.
    if (st.running && st.plan) v.push_back(vAction(tr(L"停止自動測試"), std::to_wstring(st.step + 1) + L" / " + std::to_wstring(st.steps), LabAutoId, theme.warn));
    else {
        v.push_back(vAction(tr(L"快速測試"), tr(L"約 30 分鐘"), LabQuickId, Green));
        v.push_back(vAction(tr(L"完整測試"), tr(L"約 1.5 小時"), LabAutoId));
    }
    v.push_back(vAction(tr(L"匯出結果"), tr(L"壓縮檔＋摘要"), LabExportId));
    v.push_back(vAction(tr(L"記錄詳細資料"), st.recording ? tr(L"開") : tr(L"關"), LabRecordId, st.recording ? Green : COLORREF(0)));
    v.push_back(vAction(tr(L"單項實驗"), st.running && !st.plan ? tr(L"進行中") : std::wstring(), LabSingleMenuId));
    v.push_back(vAction(tr(L"開啟資料夾"), L"", LabFolderId));
    if (st.running && st.plan) {
        v.push_back(vHeading(tr(L"自動測試進行中")));
        v.push_back(vMeter(labKnobTitle(bs::lab::knobKey(st.knob)) + std::wstring(L" · ") + tr(bs::lab::workloadName(st.workload)),
            std::to_wstring(st.step + 1) + L" / " + std::to_wstring(st.steps) + tr(L" 項 · 剩約 ") + std::to_wstring(int(std::ceil(st.remainingSeconds / 60))) + tr(L" 分"),
            double(st.step + 1), 0, double(std::max(1, st.steps))));
        if (st.paused) v.push_back(vNote(tr(L"插電中，已暫停並還原；拔掉電源後繼續。")));
    }
    // What each setting saves, per activity: the bar is the saving, the whisker its 95% range.
    std::vector<VBar> savings;
    for (const auto& r : st.results) {
        if (r.arm0.empty() || r.arm1.empty() || r.battery.pairs < 2) continue;
        const bool coreClear = r.core.significant() && r.core.difference * r.battery.difference > 0;
        const bool clear = r.battery.significant() || coreClear;
        const bool secondSaves = r.battery.difference < 0;
        const std::wstring saver = labArm(secondSaves ? r.arm1 : r.arm0), other = labArm(secondSaves ? r.arm0 : r.arm1);
        const double watts = std::abs(r.battery.difference);
        savings.push_back({labExperimentTitle(r.experiment) + (english ? L": " : L"：") + saver + L" vs " + other,
            clear ? tr(L"省 ") + number(watts, 1) + L" W" : tr(L"不明確"), watts, std::isfinite(r.battery.halfWidth) ? r.battery.halfWidth : 0,
            clear ? Green : blend(Muted, Surface, .35), {}});
    }
    std::stable_sort(savings.begin(), savings.end(), [](const VBar& a, const VBar& b) { return a.amount > b.amount; });
    v.push_back(vHeading(tr(L"每項設定能省多少")));
    if (savings.empty()) v.push_back(vNote(tr(L"還沒有結果：從「⋯ → 省電實驗室 → 快速測試」開始，約 25 分鐘。")));
    else {
        v.push_back(vBars(std::move(savings)));
        v.push_back(vLegend({{tr(L"差異明確"), Green}, {tr(L"差異不明確"), blend(Muted, Surface, .35)}}));
        v.push_back(vNote(tr(L"每條是兩種設定輪流量測的平均差，細線是 95% 範圍；「明確」才會被動態省電採用。")));
    }
    // Where battery power goes per activity: CPU cores, the rest of the processor, and everything else.
    std::vector<VBar> split;
    const COLORREF coreColor = Green, chipColor = blend(Green, Surface, .5), restColor = blend(Muted, Surface, .3);
    for (int i = 0; i < int(bs::lab::Context::count); ++i) {
        const auto& t = st.breakdown.totals(bs::lab::Context(i));
        if (t.seconds < 60) continue;
        const double core = t.core / t.seconds, chip = std::max(0.0, t.package / t.seconds - core), rest = t.restSeconds > 0 ? t.rest / t.restSeconds : 0;
        split.push_back({std::wstring(tr(bs::lab::contextKey(bs::lab::Context(i)))) + L" · " + durationText(t.seconds / 3600),
            number(t.battery / t.seconds, 1) + L" W", 0, 0, 0, {{core, coreColor}, {chip, chipColor}, {rest, restColor}}});
    }
    if (!split.empty()) {
        v.push_back(vHeading(tr(L"用電池時，電花在哪裡")));
        v.push_back(vBars(std::move(split)));
        v.push_back(vLegend({{tr(L"CPU 核心"), coreColor}, {tr(L"CPU 其他"), chipColor}, {tr(L"螢幕與其他"), restColor}}));
    }
    if (!detectionTally.empty()) {
        v.push_back(vHeading(tr(L"自動偵測準確率")));
        std::vector<VBar> bars;
        for (const wchar_t* w : {L"idle", L"browse", L"office", L"video", L"sustained", L"scroll", L"bursts"})
            if (auto a = detectionTally.accuracy(w)) {
                std::wstring name = w;
                for (int i = 1; i < int(bs::lab::Workload::count); ++i) if (name == bs::lab::workloadKey(bs::lab::Workload(i))) name = tr(bs::lab::workloadName(bs::lab::Workload(i)));
                bars.push_back({name, number(*a * 100, 0) + L"%", *a * 100, 0, *a >= .8 ? Green : theme.warn, {}});
            }
        v.push_back(vBars(std::move(bars), 100));
    }
    v.push_back(vNote(tr(L"資料只存在本機：") + bs::lab::labFolder()));
    return v;
}
void showLabPage() {
    if (!labController) {
        showChartPage(InlinePage::lab, tr(L"省電實驗室"), labPageText(), labBlocks(demoLabStatus()));
        return;
    }
    showChartPage(InlinePage::lab, tr(L"省電實驗室"), labPageText(), labBlocks(labController->status()));
    labPageShownAt = GetTickCount64();
}
void showPowerModePage() {
    refreshPowerModeControls();
    showInlinePage(InlinePage::powerMode, tr(L"電源模式"), powerModeText());
    releaseInteraction();
}
std::wstring candidatesText() {
    const auto candidates = reviewCandidates(current);
    std::wstring content = tr(L"依持續 CPU 活動、可讀身分與前景／服務關聯，列出值得檢查的背景程序。這不是安全關閉判定，也不是關閉後可省電量。\n");
    if (candidates.empty()) content += tr(L"\n目前沒有符合條件的候選，或觀測資料尚不足。\n");
    for (size_t index : candidates) {
        const auto& row = current.processes[index];
        content += L"\n• " + row.name + L" · PID " + std::to_wstring(row.pid) + L" · " +
            valueText(row.averageCpuWatts, L" CPU W", 2) + L" · " + std::to_wstring(int(row.busySeconds)) + tr(L" 秒持續活動");
        if (!current.history.empty() && row.averageCpuWatts)
            if (auto gain = extraMinutesIfStopped(current.power, current.history.back(), *row.averageCpuWatts))
                content += tr(L" · 關閉約可多用 ") + std::to_wstring(int(*gain)) + tr(L" 分");
    }
    content += tr(L"\n\n可能有同步、下載、音訊、未儲存資料或其他程序依賴。請先查看詳情；程式不會自動休眠或關閉候選。" );
    return content;
}
// "Programs using power": now (CPU watts), since BatteryScope started (energy), and
// background programs worth a look, as bars.
std::vector<const ProcessRow*> busyPrograms(size_t count);
std::vector<VBlock> ledgerBlocks() {
    std::vector<VBlock> v;
    if (const auto busy = busyPrograms(8); !busy.empty()) {
        v.push_back(vHeading(tr(L"現在（CPU 耗電）")));
        std::vector<VBar> bars;
        for (const auto* row : busy) bars.push_back({row->name, number(*row->averageCpuWatts, 2) + L" W", *row->averageCpuWatts, 0, theme.warn, {}});
        v.push_back(vBars(std::move(bars)));
    }
    v.push_back(vHeading(tr(L"從 BatteryScope 開啟到現在")));
    if (current.ledger.empty()) v.push_back(vNote(tr(L"尚無資料：需要可用的 CPU 能源計數器與一段觀測時間。")));
    else {
        std::vector<VBar> bars;
        for (size_t i = 0; i < current.ledger.size() && i < 10; ++i) {
            const auto& e = current.ledger[i];
            std::wstring value = number(e.milliWh, 0) + L" mWh";
            if (current.ledgerCoreMilliWh > 0) value += L" · " + number(e.milliWh / current.ledgerCoreMilliWh * 100, 0) + L"%";
            bars.push_back({e.name + (e.running ? L"" : tr(L"（已結束）")), value, e.milliWh, 0, e.running ? Green : blend(Muted, Surface, .35), {}});
        }
        v.push_back(vBars(std::move(bars)));
    }
    const auto candidates = reviewCandidates(current);
    v.push_back(vHeading(tr(L"值得檢查的背景程式")));
    if (candidates.empty()) v.push_back(vNote(tr(L"目前沒有持續耗電的背景程式。")));
    else {
        std::vector<VBar> bars;
        for (size_t index : candidates) {
            const auto& row = current.processes[index];
            std::wstring value = valueText(row.averageCpuWatts, L" W", 2);
            if (!current.history.empty() && row.averageCpuWatts)
                if (auto gain = extraMinutesIfStopped(current.power, current.history.back(), *row.averageCpuWatts))
                    value += tr(L" · 多用 ") + std::to_wstring(int(*gain)) + tr(L" 分");
            bars.push_back({row.name, value, row.averageCpuWatts.value_or(0), 0, theme.warn, {}});
        }
        v.push_back(vBars(std::move(bars)));
        v.push_back(vNote(tr(L"它們在背景持續用 CPU。可能是同步、下載或更新；在排行中選它，可看詳情、改效率模式或關閉。")));
    }
    v.push_back(vNote(tr(L"這是 CPU 能源分攤估計，不含螢幕、顯示卡與儲存；關閉程式不保證省下相同能量。")));
    return v;
}
// Programs using power are part of the battery tab.
void showLedger() { showHistoryPage(); }
void showReviewCandidates() { showLedger(); }
// thisCopy: the Run entry launches this executable. otherCopy: another BatteryScope
// that still exists. missingCopy: the registered executable was moved or deleted.
enum class StartupState { off, thisCopy, otherCopy, missingCopy };
bool samePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), int(a.size()), b.c_str(), int(b.size()), TRUE) == CSTR_EQUAL;
}
StartupState startupState() {
    constexpr wchar_t runKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, runKey, L"BatteryScope", RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS)
        return StartupState::off;
    std::wstring command(bytes / sizeof(wchar_t) + 1, L'\0');
    bytes = DWORD(command.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_CURRENT_USER, runKey, L"BatteryScope", RRF_RT_REG_SZ, nullptr, command.data(), &bytes) != ERROR_SUCCESS)
        return StartupState::off;
    command.resize(wcsnlen(command.c_str(), command.size()));
    std::wstring registered;
    if (!command.empty() && command.front() == L'"') {
        const size_t close = command.find(L'"', 1);
        if (close != std::wstring::npos) registered = command.substr(1, close - 1);
    } else registered = command.substr(0, command.find(L' '));
    if (!registered.empty() && samePath(registered, executablePath())) return StartupState::thisCopy;
    const DWORD attributes = registered.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(registered.c_str());
    return attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)
        ? StartupState::missingCopy : StartupState::otherCopy;
}
bool startupEnabled() { return startupState() == StartupState::thisCopy; }
DWORD setStartup(bool enable) {
    HKEY key = nullptr;
    LSTATUS result = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (result != ERROR_SUCCESS) return result;
    std::wstring command = L"\"" + executablePath() + L"\" --tray";
    result = enable ? RegSetValueExW(key, L"BatteryScope", 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()), DWORD((command.size() + 1) * sizeof(wchar_t)))
        : RegDeleteValueW(key, L"BatteryScope");
    RegCloseKey(key); return result == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : result;
}
std::wstring hexName(const std::wstring& name) {
    std::wostringstream stream; stream << std::hex << std::setfill(L'0');
    for (auto c : name) stream << std::setw(4) << unsigned(c);
    return stream.str();
}
std::wstring unhexName(const std::wstring& text) {
    if (text.empty() || text.size() % 4 || text.size() > 1024) return {};
    std::wstring result;
    for (size_t i = 0; i < text.size(); i += 4) {
        auto part = text.substr(i, 4);
        if (part.find_first_not_of(L"0123456789abcdefABCDEF") != std::wstring::npos) return {};
        unsigned long value = wcstoul(part.c_str(), nullptr, 16); if (!value) return {};
        result.push_back(wchar_t(value));
    }
    return result;
}
DWORD elevatedService(const ServiceInfo& service, const ProcessRow& row) {
    std::wstring path = executablePath(), args = L"--stop-service " + hexName(service.name) + L" " + std::to_wstring(row.pid) + L" " + std::to_wstring(row.created);
    SHELLEXECUTEINFOW info{}; info.cbSize = sizeof(info); info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.hwnd = mainWindow; info.lpVerb = L"runas"; info.lpFile = path.c_str(); info.lpParameters = args.c_str(); info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) return GetLastError();
    Handle process(info.hProcess);
    if (!process) return ERROR_INVALID_HANDLE;
    if (WaitForSingleObject(process, 60000) != WAIT_OBJECT_0) return ERROR_TIMEOUT;
    DWORD code = ERROR_GEN_FAILURE; GetExitCodeProcess(process, &code); return code;
}
void startAction(ProcessRow row, bool force, std::optional<ServiceInfo> service = {}) {
    if (pendingAction || demo) return;
    pendingAction = true; forceAction = force; serviceAction = service.has_value();
    updateButtons(); setNotice(service ? tr(L"正在停止服務…") : force ? tr(L"正在結束程序…") : tr(L"正在傳送正常關閉要求…"));
    if (actionWorker.joinable()) actionWorker.join();
    actionWorker = std::thread([row, force, service] {
        DWORD result;
        if (service) {
            result = stopService(service->name, row.pid, row.created);
            if (result == ERROR_ACCESS_DENIED) result = elevatedService(*service, row);
        } else result = closeProcess(row, force);
        PostMessageW(mainWindow, ActionMessage, result, 0);
    });
}
void chooseService(const ProcessRow& row) {
    std::vector<std::wstring> labels;
    for (const auto& service : row.services) labels.push_back(service.display + L" (" + service.name + L")" + (service.stoppable ? L"" : tr(L" — 不接受停止")));
    std::vector<TASKDIALOG_BUTTON> choices;
    for (size_t i = 0; i < labels.size(); ++i) choices.push_back({300 + int(i), labels[i].c_str()});
    TASKDIALOG_BUTTON button{101, tr(L"停止選取的服務")};
    TASKDIALOGCONFIG config{}; config.cbSize = sizeof(config); config.hwndParent = mainWindow;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    config.dwCommonButtons = TDCBF_CANCEL_BUTTON; config.pszWindowTitle = tr(L"BatteryScope · 停止服務");
    config.pszMainInstruction = tr(L"選擇要停止的服務");
    config.pszContent = tr(L"此操作可能中斷相關功能。共用程序內只停止選取服務，不結束整個程序。\n有執行中的相依服務時會拒絕操作。需要時 Windows 會要求管理員權限。");
    config.cRadioButtons = UINT(choices.size()); config.pRadioButtons = choices.data(); config.nDefaultRadioButton = 300;
    for (size_t i = 0; i < row.services.size(); ++i) if (row.services[i].stoppable) { config.nDefaultRadioButton = 300 + int(i); break; }
    config.cButtons = 1; config.pButtons = &button; config.nDefaultButton = IDCANCEL;
    int chosen = 0, pressed = 0;
    if (SUCCEEDED(TaskDialogIndirect(&config, &pressed, &chosen, nullptr)) && pressed == 101 && chosen >= 300 && size_t(chosen - 300) < row.services.size()) {
        auto service = row.services[size_t(chosen - 300)];
        if (!service.stoppable) { setNotice(tr(L"此服務目前不接受停止要求。")); return; }
        startAction(row, false, service);
    }
}
void refreshProcessButtons() {
    if (!ecoButton || !perfAppButton) return;
    const bool editable = !demo && !isolatedPreview && inlineProcess.has_value();
    const bool efficient = inlineProcess && isEfficient(*inlineProcess), listed = inlineProcess && performanceListed(*inlineProcess);
    SetWindowTextW(ecoButton, efficient ? tr(L"取消效率模式") : tr(L"效率模式"));
    {
        bool learned = false;
        const auto rule = inlineProcess ? detector.ruleFor(lowerName(inlineProcess->name), &learned) : bs::gov::AppRule::automatic;
        SetWindowTextW(perfAppButton, (tr(L"規則：") + std::wstring(tr(bs::gov::ruleKey(listed ? bs::gov::AppRule::performance : rule)))).c_str());
    }
    EnableWindow(ecoButton, editable && !pendingAction && (efficient || inlineProcess->actionable));
    EnableWindow(perfAppButton, editable && !inlineProcess->name.empty());
    InvalidateRect(ecoButton, nullptr, FALSE); InvalidateRect(perfAppButton, nullptr, FALSE);
}
std::wstring energySourceText(const CpuEnergyReport& energy) {
    switch (energy.source) {
    case EnergySource::perCore: return tr(L"每核心能源（最精確）");
    case EnergySource::coreTotal: return tr(L"全部核心總能源");
    case EnergySource::package: return tr(L"CPU 封裝能源（扣除閒置基準）");
    default: return tr(L"無，改用 CPU／I/O 活動排行");
    }
}
void showDetails(const ProcessRow& row) {
    if (!inlineProcess || &row != &*inlineProcess) inlineProcess = row;
    std::wstring content = row.name + L"\nPID: " + std::to_wstring(row.pid) +
        tr(L"\n路徑：") + (row.path.empty() ? tr(L"未知") : row.path) +
        tr(L"\n\nCPU（全機比例）: ") + number(row.cpu) + tr(L"%\n30 秒平滑 CPU: ") + number(row.averageCpu) +
        L"%\nI/O: " + (row.ioKnown ? number(row.ioMB, 2) + L" MB/s" : tr(L"未知")) +
        tr(L"\n記憶體: ") + (row.memoryKnown ? number(row.memoryMB) + tr(L" MB（") + number(row.memoryAgeSeconds, 0) + tr(L" 秒前）") : tr(L"未知")) +
        tr(L"\n活動: ") + activityText(row) +
        tr(L"\nGPU 使用率: ") + (row.gpuPercent ? number(*row.gpuPercent, 0) + tr(L"%（最忙碌的 GPU 引擎；不是瓦數）") : std::wstring(tr(L"未取得")));
    if (row.cpuWatts) {
        content += tr(L"\n\n本次 CPU 估計: ") + valueText(row.cpuWatts, L" W", 2) +
            tr(L"\n30 秒平滑 CPU 估計: ") + valueText(row.averageCpuWatts, L" W", 2) +
            tr(L"\n30 秒平滑的核心分攤模型範圍: ") + valueText(row.cpuWattsLow, L"", 2) + L"–" + valueText(row.cpuWattsHigh, L" W", 2) +
            tr(L"\n監測期間 CPU 估計能量: ") + valueText(row.cpuMilliWh, L" mWh", 2);
        content += tr(L"\n能源來源: ") + energySourceText(current.energy);
        if (current.energy.source == EnergySource::perCore)
            content += tr(L"\n本次固定核心資訊覆蓋: ") + number(row.placementCoverage*100,0) + L"%";
        content += tr(L"\n全機 CPU 時間覆蓋: ") + number(current.energy.observedCpuCoverage*100,0) +
            tr(L"%（可讀程序時間／系統忙碌時間，非準確率）");
        content += tr(L"\n觀測品質: ") + qualityText(current.energy.quality) +
            tr(L" · 採樣端點差 ") + number(current.energy.sampleSkewMs, 0) + L" ms";
        if (!current.history.empty() && current.history.back().fullWh && row.averageCpuWatts)
            content += tr(L"\nCPU 部分估計耗電速度: ") + number(*row.averageCpuWatts / *current.history.back().fullWh * 100, 2) + tr(L" 百分點／小時");
        if (current.energy.source == EnergySource::coreTotal)
            content += tr(L"\n\n這台電腦的 EMI 只提供全部核心的總能源，沒有每核心資料，因此依 CPU 時間（有週期資料時依週期）分攤。範圍固定為估計值的 0.8–1.25 倍，代表各核心時脈與負載不同造成的不確定性，並非統計信賴區間。");
        else if (current.energy.source == EnergySource::package)
            content += tr(L"\n\n這台電腦的 EMI 只提供 CPU 封裝能源（含記憶體控制器、內建顯示等共用部分）。先扣除近 10 分鐘最低的封裝功率作為閒置基準（目前 ") +
                valueText(current.energy.packageFloorWatts, L" W") +
                tr(L"），其餘依 CPU 時間分攤；範圍上限是連閒置基準也按比例分攤的值。");
        else
            content += tr(L"\n\n用 EMI 量到的核心能源約束分攤。持續固定在同一核心的執行緒優先按該核心分配；其餘依 CPU 時間估計。範圍反映未知的核心排程位置，並非統計信賴區間；不同指令與 SMT 仍可能產生誤差。");
        if (!current.history.empty() && row.averageCpuWatts)
            if (auto gain = extraMinutesIfStopped(current.power, current.history.back(), *row.averageCpuWatts))
                content += tr(L"\n若關閉此程式：CPU 部分約可多用 ") + std::to_wstring(int(*gain)) + tr(L" 分鐘（依近 5 分鐘耗電估算；不含 GPU、螢幕，非保證）");
        if (current.energy.cycleWeighted)
            content += tr(L"\n本次其餘部分依 CPU 週期加權：同樣的 CPU 時間在較高時脈下會分到較多能源。");
    }
    content += tr(L"\n\n本次 CPU 核心 ") + valueText(current.energy.coreWatts, L" W") +
        tr(L" · CPU 未歸因 ") + valueText(current.energy.unassignedCpuWatts, L" W") +
        tr(L"\nCPU 以外／未歸因 ") + valueText(current.energy.otherSystemWatts, L" W");
    content += tr(L"\n\n這是 CPU 能源分攤（包含活躍核心的共用開銷），不是程式總功率或關閉後保證能省的瓦數。GPU、螢幕、儲存等沒有硬分配給程序。");
    if (!row.actionable && row.services.empty()) content += tr(L"\n\n此程序受保護、屬於其他使用者，或無法確認安全狀態，因此不提供關閉操作。");
    if (!row.services.empty()) {
        content += tr(L"\n\n所屬服務（共用程序時無法分別歸因）：");
        for (const auto& s : row.services) content += L"\n• " + s.display + L" (" + s.name + L")";
    }
    if (isEfficient(row)) content += tr(L"\n\n效率模式：開啟（由 BatteryScope 設定；結束 BatteryScope 時會還原）。");
    if (performanceListed(row)) content += tr(L"\n效能清單：此程式執行時，插電與用電池都會自動切到效能模式。");
    showInlinePage(InlinePage::process, tr(L"程序詳情"), content);
    refreshProcessButtons();
}
void showForceConfirmation(const ProcessRow& row) {
    if (!pendingForceRow || &row != &*pendingForceRow) pendingForceRow = row;
    std::wstring text = tr(L"強制結束 ") + row.name + tr(L"（PID ") + std::to_wstring(row.pid) +
        tr(L"）？\n\n未儲存資料可能遺失。此操作僅結束所選程序，不包含其他子程序。");
    showInlinePage(InlinePage::confirm, tr(L"確認強制結束"), text);
}

int localDate(int daysBack) {
    SYSTEMTIME now{}; GetLocalTime(&now);
    if (daysBack) {
        FILETIME file{}; SystemTimeToFileTime(&now, &file);
        ULARGE_INTEGER value{}; value.LowPart = file.dwLowDateTime; value.HighPart = file.dwHighDateTime;
        value.QuadPart -= uint64_t(daysBack) * 24 * 3600 * 10000000ull;
        file.dwLowDateTime = value.LowPart; file.dwHighDateTime = value.HighPart;
        FileTimeToSystemTime(&file, &now);
    }
    return now.wYear * 10000 + now.wMonth * 100 + now.wDay;
}
// %LOCALAPPDATA%\BatteryScope\history.csv; empty if the folder is unavailable.
std::wstring historyPath() {
    PWSTR folder = nullptr;
    std::wstring path;
    // FOLDERID_LocalAppData, defined here so no extra GUID library is needed.
    static const KNOWNFOLDERID localAppData = {0xF1B32785, 0x6FBA, 0x4FCF, {0x9D, 0x55, 0x7B, 0x8E, 0x7F, 0x15, 0x70, 0x91}};
    if (SUCCEEDED(SHGetKnownFolderPath(localAppData, 0, nullptr, &folder)) && folder) path = std::wstring(folder) + L"\\BatteryScope";
    if (folder) CoTaskMemFree(folder);
    return path.empty() ? path : path + L"\\history.csv";
}
bool loadHistory(DailyHistory& history) {
    std::ifstream in(std::filesystem::path(historyPath()), std::ios::binary);
    if (!in) return false;
    std::string bytes((std::istreambuf_iterator<char>(in)), {});
    if (bytes.rfind("\xEF\xBB\xBF", 0) == 0) bytes.erase(0, 3);
    const int size = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), int(bytes.size()), nullptr, 0);
    std::wstring text(size_t(std::max(0, size)), L'\0');
    if (size > 0) MultiByteToWideChar(CP_UTF8, 0, bytes.data(), int(bytes.size()), text.data(), size);
    return history.load(text);
}
// Writes to a temporary file first so a crash never leaves a truncated history.
bool saveHistory(const DailyHistory& history) {
    const std::wstring path = historyPath();
    if (path.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
    const std::wstring temporary = path + L".tmp";
    if (!writeUtf8(temporary, history.save())) return false;
    return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}
void publishGovernorGpu(const std::vector<std::pair<std::wstring, double>>& instances, uint64_t now) {
    auto summary = bs::gov::summarizeGpu(instances);
    std::lock_guard<std::mutex> lock(governorGpuMutex);
    governorGpuLatest = std::move(summary); governorGpuAt = now;
}
// ----- Performance readings for the home page -----
// What the performance dashboard shows that the regular snapshot does not: CPU clock, integrated
// and discrete GPU use, the discrete GPU's power state and, only while it is awake anyway, its
// temperature, clock and power from NVIDIA's driver (reading it would otherwise keep it awake).
struct PerfReading {
    uint64_t ms = 0;
    Value cpuMhz, igpu, dgpu, nvTemp, nvClock, nvWatts, nvLoad;
    int dgpuState = -1;          // 0 awake (D0) ... 3 asleep (D3), 9 off (Eco), -1 unknown
    std::wstring dgpuApps;       // programs using it, ";"-separated
};
std::mutex perfMutex;
PerfReading perfLatest;
// The processor's clock: Windows' "% Processor Performance" times the base clock, as Task Manager shows it.
class CpuClock {
public:
    ~CpuClock() { if (query_) PdhCloseQuery(query_); }
    Value read() {
        if (!opened_) {
            opened_ = true;
            if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) { query_ = nullptr; return std::nullopt; }
            if (PdhAddEnglishCounterW(query_, L"\\Processor Information(_Total)\\% Processor Performance", 0, &counter_) != ERROR_SUCCESS) {
                PdhCloseQuery(query_); query_ = nullptr; return std::nullopt;
            }
            base_ = baseMhz();
        }
        if (!query_ || !base_ || PdhCollectQueryData(query_) != ERROR_SUCCESS) return std::nullopt;
        PDH_FMT_COUNTERVALUE value{};
        if (PdhGetFormattedCounterValue(counter_, PDH_FMT_DOUBLE, nullptr, &value) != ERROR_SUCCESS || value.CStatus != ERROR_SUCCESS) return std::nullopt;
        return base_ * value.doubleValue / 100;
    }
private:
    static double baseMhz() {
        struct Info { ULONG number, maxMhz, currentMhz, mhzLimit, maxIdleState, currentIdleState; };
        LONG (WINAPI* information)(int, PVOID, ULONG, PVOID, ULONG) = nullptr;
        if (HMODULE module = LoadLibraryExW(L"powrprof.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
            FARPROC symbol = GetProcAddress(module, "CallNtPowerInformation");
            std::memcpy(&information, &symbol, sizeof(information));
        }
        if (!information) return 0;
        std::vector<Info> cpus(std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));
        if (information(11 /* ProcessorInformation */, nullptr, 0, cpus.data(), ULONG(cpus.size() * sizeof(Info))) != 0) return 0;
        return double(cpus.front().maxMhz);
    }
    PDH_HQUERY query_ = nullptr; PDH_HCOUNTER counter_ = nullptr; double base_ = 0; bool opened_ = false;
};
// NVIDIA's management library, from the driver; only asked while the GPU is already awake.
struct NvReading { Value temp, clock, watts, load; };
NvReading readNvidia() {
    struct Api {
        int (*init)() = nullptr; int (*shutdown)() = nullptr; int (*handle)(unsigned, void**) = nullptr;
        int (*temperature)(void*, int, unsigned*) = nullptr; int (*clock)(void*, int, unsigned*) = nullptr;
        int (*power)(void*, unsigned*) = nullptr; int (*utilization)(void*, unsigned*) = nullptr;
    };
    static const Api api = [] {
        Api a; HMODULE module = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        auto bind = [&](const char* name, auto& target) { FARPROC symbol = module ? GetProcAddress(module, name) : nullptr; std::memcpy(&target, &symbol, sizeof(target)); };
        bind("nvmlInit_v2", a.init); bind("nvmlShutdown", a.shutdown); bind("nvmlDeviceGetHandleByIndex_v2", a.handle);
        bind("nvmlDeviceGetTemperature", a.temperature); bind("nvmlDeviceGetClockInfo", a.clock);
        bind("nvmlDeviceGetPowerUsage", a.power); bind("nvmlDeviceGetUtilizationRates", a.utilization);
        return a;
    }();
    NvReading r;
    if (!api.init || !api.shutdown || !api.handle || api.init() != 0) return r;
    void* device = nullptr;
    if (api.handle(0, &device) == 0) {
        unsigned value = 0, rates[2]{};
        if (api.temperature && api.temperature(device, 0, &value) == 0) r.temp = value;
        if (api.clock && api.clock(device, 0, &value) == 0) r.clock = value;
        if (api.power && api.power(device, &value) == 0) r.watts = value / 1000.0;
        if (api.utilization && api.utilization(device, rates) == 0) r.load = rates[0];
    }
    api.shutdown();
    return r;
}
void publishPerf(const bs::lab::Sample& sample, CpuClock& clock, uint64_t now) {
    PerfReading r;
    r.ms = now; r.cpuMhz = clock.read(); r.igpu = sample.igpuPercent; r.dgpu = sample.dgpuPercent;
    r.dgpuState = sample.dgpuState; r.dgpuApps = sample.dgpuApps;
    // Awake and in use: asking the driver costs nothing more then.
    if (r.dgpuState == 0 && ((r.dgpu && *r.dgpu >= 1) || !r.dgpuApps.empty())) {
        const auto nv = readNvidia();
        r.nvTemp = nv.temp; r.nvClock = nv.clock; r.nvWatts = nv.watts; r.nvLoad = nv.load;
    }
    std::lock_guard<std::mutex> lock(perfMutex);
    perfLatest = std::move(r);
}
void sampleLoop() {
    Collector collector;
    EnergyReader energyReader;
    CpuAttributor attributor;
    EnergyLedger ledger;
    GpuUsage gpu;
    DailyHistory history;
    bool historyDirty = false;
    uint64_t historySavedAt = GetTickCount64();
    if (historyEnabled) loadHistory(history);
    SamplingPolicy policy;
    Snapshot state;
    state.days = history.recent(14);
    uint64_t nextProcess = 0, nextPower = 0, nextGpu = 0;
    bool previousHidden = false, qosInitialized = false;
    // Power lab: undo a setting left changed by a crash, then sample every two seconds while it is on.
    if (labController) bs::lab::restoreInterrupted();
    std::unique_ptr<bs::lab::Probe> labProbe;
    uint64_t labLast = 0;
    CpuClock cpuClock;
    while (!quit) {
        auto now = GetTickCount64(); bool background = hidden.load(), processSampled = false;
        const bool labActive = labController && labController->active();
        if (resetSamples.exchange(false)) { collector.reset(); energyReader.reset(); policy.reset(); nextProcess = nextPower = 0; }
        if (!qosInitialized || previousHidden != background) {
            state.samplerEcoQoS = setSamplerEcoQoS(background) && background;
            qosInitialized = true; policy.reset(); nextProcess = nextPower = 0; previousHidden = background;
        }
        bool changed = false;
        if (now >= nextPower) {
            const PowerSample sample = readPower();
            // A gap of 10+ minutes between samples means the system (or this app) was asleep.
            if (!state.history.empty()) if (auto sleep = evaluateSleep(state.history.back(), sample)) {
                state.lastSleep = sleep; ++state.sleepCount;
                if (historyEnabled) { history.addSleep(*sleep, localDate()); historyDirty = true; }
            }
            appendPower(state.history, sample); state.power = calculatePower(state.history);
            nextPower = now + (background ? 10000 : 6000); changed = true;
            if (historyEnabled) { history.addPower(state.history.back(), localDate()); historyDirty = true; }
        }
        if (now >= nextProcess) {
            state.energy = energyReader.sample();
            state.processes = collector.collect(state.totalProcesses, state.inaccessible, background);
            state.servicesKnown = collector.servicesKnown();
            alignProcessObservation(state.energy, collector.intervalSeconds(), collector.sampleMs(), collector.systemSnapshot());
            // Per-process watts are not shown while hidden, so skip thread placement there.
            if (background) energyReader.clearThreadPlacement();
            else energyReader.sampleThreadPlacement(state.processes, collector.threadSnapshot());
            attributor.apply(state.processes, state.energy, collector.sampleMs(), std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));
            ledger.update(state.processes, state.energy, collector.sampleMs());
            if (historyEnabled) { history.addApps(state.processes, state.energy, collector.sampleMs(), localDate()); historyDirty = true; }
            // GPU utilization is only shown in the panel, so it is sampled only while visible.
            state.gpuKnown = false;
            if (background && !labActive && !governorGpu) gpu.reset();
            else if (gpu.available()) {
                std::unordered_map<uint32_t, double> usage;
                if (gpu.sample(usage)) {
                    publishGovernorGpu(gpu.instances(), now); nextGpu = now + 2000;
                    state.gpuKnown = true;
                    for (auto& row : state.processes) { auto found = usage.find(row.pid); row.gpuPercent = found != usage.end() ? found->second : 0.0; }
                }
            }
            state.ledger = ledger.top(15); state.ledgerCoreMilliWh = ledger.coreMilliWh(); state.ledgerStartedMs = ledger.startedMs();
            // Package power already includes the cores: never add it to core energy.
            if (state.energy.coreWatts && !state.history.empty()) {
                const auto& battery = state.history.back();
                if (battery.acKnown && !battery.onAc && !battery.charging && battery.watts && *battery.watts >= *state.energy.coreWatts)
                    state.energy.otherSystemWatts = *battery.watts - *state.energy.coreWatts;
            }
            Value systemCpu;
            if (state.energy.ready && !state.energy.timingMismatch) {
                double busy = 0; for (const auto& core : state.energy.cores) busy += core.busyLogicalCores;
                systemCpu = busy * 100 / std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
            }
            state.samplingSeconds = policy.update(background, now, systemCpu, state.energy.coreWatts);
            if (labActive) state.samplingSeconds = 2;
            nextProcess = now + state.samplingSeconds * 1000;
            if (background || labActive) nextPower = nextProcess;
            if (labActive) {
                if (!labProbe) labProbe = std::make_unique<bs::lab::Probe>();
                bs::lab::Sample sample; sample.ms = now;
                SYSTEMTIME t{}; GetLocalTime(&t);
                wchar_t stamp[32]; swprintf(stamp, 32, L"%04u-%02u-%02u %02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
                sample.time = stamp;
                if (!state.history.empty()) {
                    const auto& b = state.history.back();
                    sample.onBattery = b.present && b.acKnown && !b.onAc && !b.charging && b.watts && *b.watts > 0;
                    if (sample.onBattery) sample.batteryWatts = b.watts;
                }
                if (state.energy.ready && !state.energy.timingMismatch) { sample.packageWatts = state.energy.packageWatts; sample.coreWatts = state.energy.coreWatts; }
                sample.systemCpu = systemCpu;
                labProbe->read(sample, gpu.instances(), state.processes);
                publishPerf(sample, cpuClock, now);
                if (labEngine) sample.workload = labEngine->take();
                labController->onSample(sample, labLast && now > labLast ? (now - labLast) / 1000.0 : 0);
                labLast = now;
            } else {
                labLast = 0;
                // The performance page's readings, while the panel is open.
                if (!background) {
                    if (!labProbe) labProbe = std::make_unique<bs::lab::Probe>();
                    bs::lab::Sample sample; sample.ms = now; sample.systemCpu = systemCpu;
                    labProbe->read(sample, gpu.instances(), state.processes);
                    publishPerf(sample, cpuClock, now);
                }
            }
            changed = processSampled = true;
        }
        // The activity detector needs the GPU engines every 2 seconds, even while hidden.
        if (governorGpu && now >= nextGpu && gpu.available()) {
            std::unordered_map<uint32_t, double> usage;
            if (gpu.sample(usage)) publishGovernorGpu(gpu.instances(), now);
            nextGpu = now + 2000;
        }
        if (clearHistoryRequest.exchange(false)) {
            history.clear(); historyDirty = false;
            const std::wstring path = historyPath();
            if (!path.empty()) DeleteFileW(path.c_str());
            changed = true;
        }
        if (historyDirty && (now - historySavedAt >= 600000 || quit)) { saveHistory(history); historySavedAt = now; historyDirty = false; }
        if (changed) state.days = historyEnabled ? history.recent(14) : std::vector<DayStats>{};
        if (changed) {
            state.ms = now;
            {
                std::lock_guard<std::mutex> lock(mailboxMutex);
                // Keep an unconsumed process list when only power changed.
                std::vector<ProcessRow> processes;
                if (processSampled) processes = std::move(state.processes);
                else if (mailboxFresh && mailboxProcesses) processes = std::move(mailbox.processes);
                state.processes.clear();
                mailbox = state; // Copies only the small power history and energy report.
                mailbox.processes = std::move(processes);
                mailboxProcesses = processSampled || (mailboxFresh && mailboxProcesses);
                mailboxFresh = true;
            }
            PostMessageW(mainWindow, SampleMessage, 0, 0);
        }
        now = GetTickCount64(); uint64_t next = std::min(nextProcess, nextPower);
        if (governorGpu) next = std::min(next, std::max(nextGpu, now + 500));
        std::unique_lock<std::mutex> lock(wakeMutex);
        wake.wait_for(lock, std::chrono::milliseconds(next > now ? next - now : 1));
    }
    if (historyDirty && historyEnabled) saveHistory(history);
    if (labController) labController->shutdown(); // restores the setting under test
}

LRESULT CALLBACK listProc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (message == WM_MOUSEMOVE) {
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0}; TrackMouseEvent(&tracking);
        LVHITTESTINFO hit{}; hit.pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const int next = ListView_HitTest(list, &hit);
        if (next != hoveredRow) { hoveredRow = next; updateRowFeedback(); }
    }
    if (message == WM_MOUSELEAVE) { hoveredRow = -1; updateRowFeedback(); }
    if (message == WM_LBUTTONUP || message == WM_CAPTURECHANGED || message == WM_CANCELMODE) {
        auto result = DefSubclassProc(window, message, wp, lp); releaseInteraction(); return result;
    }
    if (message == WM_MOUSEWHEEL) { wheelScroll(wp); return 0; }
    if (message == WM_KEYDOWN) {
        if (wp == VK_RETURN) { PostMessageW(mainWindow, WM_COMMAND, DetailsId, 0); return 0; }
        int index = ListView_GetNextItem(list, -1, LVNI_SELECTED), next = index;
        RECT r{}; GetClientRect(viewport, &r);
        int page = std::max(1, int(r.bottom) / px(RowHeight));
        switch (wp) {
        case VK_UP: next = index < 0 ? 0 : index - 1; break;
        case VK_DOWN: next = index < 0 ? 0 : index + 1; break;
        case VK_HOME: next = 0; break;
        case VK_END: next = int(displayed.size()) - 1; break;
        case VK_PRIOR: next = index - page; break;
        case VK_NEXT: next = index + page; break;
        default: return DefSubclassProc(window, message, wp, lp);
        }
        if (displayed.empty()) return 0;
        next = std::clamp(next, 0, int(displayed.size()) - 1);
        stopScroll(); ListView_SetItemState(list, next, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        applyScroll(scrollPosition);
        int y = next * px(RowHeight);
        if (y < scrollPosition) scrollTo(y);
        else if (y + px(RowHeight) > scrollPosition + r.bottom) scrollTo(y + px(RowHeight) - r.bottom);
        return 0;
    }
    if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK) {
        pointerDown = true;
        stopScroll();
        LRESULT result = DefSubclassProc(window, message, wp, lp);
        applyScroll(scrollPosition); return result;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, listProc, 1);
    return DefSubclassProc(window, message, wp, lp);
}
LRESULT CALLBACK viewportProc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (message == WM_DRAWITEM || message == WM_MEASUREITEM || message == WM_NOTIFY)
        return SendMessageW(mainWindow, message, wp, lp);
    if (message == WM_MOUSEWHEEL) { wheelScroll(wp); return 0; }
    if (message == WM_ERASEBKGND) {
        RECT r{}; GetClientRect(window, &r); fill(reinterpret_cast<HDC>(wp), r, Background); return TRUE;
    }
    if (message == WM_VSCROLL) {
        RECT r{}; GetClientRect(window, &r);
        double target = scrollMotion.active(motionNow()) ? scrollMotion.to : scrollPosition;
        switch (LOWORD(wp)) {
        case SB_LINEUP: scrollTo(target - px(RowHeight)); break;
        case SB_LINEDOWN: scrollTo(target + px(RowHeight)); break;
        case SB_PAGEUP: scrollTo(target - r.bottom); break;
        case SB_PAGEDOWN: scrollTo(target + r.bottom); break;
        case SB_TOP: scrollTo(0); break;
        case SB_BOTTOM: scrollTo(scrollMaximum()); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: {
            SCROLLINFO info{}; info.cbSize = sizeof(info); info.fMask = SIF_TRACKPOS; GetScrollInfo(window, SB_VERT, &info);
            stopScroll(); scrollingThumb = true; scrollTo(info.nTrackPos, true); break;
        }
        case SB_ENDSCROLL:
            scrollingThumb = false;
            if (listRefreshPending) { listRefreshPending = false; refreshList(); }
            break;
        }
        return 0;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, viewportProc, 1);
    return DefSubclassProc(window, message, wp, lp);
}
LRESULT CALLBACK buttonProc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (message == WM_LBUTTONDOWN || (message == WM_KEYDOWN && wp == VK_SPACE)) {
        pointerDown = true;
        pressMotions[window].move(1, motionNow(), canAnimate() && !hidden ? 65 : 0);
        if (pressMotions[window].duration) armMotionTimer();
    }
    if (message == WM_LBUTTONUP || message == WM_CAPTURECHANGED || message == WM_CANCELMODE || (message == WM_KEYUP && wp == VK_SPACE)) {
        pressMotions[window].move(0, motionNow(), canAnimate() && !hidden ? 130 : 0);
        if (pressMotions[window].duration) armMotionTimer();
        auto result = DefSubclassProc(window, message, wp, lp); releaseInteraction(); return result;
    }
    if (message == WM_MOUSEMOVE || message == WM_MOUSELEAVE) {
        double target = message == WM_MOUSEMOVE ? 1 : 0;
        auto& motion = buttonMotions[window];
        if (motion.to != target) {
            motion.move(target, motionNow(), canAnimate() && !hidden ? 120 : 0);
            if (target) { TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0}; TrackMouseEvent(&tracking); }
            InvalidateRect(window, nullptr, FALSE);
            if (motion.duration) armMotionTimer();
        }
    }
    if (message == WM_ENABLE && !wp) { buttonMotions[window] = {}; pressMotions[window] = {}; }
    if (message == WM_NCDESTROY) { buttonMotions.erase(window); pressMotions.erase(window); RemoveWindowSubclass(window, buttonProc, 1); }
    return DefSubclassProc(window, message, wp, lp);
}
LRESULT CALLBACK inlineTextProc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (message == WM_KEYDOWN && wp == VK_ESCAPE) {
        PostMessageW(mainWindow, WM_COMMAND, InlineBackId, 0); return 0;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, inlineTextProc, 1);
    return DefSubclassProc(window, message, wp, lp);
}
// Update checks: once a day against the public GitHub release feed (only the
// version JSON is fetched), and a notification once per new version.
bool updateBusy = false, updateManual = false;
std::optional<bs::update::Release> availableUpdate;
const std::wstring UserAgent = L"BatteryScope/" BS_VERSION_STRING;
uint64_t unixNow() {
    FILETIME now{}; GetSystemTimeAsFileTime(&now);
    return (fileTime(now) - 116444736000000000ull) / 10000000ull;
}
void installUpdate();
void checkForUpdate(bool manual) {
    if (updateBusy || demo || isolatedPreview) return;
    updateBusy = true; updateManual = manual;
    HWND target = mainWindow;
    std::thread([target] {
        auto release = bs::update::fetchLatest(UserAgent);
        auto* result = release ? new bs::update::Release(std::move(*release)) : nullptr;
        if (!PostMessageW(target, UpdateMessage, 0, reinterpret_cast<LPARAM>(result))) delete result;
    }).detach();
}
void maybeCheckForUpdate() {
    if (!updateChecks) return;
    DWORD bytes = sizeof(uint64_t); uint64_t last = 0;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\BatteryScope", L"LastUpdateCheck", RRF_RT_REG_QWORD, nullptr, &last, &bytes);
    const uint64_t now = unixNow();
    if (last && now >= last && now - last < 86400) return;
    checkForUpdate(false);
}
void handleUpdateResult(bs::update::Release* raw) {
    std::unique_ptr<bs::update::Release> release(raw);
    updateBusy = false;
    if (!release) { if (updateManual) setNotice(tr(L"無法檢查更新，請確認網路連線。")); return; }
    const uint64_t now = unixNow();
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\BatteryScope", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(key, L"LastUpdateCheck", 0, REG_QWORD, reinterpret_cast<const BYTE*>(&now), sizeof(now));
        RegCloseKey(key);
    }
    if (!bs::update::isNewer(release->version, L"" BS_VERSION_STRING)) {
        availableUpdate.reset();
        if (updateManual) setNotice(tr(L"已是最新版本（") + std::wstring(L"" BS_VERSION_STRING) + tr(L"）。"));
        return;
    }
    availableUpdate = *release;
    std::wstring notified(32, L'\0'); DWORD bytes = DWORD(notified.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\BatteryScope", L"NotifiedVersion", RRF_RT_REG_SZ, nullptr, notified.data(), &bytes) == ERROR_SUCCESS)
        notified.resize(wcsnlen(notified.c_str(), notified.size()));
    else notified.clear();
    if (updateAuto && !updateManual) {
        // Automatic: install now, or right after a running test.
        if (autoTestActive || (labController && labController->status().running)) updateDeferred = true;
        else installUpdate();
        return;
    }
    if (updateManual) setNotice(tr(L"有新版本 ") + release->version + tr(L"，可從「⋯ → 安裝更新」安裝。"));
    else if (notified != release->version && trayAdded) {
        showBalloon(tr(L"BatteryScope ") + release->version + tr(L" 可以更新"),
            tr(L"點這裡下載並安裝。安裝後捷徑與工作列釘選會沿用同一個位置。"), BalloonTarget::update);
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\BatteryScope", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
            RegSetValueExW(key, L"NotifiedVersion", 0, REG_SZ, reinterpret_cast<const BYTE*>(release->version.c_str()),
                DWORD((release->version.size() + 1) * sizeof(wchar_t)));
            RegCloseKey(key);
        }
    }
}
void openReleasePage() {
    const std::wstring page = availableUpdate ? availableUpdate->page : std::wstring(bs::update::ReleasesPage);
    ShellExecuteW(nullptr, L"open", page.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
// Downloads the setup, checks its SHA-256 against the published file, and runs it.
// The setup closes this copy, installs over the same folder, and starts it again.
std::wstring pendingSetup;
void installUpdate() {
    if (!availableUpdate || updateBusy) return;
    if (availableUpdate->setupUrl.empty() || availableUpdate->setupHashUrl.empty()) { openReleasePage(); return; }
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    pendingSetup = std::wstring(temp) + L"BatteryScope-" + availableUpdate->version + L"-setup.exe";
    updateBusy = true;
    setNotice(tr(L"正在下載更新 ") + availableUpdate->version + L"…");
    HWND target = mainWindow; auto release = *availableUpdate; auto path = pendingSetup;
    std::thread([target, release, path] {
        PostMessageW(target, UpdateDownloadedMessage, bs::update::downloadVerified(release, path, UserAgent), 0);
    }).detach();
}
void finishUpdateDownload(DWORD status) {
    updateBusy = false;
    if (status != ERROR_SUCCESS) {
        setNotice(status == ERROR_INVALID_DATA ? tr(L"更新檔校驗不符，已刪除並改開啟下載頁。") : tr(L"下載更新失敗，已改開啟下載頁。"));
        DeleteFileW(pendingSetup.c_str());
        openReleasePage();
        return;
    }
    SHELLEXECUTEINFOW run{}; run.cbSize = sizeof(run); run.fMask = SEE_MASK_NOASYNC;
    run.lpFile = pendingSetup.c_str(); run.lpParameters = L"/SILENT /SUPPRESSMSGBOXES /NORESTART"; run.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&run)) { setNotice(tr(L"無法啟動安裝程式，已改開啟下載頁。")); openReleasePage(); }
    else setNotice(tr(L"正在安裝更新，BatteryScope 會自動重新開啟。"));
}
// ----- Dynamic power saving -----
// The governor runs on a one-second timer on the UI thread from cheap signals
// (input, sound, calls, full screen) and the latest sample, and writes power
// settings through the lab's actuators. The originals of what it changed are
// kept in the registry until restored, so a crash is undone at the next start.
bs::gov::Governor governor;
std::unique_ptr<bs::lab::SignalReader> signalReader;
uint64_t evidenceAt = 0;
int topRefreshSeen = 0;
constexpr wchar_t GovernorKey[] = L"Software\\BatteryScope\\Governor";
void saveGovernorOriginals() {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, GovernorKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    for (size_t i = 0; i < governor.originals().size(); ++i) {
        const std::wstring name = L"Original" + std::to_wstring(i);
        if (const auto& value = governor.originals()[i]) { const DWORD v = *value; RegSetValueExW(key, name.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v)); }
    }
    RegCloseKey(key);
}
void forgetGovernorOriginals() { RegDeleteTreeW(HKEY_CURRENT_USER, GovernorKey); }
// After a crash or power loss: put back what the governor had changed.
void restoreGovernorAfterCrash() {
    for (size_t i = 0; i < size_t(bs::gov::Setting::count); ++i) {
        DWORD value = 0, size = sizeof(value);
        const std::wstring name = L"Original" + std::to_wstring(i);
        if (RegGetValueW(HKEY_CURRENT_USER, GovernorKey, name.c_str(), RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS)
            bs::lab::applyKnob(bs::gov::knobFor(bs::gov::Setting(i)), value);
    }
    forgetGovernorOriginals();
}
void applyDecision(const bs::gov::Decision& d) {
    bool changed = false;
    for (size_t i = 0; i < d.apply.size(); ++i) if (d.apply[i]) { bs::lab::applyKnob(bs::gov::knobFor(bs::gov::Setting(i)), *d.apply[i]); changed = true; }
    for (const auto& [pid, created] : d.ecoOn) bs::lab::setProcessEco(pid, created, true);
    for (const auto& [pid, created] : d.ecoOff) bs::lab::setProcessEco(pid, created, false);
    if (d.restore) forgetGovernorOriginals();
    else if (changed) saveGovernorOriginals();
}
void stopGovernor() {
    bs::gov::Input off; off.ms = GetTickCount64(); off.onBattery = false;
    applyDecision(governor.update(off));
}
// ----- Activity sensing -----
// Raw keyboard and mouse input (timing and kind only; key codes are never read),
// foreground switches and the display state arrive as events, so a boost does not
// wait for the next one-second tick. Sensing runs while dynamic saving is on, and
// during the automatic test, where detection is scored against the known activity.
bs::gov::Assessment lastAssessment;
std::string detectionLog;                 // CSV lines written during the automatic test
uint64_t lastActiveInputMs = 0, lastPointerMs = 0, lastRawInputMs = 0, lastImmediateMs = 0, lastTallyMs = 0;
bs::gov::InputKind lastActiveKind = bs::gov::InputKind::none;
bool sensing = false, rawInputOn = false, displayOff = false;
HWINEVENTHOOK foregroundHook = nullptr;
HPOWERNOTIFY displayNotify = nullptr;
std::map<std::wstring, bs::gov::AppRule> appRules;   // the user's own rules, lower-case exe -> rule
struct Learned { DWORD up = 0, down = 0; };
std::map<std::wstring, Learned> appLearned;           // user overrides seen per program
std::map<std::wstring, uint64_t> learnedAt;
constexpr wchar_t AppRulesKey[] = L"Software\\BatteryScope\\AppRules";
constexpr wchar_t AppLearnedKey[] = L"Software\\BatteryScope\\AppLearned";
bs::gov::AppRule learnedRule(const Learned& l) {
    if (l.up >= 2 && l.up > l.down) return bs::gov::AppRule::performance;
    if (l.down >= 2 && l.down > l.up) return bs::gov::AppRule::saving;
    return bs::gov::AppRule::automatic;
}
void loadAppRules() {
    appRules.clear(); appLearned.clear();
    for (const wchar_t* path : {AppRulesKey, AppLearnedKey}) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
        for (DWORD i = 0;; ++i) {
            wchar_t name[260]; DWORD length = 260, type = 0, value = 0, size = sizeof(value);
            if (RegEnumValueW(key, i, name, &length, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) != ERROR_SUCCESS) break;
            if (type != REG_DWORD) continue;
            if (path == AppRulesKey) { if (value > 0 && value < DWORD(bs::gov::AppRule::count)) appRules[name] = bs::gov::AppRule(value); }
            else appLearned[name] = {value & 0xffff, value >> 16};
        }
        RegCloseKey(key);
    }
}
bool writeNames(const wchar_t* name, const std::set<std::wstring>& values);
void applyAutoPowerRules();
// "Performance first" is the performance list (it also switches the power mode to
// performance while the program runs); every other rule lives under AppRules.
void saveAppRule(const std::wstring& exe, bs::gov::AppRule rule) {
    const bool wasListed = performanceApps.erase(exe) > 0;
    if (rule == bs::gov::AppRule::performance) {
        performanceApps.insert(exe); writeNames(L"PerformanceApps", performanceApps);
        rule = bs::gov::AppRule::automatic; // and no separate AppRules entry
    } else if (wasListed) writeNames(L"PerformanceApps", performanceApps);
    applyAutoPowerRules();
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, AppRulesKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    if (rule == bs::gov::AppRule::automatic) { RegDeleteValueW(key, exe.c_str()); appRules.erase(exe); }
    else { const DWORD v = DWORD(rule); RegSetValueExW(key, exe.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v)); appRules[exe] = rule; }
    RegCloseKey(key);
}
void saveLearned(const std::wstring& exe) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, AppLearnedKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    auto it = appLearned.find(exe);
    if (it == appLearned.end()) RegDeleteValueW(key, exe.c_str());
    else { const DWORD v = std::min<DWORD>(it->second.up, 0xffff) | (std::min<DWORD>(it->second.down, 0xffff) << 16); RegSetValueExW(key, exe.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v)); }
    RegCloseKey(key);
}
void refreshDetectorRules() {
    std::map<std::wstring, bs::gov::AppRule> rules = appRules, learned;
    // The performance list (automatic power mode rules) counts as performance rules.
    for (const auto& name : performanceApps) rules.emplace(name, bs::gov::AppRule::performance);
    for (const auto& [exe, l] : appLearned) if (auto rule = learnedRule(l); rule != bs::gov::AppRule::automatic) learned[exe] = rule;
    detector.setRules(std::move(rules), std::move(learned));
}
// The user (or G-Helper) moved a setting away from the governor's value while this
// program was in front: two moves the same way teach a rule for the program.
void learnOverride(const std::wstring& exe, int way, uint64_t now) {
    if (exe.empty() || !way || appRules.count(exe)) return;
    auto& at = learnedAt[exe];
    if (at && now - at < 600000) return; // one lesson per program per 10 minutes
    at = now;
    auto& l = appLearned[exe];
    const auto before = learnedRule(l);
    (way > 0 ? l.up : l.down) += 1;
    saveLearned(exe); refreshDetectorRules();
    if (const auto after = learnedRule(l); after != before && after != bs::gov::AppRule::automatic)
        setNotice(exe + tr(L"：學到了，之後這個程式改用「") + tr(bs::gov::ruleKey(after)) + tr(L"」。可在「設定 → 程式規則」修改。"));
}
void onRawInput(const RAWINPUT& raw) {
    using bs::gov::InputKind;
    const uint64_t now = GetTickCount64();
    lastRawInputMs = now;
    InputKind kind = InputKind::pointer;
    if (raw.header.dwType == RIM_TYPEKEYBOARD) {
        if (raw.data.keyboard.Flags & RI_KEY_BREAK) return; // key release
        kind = InputKind::typing;
    } else if (raw.header.dwType == RIM_TYPEMOUSE) {
        const USHORT flags = raw.data.mouse.usButtonFlags;
        if (flags & (RI_MOUSE_WHEEL | RI_MOUSE_HWHEEL)) kind = InputKind::scroll;
        else if (flags & (RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_DOWN | RI_MOUSE_MIDDLE_BUTTON_DOWN | RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_5_DOWN)) kind = InputKind::click;
    } else return;
    if (kind == InputKind::pointer) { lastPointerMs = now; return; } // the common case, kept cheap
    lastActiveInputMs = now; lastActiveKind = kind;
    // Boost now instead of at the next tick, at most every 150 ms.
    if (dynamicEnabled && bs::gov::rank(governor.activity()) < bs::gov::rank(bs::gov::Activity::responsive) && now - lastImmediateMs >= 150) {
        lastImmediateMs = now; PostMessageW(mainWindow, GovernorNowMessage, 0, 0);
    }
}
void CALLBACK foregroundChanged(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    const uint64_t now = GetTickCount64();
    if (now - lastImmediateMs >= 150) { lastImmediateMs = now; PostMessageW(mainWindow, GovernorNowMessage, 0, 0); }
}
bool planRunning() {
    if (!labController) return false;
    const auto st = labController->status();
    return st.running && st.plan;
}
// Starts or stops sensing to match what needs it.
void updateSensing() {
    const bool want = (dynamicEnabled || planRunning()) && !demo && !isolatedPreview && mainWindow;
    if (want == sensing) return;
    sensing = want; governorGpu = want;
    if (want) {
        RAWINPUTDEVICE devices[2] = {{0x01, 0x06, RIDEV_INPUTSINK, mainWindow}, {0x01, 0x02, RIDEV_INPUTSINK, mainWindow}};
        rawInputOn = RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE)) != FALSE;
        foregroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, foregroundChanged, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        constexpr GUID ConsoleDisplayState = {0x6FE69556, 0x704A, 0x47A0, {0x8F, 0x24, 0xC2, 0x8D, 0x93, 0x6F, 0xDA, 0x47}};
        displayNotify = RegisterPowerSettingNotification(mainWindow, &ConsoleDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);
        SetTimer(mainWindow, GovernorTimer, 1000, nullptr);
        wake.notify_one();
    } else {
        if (rawInputOn) {
            RAWINPUTDEVICE devices[2] = {{0x01, 0x06, RIDEV_REMOVE, nullptr}, {0x01, 0x02, RIDEV_REMOVE, nullptr}};
            RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE));
        }
        rawInputOn = false;
        if (foregroundHook) { UnhookWinEvent(foregroundHook); foregroundHook = nullptr; }
        if (displayNotify) { UnregisterPowerSettingNotification(displayNotify); displayNotify = nullptr; }
        KillTimer(mainWindow, GovernorTimer);
    }
}
std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    std::string out(size_t(WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0, nullptr, nullptr)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), out.data(), int(out.size()), nullptr, nullptr);
    return out;
}
constexpr char DetectionHeader[] = "\xEF\xBB\xBFtime,workload,activity,confidence,reason,foreground,category,full_screen,idle_s,active_s,fps,cpu,gpu_3d,video_decode,"
    "audio,foreground_audio,call,locked,compositor_gpu,foreground_3d,foreground_decode,busiest_cores,busiest\r\n";
std::set<std::wstring> gpuEngineTypes; // every GPU engine type seen, for gpu-engines.txt
std::wstring busiestProgram;           // the program behind Observation::busiestCores
void flushDetectionLog() {
    if (detectionLog.empty()) return;
    const std::wstring folder = bs::lab::labFolder();
    if (folder.empty()) return;
    const auto path = std::filesystem::path(folder) / L"detection-log.csv";
    std::error_code error;
    if (std::filesystem::exists(path, error)) {
        // A log from an older version has other columns: keep it beside the new one.
        std::ifstream in(path, std::ios::binary); std::string first; std::getline(in, first); in.close();
        if (first + "\n" != DetectionHeader) std::filesystem::rename(path, std::filesystem::path(folder) / L"detection-log-old.csv", error);
    }
    const bool fresh = !std::filesystem::exists(path, error);
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (fresh) out << DetectionHeader;
    out << detectionLog;
    detectionLog.clear();
    // The GPU engine types this PC reports, to check which ones count as video decoding.
    if (!gpuEngineTypes.empty()) {
        std::ofstream types(std::filesystem::path(folder) / L"gpu-engines.txt", std::ios::binary | std::ios::trunc);
        for (const auto& type : gpuEngineTypes) types << utf8(type) << (bs::gov::decodeEngine(type) ? "  (video decoding)" : "") << "\r\n";
    }
}
void governorTick() {
    if (demo || isolatedPreview) return;
    const bool testing = planRunning();
    if (!dynamicEnabled && !testing) { updateSensing(); return; }
    if (!signalReader) signalReader = std::make_unique<bs::lab::SignalReader>();
    const uint64_t now = GetTickCount64();
    Value systemCpu;
    if (current.energy.ready) {
        double busy = 0; for (const auto& core : current.energy.cores) busy += core.busyLogicalCores;
        systemCpu = busy * 100 / std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    }
    // Detection: the reader's signals plus raw input, the display and the GPU.
    auto o = signalReader->observe(systemCpu);
    if (rawInputOn) {
        // Input Windows saw but raw input did not (e.g. some touch input) counts as active.
        const uint64_t lastInput = now - uint64_t(o.idleSeconds * 1000);
        const uint64_t active = lastInput > lastRawInputMs + 500 ? lastInput : lastActiveInputMs;
        o.activeSeconds = active ? (now - active) / 1000.0 : 1e6;
        o.lastInput = o.activeSeconds && *o.activeSeconds < bs::gov::Detector::ResponsiveSeconds ? lastActiveKind
            : lastPointerMs && now - lastPointerMs < 10000 ? bs::gov::InputKind::pointer : lastActiveKind;
    }
    o.displayOff = displayOff;
    {
        std::lock_guard<std::mutex> lock(governorGpuMutex);
        if (governorGpuAt && now - governorGpuAt < 10000) o.gpu = governorGpuLatest;
    }
    if (o.gpu.known) {
        // The compositor's GPU use shows whether anything on screen is moving.
        for (const auto& row : current.processes) if (_wcsicmp(row.name.c_str(), L"dwm.exe") == 0) {
            auto it = o.gpu.processes.find(row.pid);
            o.compositorGpu = it != o.gpu.processes.end() ? it->second.engine3d : 0.0;
            break;
        }
        // The foreground program's GPU use over all its processes: browsers draw and decode
        // video in a separate GPU process, not in the one that owns the window.
        if (!o.foregroundExe.empty())
            for (const auto& row : current.processes) {
                if (_wcsicmp(row.name.c_str(), o.foregroundExe.c_str()) != 0) continue;
                auto it = o.gpu.processes.find(row.pid);
                if (it == o.gpu.processes.end()) continue;
                o.foregroundGpu.engine3d = std::min(100.0, o.foregroundGpu.engine3d + it->second.engine3d);
                o.foregroundGpu.decode = std::min(100.0, o.foregroundGpu.decode + it->second.decode);
                o.foregroundGpu.compute = std::min(100.0, o.foregroundGpu.compute + it->second.compute);
            }
        for (const auto& type : o.gpu.engineTypes) gpuEngineTypes.insert(type);
    }
    // The busiest program, in logical processors: heavy work on a few threads is a small share
    // of 16. Services and Windows' own maintenance (updates, indexing, scans) do not count.
    {
        static const wchar_t* const maintenance[] = {L"tiworker.exe", L"trustedinstaller.exe", L"searchprotocolhost.exe", L"searchfilterhost.exe",
            L"compattelrunner.exe", L"mousocoreworker.exe", L"msmpeng.exe", L"mpdefendercoreservice.exe", L"system", L"registry", L"memory compression", L"wmiprvse.exe"};
        const double processors = double(std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));
        double busiest = 0; std::wstring busiestName;
        for (const auto& row : current.processes) {
            if (!row.measured || !row.services.empty()) continue;
            if (std::any_of(std::begin(maintenance), std::end(maintenance), [&](const wchar_t* name) { return _wcsicmp(row.name.c_str(), name) == 0; })) continue;
            const double cores = row.cpu * processors / 100;
            if (cores > busiest) { busiest = cores; busiestName = row.name; }
        }
        if (!current.processes.empty()) { o.busiestCores = busiest; busiestProgram = busiestName; }
    }
    lastAssessment = detector.assess(o);
    if (testing) {
        // The automatic test knows what it is running: score the detection against it.
        const auto st = labController->status();
        const double seconds = lastTallyMs ? std::min(5.0, (now - lastTallyMs) / 1000.0) : 1;
        lastTallyMs = now;
        const std::wstring workload = bs::lab::workloadKey(st.workload);
        if (!st.paused && st.knob != bs::lab::Knob::display) {
            detectionTally.add(workload, lastAssessment.activity, seconds);
            const auto& g = o.gpu.total;
            wchar_t line[768];
            swprintf(line, 768, L"%llu,%ls,%ls,%.2f,\"%ls\",%ls,%ls,%d,%.0f,%.0f,%.1f,%.0f,%.0f,%.0f,%d,%d,%d,%d,%.1f,%.1f,%.1f,%.2f,%ls\r\n",
                static_cast<unsigned long long>(now), workload.c_str(), bs::gov::activityKey(lastAssessment.activity), lastAssessment.confidence,
                lastAssessment.reason.c_str(), o.foregroundExe.c_str(), bs::gov::categoryKey(lastAssessment.category), int(o.foregroundCoversScreen),
                o.idleSeconds, o.activeSeconds.value_or(-1), o.framesPerSecond.value_or(-1), o.systemCpu.value_or(-1), g.engine3d, g.decode,
                int(o.audio), int(o.foregroundAudio), int(o.call), int(o.locked), o.compositorGpu.value_or(-1),
                o.foregroundGpu.engine3d, o.foregroundGpu.decode, o.busiestCores.value_or(-1), busiestProgram.c_str());
            detectionLog += utf8(line);
            if (detectionLog.size() > 64 * 1024) flushDetectionLog();
        }
    } else lastTallyMs = 0;
    if (!dynamicEnabled) return;
    bs::gov::Options options;
    options.refreshRate = dynamicRefresh; options.backgroundEco = dynamicEco;
    options.lowBattery = autoSaverThreshold ? autoSaverThreshold : 20; // saving harder below the low-battery level
    // The power goal also sets how hard dynamic saving works: when the battery drains faster
    // than the goal, each activity uses the next more saving profile. (Hours the remaining
    // energy lasts at the goal, against hours it lasts at the current rate.)
    if (goalUse && !current.history.empty() && current.history.back().remainingWh)
        options.targetHours = *current.history.back().remainingWh / double(goalUse);
    DWORD refresh = 0;
    if (bs::lab::readKnob(bs::lab::Knob::refreshRate, refresh) == ERROR_SUCCESS) topRefreshSeen = std::max(topRefreshSeen, int(refresh));
    options.topRefresh = std::max(60, topRefreshSeen);
    governor.setOptions(options);
    if (labController && (!evidenceAt || now - evidenceAt >= 60000)) { governor.setEvidence(bs::gov::learn(labController->status().results)); evidenceAt = now; }
    bs::gov::Input in; in.ms = now;
    if (!current.history.empty()) {
        const auto& p = current.history.back();
        in.onBattery = p.present && p.acKnown && !p.onAc && !p.charging;
    }
    if (labController && labController->status().running) in.onBattery = false; // the lab owns the settings while it measures
    in.context = bs::lab::classify({o.idleSeconds, o.audio, o.call, o.exclusiveFullscreen, systemCpu});
    in.idleSeconds = o.idleSeconds;
    in.detected = lastAssessment.activity; in.confidence = lastAssessment.confidence;
    in.hoursLeft = current.power.hoursRemaining;
    if (!current.history.empty()) in.batteryPercent = current.history.back().percent;
    for (size_t i = 0; i < in.actual.size(); ++i) {
        DWORD value = 0;
        if (bs::lab::readKnob(bs::gov::knobFor(bs::gov::Setting(i)), value) == ERROR_SUCCESS) in.actual[i] = value;
    }
    DWORD foreground = 0;
    if (HWND window = GetForegroundWindow()) GetWindowThreadProcessId(window, &foreground);
    for (const auto& row : current.processes)
        if (row.measured && row.actionable && row.services.empty() && row.pid != GetCurrentProcessId() &&
            std::none_of(efficientProcesses.begin(), efficientProcesses.end(), [&](const EfficientProcess& e) { return e.pid == row.pid && e.created == row.created; }))
            in.apps.push_back({row.pid, row.created, row.name, row.averageCpu, row.pid == foreground || row.pid == o.foregroundPid});
    const auto decision = governor.update(in);
    applyDecision(decision);
    {
        // The header pill follows the activity.
        static bs::gov::Activity shown = bs::gov::Activity::count;
        if (governor.activity() != shown) { shown = governor.activity(); refreshPowerButton(); }
    }
    int way = 0; for (const auto& [setting, direction] : decision.overrides) way += direction;
    if (way) learnOverride(o.foregroundExe, way > 0 ? 1 : -1, now);
    if (inlinePage == InlinePage::goal && !hidden && now - goalPageShownAt >= 3000) showGoalPage();
}
void setDynamic(bool on) {
    dynamicEnabled = on;
    if (!writeSetting(L"Dynamic", on ? 1 : 0)) setNotice(tr(L"無法儲存設定。"));
    updateSensing();
    if (on) { applyAutoPowerRules(); governorTick(); setNotice(tr(L"動態省電已開啟：用電池時依你正在做的事自動調整。")); }
    else { stopGovernor(); governor.releaseAll(); applyAutoPowerRules(); setNotice(tr(L"動態省電已關閉，設定已還原。")); }
}
bool powerModeHeld() { return dynamicEnabled && governor.held(bs::gov::Setting::powerMode); }
std::wstring dynamicPill() {
    if (!dynamicEnabled || current.history.empty() || current.history.back().onAc) return {};
    const auto a = governor.activity();
    const wchar_t* name = a == bs::gov::Activity::idle ? L"閒置" : a == bs::gov::Activity::reading ? L"閱讀" : a == bs::gov::Activity::media ? L"影片" :
        a == bs::gov::Activity::call ? L"通話" : a == bs::gov::Activity::heavy ? L"高負載" : a == bs::gov::Activity::game ? L"遊戲" :
        a == bs::gov::Activity::burst ? L"載入" : a == bs::gov::Activity::exempt ? L"不管理" : L"操作";
    return tr(L"動態 · ") + std::wstring(tr(name));
}
size_t heldCount() {
    size_t n = 0;
    for (size_t i = 0; i < size_t(bs::gov::Setting::count); ++i) n += governor.held(bs::gov::Setting(i));
    return n;
}
std::wstring settingValueName(bs::gov::Setting setting, DWORD value) {
    switch (setting) {
    case bs::gov::Setting::powerMode: return modeName(PowerMode(value));
    case bs::gov::Setting::cpuBoost: return value ? tr(L"開") : tr(L"關");
    case bs::gov::Setting::refreshRate: return std::to_wstring(value) + L" Hz";
    case bs::gov::Setting::firmwareMode: return value <= 2 ? tr(bs::asus::modeKey(bs::asus::Mode(value))) : std::to_wstring(value);
    default: return std::to_wstring(value);
    }
}
std::wstring dynamicText() {
    const wchar_t* names[] = {tr(L"電源模式"), tr(L"處理器能源偏好"), tr(L"CPU Boost"), tr(L"螢幕更新率"), tr(L"華碩效能模式")};
    std::wstring c = tr(L"用電池時，依你正在做的事自動調整電源設定：閒置與閱讀時省電，一操作就立刻恢復反應速度；高負載時給效能。插電、全螢幕遊戲或關閉時還原原本設定。G-Helper 或你自己改了某項設定，那一項會暫停管理 10 分鐘。");
    c += tr(L"\n\n狀態：") + std::wstring(dynamicEnabled ? tr(L"開啟") : tr(L"關閉"));
    if (dynamicEnabled) {
        c += tr(L"\n目前判斷：") + std::wstring(tr(bs::gov::activityKey(governor.activity())));
        const auto& a = lastAssessment;
        auto reasonText = [](const std::wstring& reason) {
            const std::wstring rule = L" · 程式規則：";
            const size_t at = reason.find(rule);
            if (at == std::wstring::npos) return std::wstring(tr(reason.c_str()));
            return tr(reason.substr(0, at).c_str()) + std::wstring(tr(rule.c_str())) + tr(reason.substr(at + rule.size()).c_str());
        };
        c += tr(L"\n偵測結果：") + std::wstring(tr(bs::gov::activityKey(a.activity))) + tr(L"（把握 ") + number(a.confidence * 100, 0) + (english ? L"%) · " : L"%）· ") + reasonText(a.reason);
        if (bs::gov::rank(a.activity) < bs::gov::rank(governor.activity())) c += tr(L"\n（等狀態穩定幾秒後才會切到較省電的設定）");
        std::wstring others;
        for (const auto& k : a.candidates) {
            if (k.activity == a.activity || others.size() > 200) continue;
            others += (others.empty() ? L"" : L"、") + std::wstring(tr(bs::gov::activityKey(k.activity))) + L" " + number(k.confidence * 100, 0) + L"%";
        }
        if (!others.empty()) c += tr(L"\n其他可能：") + others;
        if (signalReader && !signalReader->lastForeground().empty()) {
            c += tr(L"\n前景程式：") + signalReader->lastForeground() + L"（" + tr(bs::gov::categoryKey(a.category)) + L"）· " + tr(L"規則：") + tr(bs::gov::ruleKey(a.rule));
            if (a.learnedRule) c += tr(L"（學到的）");
        }
        c += tr(L"\n偵測方式：") + std::wstring(rawInputOn ? tr(L"鍵盤滑鼠即時偵測") : tr(L"每秒檢查")) + tr(L" · 前景程式切換即時偵測 · 顯示卡每 2 秒");
        const auto profile = governor.profileFor(governor.activity(), false);
        c += tr(L"\n目標設定：");
        bool any = false;
        for (size_t i = 0; i < profile.values.size(); ++i) if (profile.values[i]) {
            c += std::wstring(any ? L"，" : L"") + names[i] + L" " + settingValueName(bs::gov::Setting(i), *profile.values[i]);
            any = true;
        }
        if (!any) c += tr(L"維持原本設定");
        const uint64_t now = GetTickCount64();
        for (size_t i = 0; i < profile.values.size(); ++i)
            if (!governor.managing(bs::gov::Setting(i), now)) c += L"\n" + std::wstring(names[i]) + tr(L"：被其他程式改動，暫停管理中");
    }
    c += tr(L"\n\n只讀取時間、程式名稱和裝置使用狀態，不截圖、不讀畫面或文字內容，也不記錄按了哪些鍵。");
    c += tr(L"\n\n選項：切換螢幕更新率 ") + std::wstring(dynamicRefresh ? tr(L"開") : tr(L"關")) + tr(L" · 背景程式效率模式 ") +
        std::wstring(dynamicEco ? tr(L"開") : tr(L"關")) + tr(L" · 續航目標 ") + (dynamicTarget ? std::to_wstring(dynamicTarget) + tr(L" 小時") : std::wstring(tr(L"無")));
    const auto evidence = labController ? bs::gov::learn(labController->status().results) : std::vector<bs::gov::Evidence>{};
    c += tr(L"\n\n從省電實驗室學到的");
    if (evidence.empty()) c += tr(L"\n還沒有足夠的實驗結果：先執行一次「一鍵自動測試」，之後只採用在這台電腦上證實省電又不變卡的設定。");
    for (const auto& e : evidence)
        c += L"\n" + std::wstring(tr(bs::gov::activityKey(e.activity))) + L" · " + names[size_t(e.setting)] + L" " + settingValueName(e.setting, e.savingValue) +
            tr(L"：省 ") + number(e.wattsSaved, 2) + L" W" + (e.approved ? tr(L"，採用") : tr(L"，有代價，不採用"));
    if (!governor.log.empty()) {
        c += tr(L"\n\n最近的調整");
        for (auto it = governor.log.rbegin(); it != governor.log.rend(); ++it) {
            // Entries are Chinese; known prefixes are translated, app names stay as they are.
            std::wstring line = *it;
            for (const wchar_t* prefix : {L"背景程式改為效率模式：", L"→ "})
                if (line.rfind(prefix, 0) == 0) { line = std::wstring(tr(prefix)) + line.substr(wcslen(prefix)); break; }
            if (line.rfind(L"→ ", 0) != 0 && line.find(L"：") == std::wstring::npos) line = tr(line.c_str());
            c += L"\n" + line;
        }
    }
    return c;
}
// Dynamic saving's status is part of the power overview.
void showDynamicPage() { showGoalPage(); }

// ----- Power goal -----
// Battery power in use and while still over the last 30 minutes, against the
// user's goal, with the changes that would close the gap ranked by what the
// lab measured on this PC. "Still" means no keyboard or mouse input for a minute
// (or the detector saw idle); automatic test time is left out.
struct GoalSample { uint64_t ms = 0; double watts = 0; bool still = false; };
std::deque<GoalSample> goalSamples;
uint64_t goalLastPowerMs = 0;
void recordGoalSample() {
    if (current.history.empty()) return;
    const auto& p = current.history.back();
    if (p.ms == goalLastPowerMs) return;
    goalLastPowerMs = p.ms;
    if (!p.present || !p.acKnown || p.onAc || p.charging || !p.watts || *p.watts <= 0 || planRunning() || demo || isolatedPreview) return;
    LASTINPUTINFO input{}; input.cbSize = sizeof(input);
    const double idle = GetLastInputInfo(&input) ? (GetTickCount() - input.dwTime) / 1000.0 : 0;
    const bool still = idle >= 60 || (sensing && lastAssessment.activity == bs::gov::Activity::idle);
    goalSamples.push_back({p.ms, *p.watts, still});
    while (!goalSamples.empty() && p.ms - goalSamples.front().ms > 30 * 60000) goalSamples.pop_front();
}
struct GoalAverage { double watts = 0, minutes = 0; };
GoalAverage goalAverage(bool still) {
    // Time-weighted: each sample counts until the next one (at most 30 s).
    GoalAverage a; double energy = 0, seconds = 0;
    for (size_t i = 0; i < goalSamples.size(); ++i) {
        if (goalSamples[i].still != still) continue;
        const double span = i + 1 < goalSamples.size() ? std::min(30.0, (goalSamples[i + 1].ms - goalSamples[i].ms) / 1000.0) : 6.0;
        energy += goalSamples[i].watts * span; seconds += span;
    }
    if (seconds > 0) { a.watts = energy / seconds; a.minutes = seconds / 60; }
    return a;
}
// What the lab measured for a setting: the smallest and largest clear saving over its activities.
struct Measured { double low = 0, high = 0; int count = 0; std::wstring arms; };
Measured measuredSaving(const wchar_t* knob) {
    Measured m;
    if (!labController) return m;
    const std::wstring prefix = knob;
    for (const auto& r : labController->status().results) {
        if (r.experiment.compare(0, prefix.size(), prefix) != 0 || (r.experiment.size() > prefix.size() && r.experiment[prefix.size()] != L'@')) continue;
        if (r.arm0.empty() || r.arm1.empty() || r.battery.pairs < 2) continue;
        const bool coreClear = r.core.significant() && r.core.difference * r.battery.difference > 0;
        if (!r.battery.significant() && !coreClear) continue;
        const double saving = r.battery.significant() ? std::abs(r.battery.difference) : std::abs(r.core.difference);
        if (!m.count) { m.low = m.high = saving; m.arms = r.battery.difference < 0 ? labArm(r.arm1) : labArm(r.arm0); }
        m.low = std::min(m.low, saving); m.high = std::max(m.high, saving); ++m.count;
    }
    return m;
}
// The ways to save, ranked by watts. Measured ones come from the lab on this PC.
struct Tip { double watts = 0; std::wstring label, detail; bool measured = false; };
// What dynamic saving already takes care of, so the tips never suggest it twice.
std::wstring handledByDynamic() {
    std::wstring list = tr(L"電源模式、CPU Boost");
    if (asusAvailable) list += tr(L"、華碩效能模式");
    if (dynamicRefresh) list += tr(L"、螢幕更新率");
    if (dynamicEco) list += tr(L"、背景程式效率模式");
    return list;
}
std::vector<Tip> savingTips() {
    std::vector<Tip> tips;
    auto saving = [](const wchar_t* knob, double guess, bool& measured) {
        const auto m = measuredSaving(knob);
        measured = m.count > 0;
        return m.count ? m.high : guess;
    };
    auto measuredTip = [&](const wchar_t* knob, const std::wstring& label, const std::wstring& detail, double guess) {
        bool measured = false; const double watts = saving(knob, guess, measured);
        tips.push_back({watts, label, detail, measured});
    };
    const auto modes = queryPowerModes();
    DWORD value = 0;
    const auto* hw = asusAvailable ? &asusNow() : nullptr;
    if (!dynamicEnabled) {
        // The settings dynamic saving would manage, as one suggestion instead of several.
        double watts = 0; bool allMeasured = true, any = false; std::wstring what;
        auto add = [&](const wchar_t* knob, double guess, const wchar_t* name) {
            bool m = false; watts += saving(knob, guess, m); allMeasured &= m; any = true;
            what += (what.empty() ? L"" : std::wstring(tr(L"、"))) + tr(name);
        };
        if (modes.dcKnown && modes.dc != PowerMode::efficiency) add(L"power-mode", 1.5, L"電源模式");
        if (bs::lab::readKnob(bs::lab::Knob::cpuBoost, value) == ERROR_SUCCESS && value != 0) add(L"cpu-boost", 3, L"CPU Boost");
        if (hw && hw->mode && *hw->mode != bs::asus::Mode::silent) add(L"asus-mode", .8, L"華碩效能模式");
        if (any) tips.push_back({watts, tr(L"開啟動態省電"), tr(L"自動處理") + what + tr(L"：平常省電，需要時才給效能（⋯ → 動態省電）"), allMeasured});
    } else if (!dynamicEco) measuredTip(L"background-eco", tr(L"開啟背景程式效率模式"), tr(L"設定 → 背景程式自動效率模式"), .4);
    if (bs::lab::readKnob(bs::lab::Knob::refreshRate, value) == ERROR_SUCCESS && value > 60 && !(dynamicEnabled && dynamicRefresh))
        measuredTip(L"refresh", tr(L"螢幕改 60 Hz"), dynamicEnabled ? tr(L"或在設定開啟「動態切換螢幕更新率」，只在閱讀與閒置時用 60 Hz") :
            tr(L"Windows 設定 → 顯示器 → 進階顯示 · 目前：") + std::to_wstring(value) + L" Hz", .7);
    if (hw && hw->gpuEco && !*hw->gpuEco && !gpuAuto)
        tips.push_back({.8, tr(L"獨顯改用自動（用電池時 Eco）"), tr(L"⋯ → 華碩硬體 → 獨顯 → 自動；有程式喚醒獨顯時最多可省數瓦"), false});
    if (bs::lab::readKnob(bs::lab::Knob::darkMode, value) == ERROR_SUCCESS && value == 0)
        measuredTip(L"dark-mode", tr(L"改用深色模式（OLED）"), tr(L"Windows 設定 → 個人化 → 色彩"), .8);
    measuredTip(L"brightness", tr(L"螢幕亮度降 40%"), tr(L"OLED 上白底畫面影響最大"), .6);
    std::sort(tips.begin(), tips.end(), [](const Tip& a, const Tip& b) { return a.watts > b.watts; });
    return tips;
}
std::vector<const ProcessRow*> busyPrograms(size_t count) {
    std::vector<const ProcessRow*> busy;
    for (const auto& row : current.processes)
        if (row.averageCpuWatts && *row.averageCpuWatts >= .05) busy.push_back(&row);
    std::sort(busy.begin(), busy.end(), [](const ProcessRow* a, const ProcessRow* b) { return *a->averageCpuWatts > *b->averageCpuWatts; });
    if (busy.size() > count) busy.resize(count);
    return busy;
}
std::wstring goalText() {
    std::wstring c;
    if (!goalUse) c = tr(L"沒有設定目標（可在「設定 → 省電目標」選擇）。");
    else c = tr(L"目標：使用時 ") + std::to_wstring(goalUse) + tr(L" W 以下、靜態時 ") + std::to_wstring(goalStill) + tr(L" W 以下。只算用電池的時間，自動測試期間不算。");
    const auto use = goalAverage(false), still = goalAverage(true);
    c += tr(L"\n\n最近 30 分鐘");
    auto line = [&](const wchar_t* label, const GoalAverage& a, unsigned goal) {
        c += L"\n" + std::wstring(tr(label));
        if (a.minutes < 1) { c += tr(L"：資料不足（用電池至少 1 分鐘）"); return; }
        c += L"：" + number(a.watts, 1) + L" W（" + number(a.minutes, 0) + tr(L" 分鐘）");
        if (goal) c += a.watts <= goal ? tr(L" · 已達成") : tr(L" · 比目標多 ") + number(a.watts - goal, 1) + L" W";
    };
    line(L"使用中", use, goalUse);
    line(L"靜態（1 分鐘沒操作）", still, goalStill);
    c += tr(L"\n\n可以怎麼省（省最多的在前）");
    const auto tips = savingTips();
    for (size_t i = 0; i < tips.size(); ++i)
        c += L"\n" + std::to_wstring(i + 1) + L". " + tips[i].label + L" " + (tips[i].measured ? tr(L"約 −") : tr(L"估計 −")) + number(tips[i].watts, 1) + L" W · " + tips[i].detail;
    c += L"\n\n" + dynamicText();
    return c;
}
std::vector<VBlock> goalBlocks() {
    std::vector<VBlock> v;
    if ((demo || isolatedPreview) && goalSamples.empty())
        // Preview: the demo history, every third sample counted as still.
        for (size_t i = 0; i < current.history.size(); ++i)
            if (current.history[i].watts && *current.history[i].watts > 0) goalSamples.push_back({current.history[i].ms, *current.history[i].watts, i % 3 == 0});
    const auto use = goalAverage(false), still = goalAverage(true);
    auto meterValue = [](const GoalAverage& a, unsigned goal) {
        if (a.minutes < 1) return std::wstring(tr(L"資料不足"));
        return number(a.watts, 1) + L" W" + (goal ? L" / " + std::to_wstring(goal) + L" W" : std::wstring());
    };
    v.push_back(vHeading(goalUse ? tr(L"最近 30 分鐘 vs 目標") : tr(L"最近 30 分鐘")));
    v.push_back(vMeter(tr(L"使用中"), meterValue(use, goalUse), use.minutes >= 1 ? use.watts : 0, goalUse, 0));
    v.push_back(vMeter(tr(L"靜態（1 分鐘沒操作）"), meterValue(still, goalStill), still.minutes >= 1 ? still.watts : 0, goalStill, 0));
    {
        // The live series: the last 30 minutes of battery power, at most 120 columns.
        std::vector<double> series;
        for (const auto& point : current.history)
            if (point.watts && *point.watts > 0 && !point.onAc && current.history.back().ms - point.ms <= 30 * 60000) series.push_back(*point.watts);
        if (series.size() > 120) {
            std::vector<double> thin; const double step = series.size() / 120.0;
            for (int i = 0; i < 120; ++i) thin.push_back(series[size_t(i * step)]);
            series.swap(thin);
        }
        std::wstring now;
        if (!current.history.empty() && current.history.back().watts && !current.history.back().onAc) now = tr(L"現在 ") + number(*current.history.back().watts, 1) + L" W";
        v.push_back(vSeries(tr(L"用電池的耗電"), std::move(series), goalUse, now));
    }
    {
        std::vector<std::pair<std::wstring, COLORREF>> chips;
        chips.push_back({tr(L"動態省電 ") + std::wstring(dynamicEnabled ? tr(L"開") : tr(L"關")), dynamicEnabled ? Green : 0});
        if (dynamicEnabled && governor.stretchLevel())
            chips.push_back({governor.stretchLevel() == 2 ? tr(L"最大省電") : tr(L"加強省電"), theme.warn});
        const auto modes = queryPowerModes();
        if (modes.dcKnown) chips.push_back({tr(L"用電池：") + std::wstring(modeName(modes.dc)), modes.dc == PowerMode::efficiency ? Green : theme.warn});
        DWORD value = 0;
        if (bs::lab::readKnob(bs::lab::Knob::refreshRate, value) == ERROR_SUCCESS) chips.push_back({std::to_wstring(value) + L" Hz", value <= 60 ? Green : COLORREF(0)});
        if (bs::lab::readKnob(bs::lab::Knob::cpuBoost, value) == ERROR_SUCCESS) chips.push_back({value ? tr(L"Boost 開") : tr(L"Boost 關"), value ? theme.warn : Green});
        if (bs::lab::readKnob(bs::lab::Knob::darkMode, value) == ERROR_SUCCESS) chips.push_back({value ? tr(L"深色模式") : tr(L"淺色模式"), value ? Green : COLORREF(0)});
        if (asusAvailable) {
            const auto& h = asusNow();
            if (h.mode) chips.push_back({tr(L"華碩 ") + std::wstring(tr(bs::asus::modeKey(*h.mode))), *h.mode == bs::asus::Mode::silent ? Green : *h.mode == bs::asus::Mode::turbo ? theme.warn : COLORREF(0)});
            if (h.gpuEco) chips.push_back({(*h.gpuEco ? tr(L"獨顯 Eco") : tr(L"獨顯 標準")) + std::wstring(gpuAuto ? tr(L"（自動）") : L""), *h.gpuEco ? Green : COLORREF(0)});
        }
        // Settings the user chose by hand this battery session.
        const wchar_t* heldNames[] = {L"電源模式", L"處理器能源偏好", L"CPU Boost", L"螢幕更新率", L"華碩效能模式"};
        if (dynamicEnabled)
            for (size_t i = 0; i < size_t(bs::gov::Setting::count); ++i)
                if (governor.held(bs::gov::Setting(i))) chips.push_back({tr(L"手動：") + std::wstring(tr(heldNames[i])), theme.warn});
        v.push_back(vChips(std::move(chips)));
    }
    // Every saving switch, here where the saving is shown, each turned on or off with one click.
    v.push_back(vHeading(tr(L"省電開關")));
    v.push_back(vToggle(tr(L"動態省電"), tr(L"依你正在做的事自動調整電源設定"), dynamicEnabled, DynamicToggleId));
    {
        const int top = pluggedRefresh();
        v.push_back(vToggle(tr(L"用電池時 60 Hz"), top > 60 ? tr(L"插電自動回到 ") + std::to_wstring(top) + tr(L" Hz · 實測省 0.7–1.5 W") : std::wstring(tr(L"插電自動回到最高更新率")),
            refreshAuto, RefreshAutoId));
    }
    v.push_back(vToggle(tr(L"背景程式效率模式"), dynamicEnabled ? tr(L"用電池時讓背景程式以省電方式執行") : tr(L"隨動態省電一起運作"), dynamicEco, DynamicEcoId));
    {
        DWORD dark = 0;
        if (bs::lab::readKnob(bs::lab::Knob::darkMode, dark) == ERROR_SUCCESS)
            v.push_back(vToggle(tr(L"深色模式"), tr(L"OLED 螢幕上深色畫面較省電"), dark != 0, DarkModeId));
    }
    if (asusAvailable) {
        const auto& h = asusNow();
        if (h.gpuEco) v.push_back(vToggle(tr(L"獨顯自動 Eco"), tr(L"用電池時關閉獨顯，插電恢復"), gpuAuto, GpuAutoId));
        if (h.keyboardDevice) v.push_back(vToggle(tr(L"鍵盤背光閒置自動關"), tr(L"用電池時 30 秒沒操作就關，一碰就亮"), keyboardAutoOff, KeyboardAutoOffId));
        if (h.slash) v.push_back(vToggle(tr(L"Slash 燈效用電池時關閉"), tr(L"由韌體在拔掉電源時關閉燈效"), bs::asus::slashSettings().offOnBattery, SlashBatteryId));
        if (h.modeDevice) {
            const bool autoMode = dynamicEnabled && !governor.held(bs::gov::Setting::firmwareMode);
            v.push_back(vAction(tr(L"華碩效能模式"), (autoMode ? tr(L"自動") + std::wstring(L" · ") : std::wstring()) + (h.mode ? tr(bs::asus::modeKey(*h.mode)) : tr(L"未設定")),
                HwModeMenuId, h.mode == bs::asus::Mode::silent ? Green : COLORREF(0)));
        }
    }
    v.push_back(vAction(tr(L"低電量自動省電"), autoSaverThreshold ? std::to_wstring(autoSaverThreshold) + tr(L"% 以下") : std::wstring(tr(L"關")), AutoSaverCycleId,
        autoSaverThreshold ? Green : COLORREF(0)));
    if (dynamicEnabled && heldCount())
        v.push_back(vAction(tr(L"把手動設定交回動態省電"), std::to_wstring(heldCount()) + tr(L" 項"), ReleaseHoldsId, theme.warn));
    {
        const auto modes = queryPowerModes();
        v.push_back(vAction(tr(L"電源模式"), modes.dcKnown ? tr(L"用電池：") + std::wstring(modeName(modes.dc)) : std::wstring(), PowerModeId,
            modes.dcKnown && modes.dc == PowerMode::efficiency ? Green : COLORREF(0)));
    }
    v.push_back(vAction(tr(L"省電目標"), goalUse ? std::to_wstring(goalUse) + L" W／" + std::to_wstring(goalStill) + L" W" : std::wstring(tr(L"關")), GoalCycleId));
    {
        size_t rules = appRules.size(); for (const auto& exe : performanceApps) rules += !appRules.count(exe);
        v.push_back(vAction(tr(L"程式規則"), std::to_wstring(rules) + tr(L" 個"), AppRulesMenuId));
    }
    const auto tips = savingTips();
    if (!tips.empty()) {
        v.push_back(vHeading(tr(L"可以怎麼省")));
        std::vector<VBar> bars;
        for (const auto& t : tips)
            bars.push_back({t.label, (t.measured ? tr(L"實測 −") : tr(L"估計 −")) + number(t.watts, 1) + L" W", t.watts, 0, t.measured ? Green : blend(Muted, Surface, .35), {}});
        v.push_back(vBars(std::move(bars)));
        v.push_back(vLegend({{tr(L"這台實測"), Green}, {tr(L"估計（跑快速測試後換成實測）"), blend(Muted, Surface, .35)}}));
        std::wstring how;
        for (const auto& t : tips) how += (how.empty() ? L"" : L"\n") + t.label + (english ? L": " : L"：") + t.detail;
        if (dynamicEnabled) how += tr(L"\n動態省電已自動處理：") + handledByDynamic() + (heldCount() ? tr(L"（手動設定的項目除外，可從「⋯」交回）") : L"");
        v.push_back(vNote(how));
    }
    if (const auto busy = busyPrograms(6); !busy.empty()) {
        v.push_back(vHeading(tr(L"正在耗電的程式（CPU）")));
        std::vector<VBar> bars;
        for (const auto* row : busy) bars.push_back({row->name, number(*row->averageCpuWatts, 2) + L" W", *row->averageCpuWatts, 0, theme.warn, {}});
        v.push_back(vBars(std::move(bars)));
    }
    v.push_back(vHeading(tr(L"動態省電")));
    if (!dynamicEnabled) v.push_back(vNote(tr(L"關閉中。開啟後，用電池時會依你正在做的事自動切換：閒置與閱讀省電，一操作就恢復反應，高負載給效能。")));
    else {
        const auto& a = lastAssessment;
        std::vector<std::pair<std::wstring, COLORREF>> chips{{tr(bs::gov::activityKey(governor.activity())), Green}};
        if (signalReader && !signalReader->lastForeground().empty())
            chips.push_back({signalReader->lastForeground() + L" · " + tr(bs::gov::categoryKey(a.category)), 0});
        if (a.rule != bs::gov::AppRule::automatic) chips.push_back({tr(bs::gov::ruleKey(a.rule)), theme.warn});
        v.push_back(vChips(std::move(chips)));
        // Every candidate the detector weighed, by confidence.
        std::vector<VBar> bars;
        for (const auto& k : a.candidates)
            if (bars.size() < 5) bars.push_back({std::wstring(tr(bs::gov::activityKey(k.activity))) + L" · " + tr(k.reason.c_str()), number(k.confidence * 100, 0) + L"%",
                k.confidence * 100, 0, k.activity == a.activity ? Green : blend(Muted, Surface, .35), {}});
        if (!bars.empty()) v.push_back(vBars(std::move(bars), 100));
        std::wstring recent;
        int shown = 0;
        for (auto it = governor.log.rbegin(); it != governor.log.rend() && shown < 4; ++it, ++shown) recent += (recent.empty() ? L"" : L"\n") + *it;
        if (!recent.empty()) v.push_back(vNote(tr(L"最近的調整：\n") + recent));
    }
    v.push_back(vNote(tr(L"只讀取時間、程式名稱和裝置使用狀態，不截圖、不讀畫面或文字內容，也不記錄按了哪些鍵。")));
    return v;
}
// Shows a chart page; a refresh of the page already open only updates the charts.
void showChartPage(InlinePage page, const std::wstring& title, const std::wstring& text, std::vector<VBlock> blocks) {
    if (inlinePage == page && visualView && IsWindowVisible(visualView)) {
        std::wstring lines;
        for (size_t i = 0; i < text.size(); ++i) { if (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r')) lines += L'\r'; lines += text[i]; }
        SetWindowTextW(inlineText, lines.c_str());
        showVisual(std::move(blocks), true);
        return;
    }
    showInlinePage(page, title, text);
    showVisual(std::move(blocks), false);
    if (visualView) SetFocus(visualView);
}
void showGoalPage() {
    showChartPage(InlinePage::goal, tr(L"省電總覽"), goalText(), goalBlocks());
    goalPageShownAt = GetTickCount64();
}

// ----- Settings page -----
// Every option in one scrolling list, grouped into sections. A row shows its
// value; clicking it switches or cycles the value through the same commands
// the menus used.
enum class Row { section, dynamic, refreshAuto, dynamicRefresh, dynamicEco, target, goal, appRules, autoSaver, perfApps, lowBattery, chargeReminder, drainAlert,
    sleepAlert, theme, style, mascot, trayPercent, mini, language, startup, hotkey, history, clearHistory, updates };
struct SettingRow { Row row; const wchar_t* label; };
const SettingRow settingRows[] = {
    {Row::section, L"省電"}, {Row::dynamic, L"動態省電（實驗功能）"}, {Row::refreshAuto, L"用電池時 60 Hz"}, {Row::dynamicRefresh, L"動態切換螢幕更新率"}, {Row::dynamicEco, L"背景程式自動效率模式"},
    {Row::goal, L"省電目標"}, {Row::appRules, L"程式規則"}, {Row::autoSaver, L"低電量時自動省電"},
    {Row::section, L"提醒"}, {Row::lowBattery, L"低電量提醒"}, {Row::chargeReminder, L"充電提醒"}, {Row::drainAlert, L"背景耗電提醒"}, {Row::sleepAlert, L"睡眠耗電提醒"},
    {Row::section, L"外觀"}, {Row::theme, L"主題"}, {Row::style, L"風格"}, {Row::mascot, L"電池小精靈"}, {Row::trayPercent, L"系統匣顯示電量"},
    {Row::mini, L"迷你浮動視窗"}, {Row::language, L"語言 / Language"},
    {Row::section, L"一般"}, {Row::startup, L"登入時啟動"}, {Row::hotkey, L"快速鍵 Ctrl+Alt+B"}, {Row::history, L"記錄每日歷史"},
    {Row::clearHistory, L"清除歷史紀錄"}, {Row::updates, L"更新"}};
std::wstring rowValue(Row row) {
    auto onOff = [](bool on) { return std::wstring(on ? tr(L"開") : tr(L"關")); };
    auto percentOrOff = [](unsigned value) { return value ? std::to_wstring(value) + L"%" : std::wstring(tr(L"關")); };
    switch (row) {
    case Row::dynamic: return onOff(dynamicEnabled);
    case Row::dynamicRefresh: return onOff(dynamicRefresh);
    case Row::refreshAuto: return onOff(refreshAuto);
    case Row::dynamicEco: return onOff(dynamicEco);
    case Row::target: return dynamicTarget ? std::to_wstring(dynamicTarget) + tr(L" 小時") : std::wstring(tr(L"無"));
    case Row::goal: return goalUse ? std::to_wstring(goalUse) + L" W／" + std::to_wstring(goalStill) + L" W" : std::wstring(tr(L"關"));
    case Row::autoSaver: return percentOrOff(autoSaverThreshold);
    case Row::perfApps: return std::to_wstring(performanceApps.size()) + tr(L" 個");
    case Row::appRules: {
        size_t learned = 0; for (const auto& [exe, l] : appLearned) learned += learnedRule(l) != bs::gov::AppRule::automatic && !appRules.count(exe);
        size_t listed = 0; for (const auto& exe : performanceApps) listed += !appRules.count(exe);
        return std::to_wstring(appRules.size() + listed + learned) + tr(L" 個");
    }
    case Row::lowBattery: return percentOrOff(lowBatteryThreshold);
    case Row::chargeReminder: return percentOrOff(chargeReminderTarget);
    case Row::drainAlert: return onOff(drainAlerts);
    case Row::sleepAlert: return onOff(sleepAlerts);
    case Row::theme: return themeChoice == ui::ThemeChoice::system ? tr(L"跟隨 Windows") : themeChoice == ui::ThemeChoice::dark ? tr(L"深色") : tr(L"淺色");
    case Row::style: return pixelStyle ? tr(L"像素") : tr(L"平滑");
    case Row::mascot: return onOff(showMascot);
    case Row::trayPercent: return onOff(trayPercent);
    case Row::mini: return onOff(miniEnabled);
    case Row::language: return english ? L"English" : L"繁體中文";
    case Row::startup: return onOff(autoStart);
    case Row::hotkey: return onOff(hotkeyEnabled);
    case Row::history: return onOff(historyEnabled);
    case Row::clearHistory: return L"…";
    case Row::updates:
        if (availableUpdate) return availableUpdate->version + tr(L" 可安裝");
        if (updateBusy) return tr(L"檢查中…");
        return std::wstring(L"" BS_VERSION_STRING L" · ") + (!updateChecks ? tr(L"不檢查") : updateAuto ? tr(L"自動") : tr(L"只通知"));
    default: return {};
    }
}
bool rowOn(Row row) {
    switch (row) {
    case Row::dynamic: return dynamicEnabled;
    case Row::dynamicRefresh: return dynamicRefresh;
    case Row::refreshAuto: return refreshAuto;
    case Row::dynamicEco: return dynamicEco;
    case Row::target: return dynamicTarget != 0;
    case Row::autoSaver: return autoSaverThreshold != 0;
    case Row::lowBattery: return lowBatteryThreshold != 0;
    case Row::chargeReminder: return chargeReminderTarget != 0;
    case Row::drainAlert: return drainAlerts;
    case Row::sleepAlert: return sleepAlerts;
    case Row::mascot: return showMascot;
    case Row::trayPercent: return trayPercent;
    case Row::mini: return miniEnabled;
    case Row::startup: return autoStart;
    case Row::hotkey: return hotkeyEnabled;
    case Row::history: return historyEnabled;
    case Row::updates: return availableUpdate.has_value();
    default: return false;
    }
}
// The menu command a row sends; cycling rows pick the next value.
int rowCommand(Row row) {
    auto next = [](unsigned value, std::initializer_list<unsigned> values) {
        auto it = std::find(values.begin(), values.end(), value);
        return it == values.end() || std::next(it) == values.end() ? *values.begin() : *std::next(it);
    };
    switch (row) {
    case Row::dynamic: return DynamicToggleId;
    case Row::dynamicRefresh: return DynamicRefreshId;
    case Row::refreshAuto: return RefreshAutoId;
    case Row::dynamicEco: return DynamicEcoId;
    case Row::target: return DynamicTargetId;
    case Row::goal: return GoalCycleId;
    case Row::autoSaver: { const unsigned v = next(autoSaverThreshold, {0, 20, 30}); return v == 0 ? AutoSaverOffId : v == 20 ? AutoSaver20Id : AutoSaver30Id; }
    case Row::perfApps: return PerfAppsPageId;
    case Row::lowBattery: return LowBatteryOffId + int(next(lowBatteryThreshold, {0, 10, 20, 30}) / 10);
    case Row::chargeReminder: {
        const unsigned v = next(chargeReminderTarget, {0, 80, 85, 90, 100});
        return v == 0 ? ChargeReminderOffId : v == 80 ? ChargeReminder80Id : v == 85 ? ChargeReminder85Id : v == 90 ? ChargeReminder90Id : ChargeReminder100Id;
    }
    case Row::drainAlert: return DrainAlertId;
    case Row::sleepAlert: return SleepAlertId;
    case Row::theme: return themeChoice == ui::ThemeChoice::system ? ThemeDarkId : themeChoice == ui::ThemeChoice::dark ? ThemeLightId : ThemeSystemId;
    case Row::style: return pixelStyle ? StyleSmoothId : StylePixelId;
    case Row::mascot: return MascotToggleId;
    case Row::trayPercent: return TrayPercentId;
    case Row::mini: return MiniToggleId;
    case Row::language: return english ? LanguageZhId : LanguageEnId;
    case Row::startup: return StartupId;
    case Row::hotkey: return HotkeyToggleId;
    case Row::history: return HistoryToggleId;
    case Row::clearHistory: return ClearHistoryId;
    case Row::updates: return UpdateCheckId;
    default: return 0;
    }
}
void showSettingsPage() {
    if (SendMessageW(settingsList, LB_GETCOUNT, 0, 0) == 0)
        for (size_t i = 0; i < std::size(settingRows); ++i) SendMessageW(settingsList, LB_ADDSTRING, 0, LPARAM(i));
    showInlinePage(InlinePage::settings, tr(L"設定"), L"");
    InvalidateRect(settingsList, nullptr, FALSE);
}
void drawSettingRow(const DRAWITEMSTRUCT& item) {
    if (item.itemID == UINT(-1) || item.itemID >= std::size(settingRows)) return;
    const auto& entry = settingRows[item.itemID];
    HDC dc = item.hDC; RECT r = item.rcItem;
    fill(dc, r, Surface);
    if (entry.row == Row::section) {
        drawText(dc, tr(entry.label), {r.left + sp(6), r.top + sp(10), r.right - sp(6), r.bottom}, Green, strongFont);
        return;
    }
    RECT box{r.left + sp(2), r.top + sp(3), r.right - sp(2), r.bottom - sp(3)};
    rounded(dc, box, blend(Surface, Border, .45), 8);
    drawText(dc, tr(entry.label), {box.left + sp(12), box.top, box.right - sp(110), box.bottom}, Ink, normalFont);
    const std::wstring value = rowValue(entry.row);
    const bool on = rowOn(entry.row);
    const int width = std::max(sp(44), textWidth(dc, value, smallFont) + sp(18));
    RECT pill{box.right - sp(10) - width, box.top + sp(8), box.right - sp(10), box.bottom - sp(8)};
    rounded(dc, pill, on ? blend(Green, Surface, theme.highContrast ? 0 : .25) : blend(Surface, Border, .9), 8);
    drawText(dc, value, pill, on ? theme.accentInk : Muted, smallFont, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
}
// Per-program rules in one menu: the program last in front first, then every
// rule, each with the five choices; learned rules can be kept, changed or forgotten.
void showAppRulesMenu() {
    using bs::gov::AppRule;
    if (!signalReader) signalReader = std::make_unique<bs::lab::SignalReader>();
    signalReader->observe(std::nullopt);
    const std::wstring currentApp = signalReader->lastForeground();
    std::vector<std::wstring> apps;
    if (!currentApp.empty()) apps.push_back(currentApp);
    for (const auto& [exe, rule] : appRules) if (exe != currentApp) apps.push_back(exe);
    for (const auto& exe : performanceApps) if (exe != currentApp && !appRules.count(exe)) apps.push_back(exe);
    for (const auto& [exe, l] : appLearned) if (learnedRule(l) != AppRule::automatic && !appRules.count(exe) && exe != currentApp) apps.push_back(exe);
    constexpr int Base = 6000, Forget = 15;
    std::vector<MenuItem> items;
    for (size_t i = 0; i < apps.size() && i < 60; ++i) {
        bool learned = false;
        const AppRule rule = detector.ruleFor(apps[i], &learned);
        std::vector<MenuItem> choices;
        for (int r = 0; r < int(AppRule::count); ++r) choices.push_back(menuItem(Base + int(i) * 16 + r, tr(bs::gov::ruleKey(AppRule(r))), rule == AppRule(r)));
        if (appLearned.count(apps[i])) { choices.push_back(menuSeparator()); choices.push_back(menuItem(Base + int(i) * 16 + Forget, tr(L"忘記學到的習慣"))); }
        MenuItem entry = submenu((i == 0 && apps[i] == currentApp ? tr(L"目前程式：") : std::wstring()) + apps[i], choices);
        if (i == 0 && apps[i] == currentApp && apps.size() > 1) { items.push_back(entry); items.push_back(menuSeparator()); continue; }
        items.push_back(entry);
    }
    if (items.empty()) items.push_back(menuItem(0, tr(L"還沒有規則：先切到想設定的程式，再回來這裡"), false, true));
    const POINT point = menuAnchor();
    const int chosen = showMenu(items, point, mainWindow);
    if (chosen < Base) return;
    const size_t index = size_t(chosen - Base) / 16; const int choice = (chosen - Base) % 16;
    if (index >= apps.size()) return;
    const std::wstring& exe = apps[index];
    if (choice == Forget) { appLearned.erase(exe); saveLearned(exe); }
    else saveAppRule(exe, AppRule(choice));
    refreshDetectorRules();
    setNotice(exe + L"：" + (choice == Forget ? std::wstring(tr(L"已忘記學到的習慣")) : std::wstring(tr(bs::gov::ruleKey(AppRule(choice))))));
    if (dynamicEnabled) governorTick();
}
// Everything about updates in one menu: check or install now, and how updates happen.
std::vector<MenuItem> updateMenuItems() {
    std::vector<MenuItem> items;
    if (availableUpdate) {
        auto install = menuItem(UpdateInstallId, tr(L"安裝 ") + availableUpdate->version, false, updateBusy);
        install.hint = tr(L"新版本"); install.checked = true;
        items.push_back(install);
        items.push_back(menuItem(UpdatePageId, tr(L"看這版更新了什麼")));
    } else {
        auto check = menuItem(UpdateNowId, tr(L"立即檢查更新"), false, updateBusy);
        check.hint = updateBusy ? tr(L"檢查中…") : std::wstring(L"" BS_VERSION_STRING);
        items.push_back(check);
    }
    items.push_back(menuSeparator());
    items.push_back(menuItem(UpdateAutoId, tr(L"自動安裝新版本"), updateChecks && updateAuto));
    items.push_back(menuItem(UpdateNotifyId, tr(L"有新版本時通知我"), updateChecks && !updateAuto));
    items.push_back(menuItem(UpdateOffId, tr(L"不要自動檢查"), !updateChecks));
    return items;
}
void activateSettingRow(int index) {
    if (index < 0 || size_t(index) >= std::size(settingRows) || demo || isolatedPreview) return;
    if (settingRows[index].row == Row::updates) {
        POINT point{}; GetCursorPos(&point);
        if (const int chosen = showMenu(updateMenuItems(), point, mainWindow)) SendMessageW(mainWindow, WM_COMMAND, chosen, 0);
        if (inlinePage == InlinePage::settings) InvalidateRect(settingsList, nullptr, FALSE);
        return;
    }
    if (settingRows[index].row == Row::appRules) { showAppRulesMenu(); if (inlinePage == InlinePage::settings) InvalidateRect(settingsList, nullptr, FALSE); return; }
    if (const int command = rowCommand(settingRows[index].row)) SendMessageW(mainWindow, WM_COMMAND, command, 0);
    if (inlinePage == InlinePage::settings) InvalidateRect(settingsList, nullptr, FALSE);
}

// ----- ASUS hardware -----
// The firmware settings G-Helper and Armoury Crate change, through the same ATKACPI
// driver. Reads are cheap; every write is read back, and dynamic saving also manages
// the performance mode (silent for light use) when it is on.
// The discrete GPU goes to Eco only when no program is using it: the check samples
// the GPU engines for a second on a worker thread, then posts the users found.
bool gpuCheckBusy = false, gpuCheckManual = false;
int gpuAutoDone = -1;          // the power source the automatic switch last handled (0 battery, 1 plugged in)
uint64_t gpuRetryAt = 0;
void requestGpuEco(bool manual, bool) {
    if (gpuCheckBusy) return;
    gpuCheckBusy = true; gpuCheckManual = manual;
    HWND target = mainWindow; auto processes = current.processes;
    std::thread([target, processes] {
        GpuUsage gpu; std::unordered_map<uint32_t, double> usage;
        gpu.sample(usage); Sleep(1000); gpu.sample(usage);
        auto* users = new std::vector<std::wstring>(bs::lab::discreteGpuUsers(gpu.instances(), processes));
        if (!PostMessageW(target, GpuUsersMessage, 0, reinterpret_cast<LPARAM>(users))) delete users;
    }).detach();
}
void finishGpuEco(std::vector<std::wstring>* raw) {
    std::unique_ptr<std::vector<std::wstring>> users(raw);
    gpuCheckBusy = false;
    std::wstring names;
    for (const auto& name : *users) names += (names.empty() ? L"" : std::wstring(tr(L"、"))) + name;
    if (!names.empty()) {
        if (!gpuCheckManual) { // automatic: try again in a minute
            gpuRetryAt = GetTickCount64() + 60000;
            setNotice(tr(L"獨顯正被使用（") + names + tr(L"），關閉後會自動切到 Eco。"));
            return;
        }
        if (MessageBoxW(mainWindow, (tr(L"這些程式正在用獨顯：") + names + tr(L"\n切到 Eco 前請先關閉它們，否則可能失敗或讓它們出錯。仍要切換嗎？")).c_str(),
            L"BatteryScope", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    }
    const DWORD status = bs::asus::setGpuEco(true);
    asusReadAt = 0;
    setNotice(status == ERROR_SUCCESS ? std::wstring(tr(L"獨顯已切到 Eco（斷電）")) : tr(L"韌體沒有接受這個設定：") + errorText(status));
    if (inlinePage == InlinePage::hardware) showHardwarePage();
}
// G-Helper's "optimized" GPU mode: Eco on battery, standard when plugged in.
// 60 Hz on battery and the panel's highest rate plugged in, set when the power source changes
// (and a few samples after, since dynamic saving puts back what it found when it stops). Each
// change is only made once per switch, so a rate picked by hand afterwards stays. Dynamic saving's
// own rate choice, when turned on, and the lab while it measures go first.
int refreshAutoSource = -1, refreshAutoLeft = 0;
int batteryRefresh() {
    const auto rates = bs::lab::availableRefreshRates();
    if (std::find(rates.begin(), rates.end(), 60) != rates.end()) return 60;
    for (int hz : rates) if (hz >= 48) return hz;
    return 0;
}
int pluggedRefresh() { const auto rates = bs::lab::availableRefreshRates(); return rates.empty() ? 0 : rates.back(); }
void applyRefreshAuto() {
    if (!refreshAuto || demo || isolatedPreview || current.history.empty()) return;
    if (labController && labController->status().running) return;
    const auto& p = current.history.back();
    if (!p.acKnown || !p.present) return; // desktops: nothing to save
    const int source = p.onAc ? 1 : 0;
    if (source != refreshAutoSource) { refreshAutoSource = source; refreshAutoLeft = 3; }
    if (refreshAutoLeft <= 0) return;
    --refreshAutoLeft;
    if (source == 0 && dynamicEnabled && dynamicRefresh) { refreshAutoLeft = 0; return; } // dynamic saving picks the rate per activity
    const int target = source ? pluggedRefresh() : batteryRefresh();
    DWORD now = 0;
    if (!target || bs::lab::readKnob(bs::lab::Knob::refreshRate, now) != ERROR_SUCCESS || int(now) == target) return;
    if (source == 0 && int(now) < target) return; // already lower than 60: leave it
    bs::lab::applyKnob(bs::lab::Knob::refreshRate, DWORD(target));
}
void applyGpuAuto() {
    if (!gpuAuto || !asusAvailable || demo || isolatedPreview || current.history.empty() || gpuCheckBusy) return;
    if (labController && labController->status().running) return; // never during a measurement
    const auto& p = current.history.back();
    if (!p.acKnown) return;
    const int source = p.onAc ? 1 : 0;
    const auto& h = asusNow();
    if (!h.gpuEco) return;
    if (source == gpuAutoDone && (source == 1 || *h.gpuEco || GetTickCount64() < gpuRetryAt)) return;
    gpuAutoDone = source;
    if (source == 1 && *h.gpuEco) { if (bs::asus::setGpuEco(false) == ERROR_SUCCESS) setNotice(tr(L"插電：獨顯已切到標準。")); asusReadAt = 0; }
    else if (source == 0 && !*h.gpuEco) requestGpuEco(false, true);
}
// The firmware of many models forgets the charge limit at a restart and never reports the
// mode back: what BatteryScope set (mode, charge limit, keyboard, Fn lock, Slash) is put back
// at start and after sleep. A running test owns the mode until it ends.
void reapplyAsus() {
    if (!asusAvailable || demo || isolatedPreview) return;
    const bool labOwnsMode = labController && labController->status().running;
    bs::asus::reapply(!labOwnsMode, true);
    if (const auto slash = bs::asus::slashSettings(); slash.remembered && bs::asus::slashPresent()) bs::asus::setSlash(slash);
    asusReadAt = 0;
}
// ----- Hardware controls: one menu per row of the hardware page -----
bs::asus::Hotkeys asusKeys;
bool keyboardDimmed = false; unsigned keyboardBefore = 0;
std::vector<bs::asus::Service> asusServicesCache; uint64_t asusServicesAt = 0;
bool asusServicesBusy = false;
const std::vector<bs::asus::Service>& asusServicesNow() {
    const uint64_t now = GetTickCount64();
    if (!asusServicesAt || now - asusServicesAt >= 5000) { asusServicesCache = bs::asus::services(); asusServicesAt = now; }
    return asusServicesCache;
}
const wchar_t* keyboardLevelName(unsigned level) {
    switch (level) { case 0: return tr(L"關"); case 1: return tr(L"低"); case 2: return tr(L"中"); default: return tr(L"高"); }
}
std::vector<MenuItem> modeMenuItems() {
    const auto& h = asusNow();
    const bool preview = demo || isolatedPreview, off = preview || !h.modeDevice;
    // "Automatic" hands the mode to dynamic saving; picking a mode holds it for this battery session.
    const bool autoMode = dynamicEnabled && !governor.held(bs::gov::Setting::firmwareMode);
    auto automatic = menuItem(AsusAutoId, tr(L"自動（動態省電）"), autoMode, off || !dynamicEnabled);
    if (!dynamicEnabled) automatic.hint = tr(L"需開啟動態省電");
    return {automatic, menuSeparator(),
        menuItem(AsusSilentId, tr(L"靜音（最省電）"), !autoMode && h.mode == bs::asus::Mode::silent, off),
        menuItem(AsusBalancedId, tr(L"平衡"), !autoMode && h.mode == bs::asus::Mode::balanced, off),
        menuItem(AsusTurboId, tr(L"Turbo"), !autoMode && h.mode == bs::asus::Mode::turbo, off)};
}
std::vector<MenuItem> gpuMenuItems() {
    const auto& h = asusNow();
    const bool preview = demo || isolatedPreview, eco = h.gpuEco.value_or(false);
    return {menuItem(GpuAutoId, tr(L"自動（用電池 Eco、插電標準）"), gpuAuto, preview), menuSeparator(),
        menuItem(GpuEcoId, tr(L"Eco（斷電，最省電）"), !gpuAuto && eco, preview), menuItem(GpuStandardId, tr(L"標準（需要時使用）"), !gpuAuto && !eco, preview)};
}
std::vector<MenuItem> chargeMenuItems() {
    const unsigned limit = asusNow().chargeLimit.value_or(0);
    const bool preview = demo || isolatedPreview;
    return {menuItem(Charge60Id, tr(L"60%（長時間插電）"), limit == 60, preview), menuItem(Charge80Id, tr(L"80%（建議）"), limit == 80, preview),
        menuItem(Charge100Id, tr(L"100%（要出門前）"), limit == 100, preview)};
}
std::vector<MenuItem> keyboardMenuItems() {
    const auto level = asusNow().keyboard;
    const bool preview = demo || isolatedPreview;
    std::vector<MenuItem> items;
    for (unsigned i = 0; i < 4; ++i) items.push_back(menuItem(Keyboard0Id + int(i), keyboardLevelName(i), !keyboardDimmed && level == i, preview));
    items.push_back(menuSeparator());
    items.push_back(menuItem(KeyboardAutoOffId, tr(L"用電池時閒置 30 秒關閉"), keyboardAutoOff, preview));
    return items;
}
std::vector<MenuItem> keysMenuItems() {
    const bool preview = demo || isolatedPreview;
    auto handle = menuItem(KeysHandleId, tr(L"由 BatteryScope 接手華碩按鍵"), asusKeysHandled, preview);
    handle.hint = asusKeysHandled ? tr(L"華碩軟體不在時") : tr(L"關");
    return {handle, menuSeparator(),
        submenu(tr(L"ROG 鍵"), {menuItem(RogOpenId, tr(L"開啟 BatteryScope"), rogKeyAction == 0, preview), menuItem(RogModeId, tr(L"切換效能模式"), rogKeyAction == 1, preview),
            menuItem(RogNoneId, tr(L"不動作"), rogKeyAction == 2, preview)}, !asusKeysHandled),
        submenu(L"Fn+C", {menuItem(FnCLockId, tr(L"切換 Fn 鎖定"), fnCAction == 0, preview), menuItem(FnCNoneId, tr(L"不動作"), fnCAction == 1, preview)}, !asusKeysHandled)};
}
std::vector<MenuItem> servicesMenuItems() {
    const bool preview = demo || isolatedPreview;
    size_t running = 0, managed = 0, disabled = 0;
    for (const auto& sv : asusServicesNow()) { if (sv.managed) { ++managed; running += sv.running; } disabled += sv.startType == SERVICE_DISABLED; }
    auto stop = menuItem(ServicesStopId, tr(L"停止華碩背景服務（開機也不啟動）"), false, preview || asusServicesBusy || !managed);
    stop.hint = std::to_wstring(running) + tr(L" 個執行中");
    auto defaults = menuItem(ServicesDefaultsId, tr(L"恢復成華碩預設（自動啟動）"), false, preview || asusServicesBusy || asusServicesNow().empty());
    if (disabled) defaults.hint = std::to_wstring(disabled) + tr(L" 個已停用");
    return {stop, menuItem(ServicesStartId, tr(L"恢復停止前的設定"), false, preview || asusServicesBusy || !bs::asus::servicesKeptOff()), defaults};
}
std::vector<MenuItem> slashMenuItems() {
    const auto s = bs::asus::slashSettings();
    const bool preview = demo || isolatedPreview;
    std::vector<MenuItem> modes, brightness, interval;
    const auto& all = bs::asus::slashModes();
    for (size_t i = 0; i < all.size(); ++i) modes.push_back(menuItem(SlashModeId + int(i), bs::asus::slashModeName(all[i]), s.mode == all[i], preview));
    for (unsigned i = 1; i <= 3; ++i) brightness.push_back(menuItem(SlashBright1Id + int(i) - 1, keyboardLevelName(i), s.brightness == i, preview));
    for (unsigned i = 0; i <= 5; ++i) interval.push_back(menuItem(SlashIntervalId + int(i), i ? std::to_wstring(i) : std::wstring(tr(L"不停頓")), s.interval == i, preview));
    auto toggle = menuItem(SlashToggleId, tr(L"Slash 燈效"), s.on, preview);
    toggle.hint = s.on ? tr(L"開") : tr(L"關");
    return {toggle, menuSeparator(), submenu(tr(L"燈效"), modes, !s.on), submenu(tr(L"亮度"), brightness, !s.on), submenu(tr(L"動畫停頓"), interval, !s.on),
        menuSeparator(), menuItem(SlashBatteryId, tr(L"用電池時關閉"), s.offOnBattery, preview)};
}
// ----- Hardware keys -----
// Key actions that take a moment (WMI, audio devices) run one at a time, in order, off the UI thread.
class KeyWork {
public:
    void post(std::function<void()> job) {
        { std::lock_guard<std::mutex> guard(lock_); if (stop_) return; jobs_.push_back(std::move(job)); }
        if (!thread_.joinable()) thread_ = std::thread([this] { run(); });
        ready_.notify_one();
    }
    void stop() {
        { std::lock_guard<std::mutex> guard(lock_); stop_ = true; jobs_.clear(); }
        ready_.notify_one();
        if (thread_.joinable()) thread_.join();
    }
    ~KeyWork() { if (thread_.joinable()) thread_.detach(); }
private:
    void run() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> guard(lock_);
                ready_.wait(guard, [this] { return stop_ || !jobs_.empty(); });
                if (stop_) return;
                job = std::move(jobs_.front()); jobs_.pop_front();
            }
            job();
        }
    }
    std::mutex lock_; std::condition_variable ready_; std::deque<std::function<void()>> jobs_; std::thread thread_; bool stop_ = false;
};
KeyWork keyWork;
enum OsdKind : WPARAM { OsdBrightness = 0, OsdMicrophone = 1, OsdTouchpad = 2 };
// A small on-screen display for the keys: what changed, with a level bar where there is one.
// Which ASUS software still handles the keys: ASUS Optimization (brightness, backlight, touchpad,
// microphone, Fn lock...) and Armoury Crate (Fn+F5, the ROG key, lighting). Checked at most every 3 s.
struct { uint64_t at = 0; bool optimization = false, armoury = false; } asusRunning;
void refreshAsusRunning() {
    const uint64_t now = GetTickCount64();
    if (asusRunning.at && now - asusRunning.at < 3000) return;
    asusRunning.at = now;
    asusRunning.optimization = bs::asus::os::processRunning(L"asusoptimization.exe") || bs::asus::os::serviceRunning(L"ASUSOptimization");
    asusRunning.armoury = bs::asus::os::serviceRunning(L"ArmouryCrateControlInterface") || bs::asus::os::serviceRunning(L"ArmouryCrateService");
}
void cycleAsusMode() {
    const auto& h = asusNow();
    if (!h.modeDevice) return;
    // Silent → balanced → turbo → silent; a choice made with the key holds against dynamic saving.
    const auto next = bs::asus::nextMode(h.mode.value_or(bs::asus::Mode::balanced));
    const DWORD status = bs::asus::setMode(next);
    asusReadAt = 0;
    if (status != ERROR_SUCCESS) { showOsd(tr(L"效能模式沒有切換")); return; }
    if (dynamicEnabled) governor.hold(bs::gov::Setting::firmwareMode);
    showOsd(tr(L"效能模式：") + std::wstring(tr(bs::asus::modeKey(next))), -1, next == bs::asus::Mode::silent ? Green : next == bs::asus::Mode::turbo ? theme.warn : COLORREF(0));
}
void stepKeyboard(unsigned code) {
    const auto& h = asusNow();
    if (!h.keyboardDevice) return;
    const unsigned level = keyboardDimmed ? keyboardBefore : h.keyboard.value_or(0);
    const unsigned next = code == bs::asus::key::KeyboardUp ? std::min(3u, level + 1) : code == bs::asus::key::KeyboardDown ? (level ? level - 1 : 0) : (level + 1) % 4;
    keyboardDimmed = false;
    const DWORD status = bs::asus::setKeyboardBacklight(next);
    asusReadAt = 0;
    if (status == ERROR_SUCCESS) showOsd(tr(L"鍵盤背光：") + std::wstring(keyboardLevelName(next)), int(next * 100 / 3));
    else showOsd(tr(L"鍵盤背光沒有回應"));
}
// Fn lock without one in the firmware (ROG): F1-F11 are taken as hotkeys and do what Fn+F1..F11
// does, as G-Helper does it. Registered while the lock is on and BatteryScope handles the keys.
bool fnKeysRegistered = false;
std::array<uint64_t, 11> fnKeyLast{};
void updateFnKeys() {
    const bool wanted = asusAvailable && asusKeysHandled && !demo && !isolatedPreview && bs::asus::remembered().fnLock.value_or(false) && !bs::asus::fnLockHardware();
    if (wanted == fnKeysRegistered) return;
    for (int i = 0; i < 11; ++i) {
        if (wanted) RegisterHotKey(mainWindow, FnKeyHotkey + i, 0, VK_F1 + i);
        else UnregisterHotKey(mainWindow, FnKeyHotkey + i);
    }
    fnKeysRegistered = wanted;
}
void toggleFnLock() {
    const bool next = !bs::asus::remembered().fnLock.value_or(false);
    const DWORD status = bs::asus::setFnLock(next);
    asusReadAt = 0;
    updateFnKeys();
    showOsd(status != ERROR_SUCCESS ? std::wstring(tr(L"Fn 鎖定沒有回應")) : next ? tr(L"Fn 鎖定：開") : tr(L"Fn 鎖定：關"));
    if (inlinePage == InlinePage::hardware && !hidden) PostMessageW(mainWindow, SectionRefreshMessage, 0, 0);
}
void toggleSlash() {
    if (!bs::asus::slashPresent()) return;
    auto s = bs::asus::slashSettings();
    s.on = !s.on;
    showOsd(bs::asus::setSlash(s) != ERROR_SUCCESS ? std::wstring(tr(L"Slash 燈效沒有回應")) : s.on ? tr(L"Slash 燈效：開") : tr(L"Slash 燈效：關"));
}
// When BatteryScope last had the firmware send a brightness key: should the firmware also
// report it back as an ASUS key, that echo is not taken for a new press.
std::atomic<uint64_t> brightnessSentAt{0};
std::atomic<int> brightnessPending{0}; // brightness steps waiting for the key worker (+ up, - down)
// What each key does (also F1-F11 under a software Fn lock).
void performAsusKey(unsigned code) {
    namespace key = bs::asus::key;
    HWND target = mainWindow;
    switch (code) {
    case key::BrightnessDown: case key::BrightnessUp: {
        // The firmware sends Windows its own brightness key (as G-Helper does); where that
        // changes nothing, BatteryScope sets the level itself and shows it.
        // Presses while one is being applied add up (a held key), applied together.
        if (brightnessPending.fetch_add(code == key::BrightnessUp ? 1 : -1) != 0) return;
        keyWork.post([target] {
            for (int steps; (steps = brightnessPending.load()) != 0; brightnessPending.fetch_sub(steps)) {
                const bool up = steps > 0;
                const auto before = bs::asus::os::screenBrightness();
                brightnessSentAt = GetTickCount64();
                for (int i = 0; i < std::abs(steps); ++i)
                    bs::asus::firmwareKey(up ? bs::asus::FirmwareKey::brightnessUp : bs::asus::FirmwareKey::brightnessDown);
                Sleep(150);
                auto after = bs::asus::os::screenBrightness();
                if (before && after && *after != *before) continue; // Windows did it, with its own display
                if (before && (up ? *before >= 100 : *before <= 0)) { PostMessageW(target, OsdMessage, OsdBrightness, *before); continue; }
                if (bs::asus::os::stepScreenBrightness(steps * 10) == ERROR_SUCCESS) after = bs::asus::os::screenBrightness();
                PostMessageW(target, OsdMessage, OsdBrightness, after ? *after : -1);
            }
        });
        return;
    }
    case key::MicMute:
        keyWork.post([target] {
            const auto muted = bs::asus::os::toggleMicrophone();
            PostMessageW(target, OsdMessage, OsdMicrophone, muted ? (*muted ? 1 : 0) : -1);
        });
        return;
    case key::KeyboardUp: case key::KeyboardDown: case key::KeyboardCycle: stepKeyboard(code); return;
    case key::Touchpad:
        bs::asus::os::toggleTouchpad();
        keyWork.post([target] { Sleep(250); const auto on = bs::asus::os::touchpadOn(); PostMessageW(target, OsdMessage, OsdTouchpad, on ? (*on ? 1 : 0) : -1); });
        return;
    case key::Sleep: if (bs::asus::firmwareKey(bs::asus::FirmwareKey::sleep) != ERROR_SUCCESS) bs::asus::os::screenOff(mainWindow); return;
    case key::ScreenOff: case key::ScreenOffOld: bs::asus::os::screenOff(mainWindow); return;
    case key::Emoji: bs::asus::os::emojiPanel(); return;
    case key::Airplane: bs::asus::firmwareKey(bs::asus::FirmwareKey::airplane); return;
    case key::Calculator: ShellExecuteW(nullptr, L"open", L"calc.exe", nullptr, nullptr, SW_SHOWNORMAL); return;
    case key::FnLock: toggleFnLock(); return;
    case key::FnC: if (fnCAction == 0) toggleFnLock(); return;
    case key::Mode: case key::ModeOld: cycleAsusMode(); return;
    case key::Rog: if (rogKeyAction == 0) showMain(); else if (rogKeyAction == 1) cycleAsusMode(); return;
    case key::Aura: case key::AuraOld: if (bs::asus::slashPresent()) toggleSlash(); else stepKeyboard(key::KeyboardCycle); return;
    default: return;
    }
}
// The hardware keys, as the firmware and the keyboard report them. Each is left to the ASUS
// software that normally handles it while that runs, so nothing happens twice; once it is
// stopped (or was never installed) BatteryScope does it.
void handleAsusKey(unsigned code, bool fromKeyboard) {
    // The same key through both routes within a moment counts once.
    static unsigned lastCode = 0; static bool lastKeyboard = false; static uint64_t lastAt = 0;
    const uint64_t now = GetTickCount64();
    if (code == lastCode && fromKeyboard != lastKeyboard && now - lastAt < 150) return;
    lastCode = code; lastKeyboard = fromKeyboard; lastAt = now;
    asusKeysSeen.push_back({code, now});
    while (asusKeysSeen.size() > 6) asusKeysSeen.pop_front();
    if (inlinePage == InlinePage::hardware && !hidden) PostMessageW(mainWindow, SectionRefreshMessage, 0, 0);
    if (!asusKeysHandled || demo || isolatedPreview) return;
    namespace key = bs::asus::key;
    if ((code == key::BrightnessUp || code == key::BrightnessDown) && now - brightnessSentAt.load() < 250) return;
    refreshAsusRunning();
    const bool armouryKey = code == key::Mode || code == key::ModeOld || code == key::Rog || code == key::Aura || code == key::AuraOld;
    if (armouryKey ? asusRunning.armoury : code != key::FnC && asusRunning.optimization) return;
    performAsusKey(code);
}
// F1-F11 under the software Fn lock: what Fn+F1..F11 does on a ROG keyboard.
void handleFnKey(int index) {
    if (index < 0 || index > 10) return;
    namespace key = bs::asus::key;
    const uint64_t now = GetTickCount64();
    const bool repeats = index == 1 || index == 2 || index == 6 || index == 7; // backlight and brightness steps repeat while held
    if (!repeats && now - fnKeyLast[size_t(index)] < 400) return;
    fnKeyLast[size_t(index)] = now;
    switch (index) {
    case 0: bs::asus::os::tap({VK_VOLUME_MUTE}); return;
    case 1: performAsusKey(key::KeyboardDown); return;
    case 2: performAsusKey(key::KeyboardUp); return;
    case 3: performAsusKey(key::Aura); return;
    case 4: performAsusKey(key::Mode); return;
    case 5: bs::asus::os::tap({VK_SNAPSHOT}); return;
    case 6: performAsusKey(key::BrightnessDown); return;
    case 7: performAsusKey(key::BrightnessUp); return;
    case 8: bs::asus::os::tap({VK_LWIN, 'P'}); return;
    case 9: performAsusKey(key::Touchpad); return;
    default: performAsusKey(key::Sleep); return;
    }
}
// Keyboard backlight off after 30 s without input on battery, back on at the next input.
void keyboardTick() {
    if (!keyboardAutoOff || !asusAvailable || demo || isolatedPreview || current.history.empty()) return;
    LASTINPUTINFO input{}; input.cbSize = sizeof(input);
    const double idle = GetLastInputInfo(&input) ? (GetTickCount() - input.dwTime) / 1000.0 : 0;
    const auto& p = current.history.back();
    const bool battery = p.acKnown && !p.onAc;
    if (keyboardDimmed && (idle < 1 || !battery)) {
        bs::asus::setKeyboardBacklight(keyboardBefore); keyboardDimmed = false; asusReadAt = 0;
    } else if (!keyboardDimmed && battery && idle >= 30) {
        const auto level = asusNow().keyboard;
        if (level && *level > 0 && bs::asus::setKeyboardBacklight(0, false) == ERROR_SUCCESS) { keyboardBefore = *level; keyboardDimmed = true; asusReadAt = 0; }
    }
}
void updateKeyboardTimer() {
    if (keyboardAutoOff && asusAvailable && !demo && !isolatedPreview) SetTimer(mainWindow, KeyboardTimer, 1000, nullptr);
    else { KillTimer(mainWindow, KeyboardTimer); if (keyboardDimmed) { bs::asus::setKeyboardBacklight(keyboardBefore); keyboardDimmed = false; } }
}
// Stopping or starting ASUS services needs administrator rights: BatteryScope runs a copy
// of itself elevated (one UAC prompt) and waits for it on a worker thread.
enum class ServicesAction { start = 0, stop = 1, defaults = 2 };
void runAsusServices(ServicesAction action) {
    if (asusServicesBusy) return;
    asusServicesBusy = true;
    HWND target = mainWindow; const std::wstring path = executablePath();
    std::thread([target, path, action] {
        SHELLEXECUTEINFOW info{}; info.cbSize = sizeof(info); info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        info.lpVerb = L"runas"; info.lpFile = path.c_str(); info.nShow = SW_HIDE;
        info.lpParameters = action == ServicesAction::stop ? L"--asus-services stop" : action == ServicesAction::defaults ? L"--asus-services defaults" : L"--asus-services start";
        DWORD code = ERROR_CANCELLED;
        if (ShellExecuteExW(&info) && info.hProcess) {
            WaitForSingleObject(info.hProcess, 120000);
            GetExitCodeProcess(info.hProcess, &code);
            CloseHandle(info.hProcess);
        } else code = GetLastError();
        PostMessageW(target, AsusServicesMessage, code, LPARAM(action));
    }).detach();
}
std::wstring hardwareText(const bs::asus::Snapshot& h) {
    std::wstring c = tr(L"型號：") + (h.model.empty() ? std::wstring(tr(L"未知")) : h.model);
    if (!h.driver) return c + tr(L"\n找不到華碩的 ATKACPI 驅動（ASUS System Control Interface）。這台可能不是華碩筆電，或驅動沒有安裝。");
    if (h.modeDevice) c += tr(L"\n效能模式：") + std::wstring(h.mode ? tr(bs::asus::modeKey(*h.mode)) : tr(L"未設定"));
    if (h.gpuEco) c += tr(L"\n獨顯：") + std::wstring(*h.gpuEco ? L"Eco" : tr(L"標準"));
    if (h.gpuMux) c += tr(L"\n畫面輸出：") + std::wstring(*h.gpuMux ? tr(L"混合（內顯）") : tr(L"獨顯直連"));
    if (h.chargeDevice) c += tr(L"\n充電上限：") + (h.chargeLimit ? std::to_wstring(*h.chargeLimit) + L"%" : std::wstring(tr(L"未設定")));
    if (h.keyboard) c += tr(L"\n鍵盤背光：") + std::wstring(keyboardLevelName(*h.keyboard));
    if (h.fnLock) c += tr(L"\nFn 鎖定：") + std::wstring(*h.fnLock ? tr(L"開") : tr(L"關")) + (h.fnLockHardware ? L"" : tr(L"（BatteryScope 對應）"));
    if (h.cpuTemp) c += tr(L"\nCPU 溫度：") + std::to_wstring(*h.cpuTemp) + L" °C";
    if (h.cpuFan) c += tr(L"\nCPU 風扇：") + std::to_wstring(*h.cpuFan * 100) + L" RPM";
    if (h.gpuFan) c += tr(L"\n獨顯風扇：") + std::to_wstring(*h.gpuFan * 100) + L" RPM";
    for (const auto& [label, watts] : h.limits) c += L"\n" + std::wstring(tr(label)) + L"：" + std::to_wstring(watts) + L" W";
    wchar_t status[64]; swprintf(status, 64, L"\nhotkeys wmi=0x%08lX keyboard=%d", static_cast<unsigned long>(asusKeys.status()), asusKeys.keyboard() ? 1 : 0);
    c += L"\n\n" + bs::asus::dump(h) + status;
    return c;
}
void showHardwarePage() {
    bs::asus::Snapshot h = asusNow();
    const bool preview = demo || isolatedPreview;
    if (preview) { // preview data, as a G14 2024 reports it
        h.driver = true; h.model = L"ROG Zephyrus G14 GA403UV"; h.modeDevice = true; h.mode = bs::asus::Mode::balanced; h.gpuEco = true; h.gpuMux = true;
        h.chargeDevice = true; h.chargeLimit = 80; h.overdrive.reset(); h.cpuTemp = 52; h.cpuFan = 28; h.gpuFan = 0;
        h.keyboardDevice = true; h.keyboard = 1; h.fnLockHardware = false; h.fnLock = false; h.slash = true; h.limits.clear();
    }
    std::vector<VBlock> v;
    std::vector<std::pair<std::wstring, COLORREF>> chips{{h.model.empty() ? std::wstring(tr(L"型號未知")) : h.model, 0}};
    if (!h.driver) {
        v.push_back(vChips(std::move(chips)));
        v.push_back(vNote(tr(L"找不到華碩的 ATKACPI 驅動（ASUS System Control Interface）。這台可能不是華碩筆電，或驅動沒有安裝。")));
        showChartPage(InlinePage::hardware, tr(L"華碩硬體"), hardwareText(h), std::move(v));
        return;
    }
    const auto modeColor = [&](std::optional<bs::asus::Mode> mode) { return mode == bs::asus::Mode::silent ? Green : mode == bs::asus::Mode::turbo ? theme.warn : COLORREF(0); };
    if (h.mode) chips.push_back({tr(L"效能模式 ") + std::wstring(tr(bs::asus::modeKey(*h.mode))), modeColor(h.mode)});
    if (h.gpuEco) chips.push_back({*h.gpuEco ? tr(L"獨顯 Eco") : tr(L"獨顯 標準"), *h.gpuEco ? Green : theme.warn});
    if (h.gpuMux && !*h.gpuMux) chips.push_back({tr(L"獨顯直連"), theme.warn});
    if (h.chargeLimit) chips.push_back({tr(L"充電上限 ") + std::to_wstring(*h.chargeLimit) + L"%", *h.chargeLimit <= 80 ? Green : COLORREF(0)});
    v.push_back(vChips(std::move(chips)));
    // Controls, each opening its choices in place.
    {
        const bool autoMode = dynamicEnabled && !governor.held(bs::gov::Setting::firmwareMode);
        const std::wstring unset = tr(L"未設定");
        if (h.modeDevice) v.push_back(vAction(tr(L"效能模式"), (autoMode ? tr(L"自動") + std::wstring(L" · ") : std::wstring()) + (h.mode ? tr(bs::asus::modeKey(*h.mode)) : unset),
            HwModeMenuId, modeColor(h.mode)));
        if (h.gpuEco) v.push_back(vAction(tr(L"獨顯"), (gpuAuto ? tr(L"自動") + std::wstring(L" · ") : std::wstring()) + (*h.gpuEco ? L"Eco" : tr(L"標準")), HwGpuMenuId, *h.gpuEco ? Green : COLORREF(0)));
        if (h.chargeDevice) v.push_back(vAction(tr(L"充電上限"), h.chargeLimit ? std::to_wstring(*h.chargeLimit) + L"%" : unset, HwChargeMenuId,
            h.chargeLimit && *h.chargeLimit <= 80 ? Green : COLORREF(0)));
        if (h.keyboardDevice) v.push_back(vAction(tr(L"鍵盤背光"), (h.keyboard || keyboardDimmed ? std::wstring(keyboardLevelName(keyboardDimmed ? 0 : *h.keyboard)) : unset)
            + (keyboardAutoOff ? tr(L" · 閒置自動關") : L""), HwKeyboardMenuId));
        v.push_back(vAction(tr(L"Fn 鎖定"), h.fnLock.value_or(false) ? tr(L"開（F1–F11 免按 Fn）") : tr(L"關"), FnLockId, h.fnLock.value_or(false) ? Green : COLORREF(0)));
        if (h.slash) {
            const auto s = preview ? bs::asus::Slash{} : bs::asus::slashSettings();
            v.push_back(vAction(tr(L"Slash 燈效"), s.on ? std::wstring(bs::asus::slashModeName(s.mode)) + (s.offOnBattery ? tr(L" · 用電池關") : L"") : std::wstring(tr(L"關")), HwSlashMenuId,
                s.on ? Green : COLORREF(0)));
        }
        if (h.overdrive) v.push_back(vAction(tr(L"螢幕 Overdrive"), *h.overdrive ? tr(L"開") : tr(L"關"), OverdriveId));
        const bool listening = preview || SUCCEEDED(asusKeys.status()) || asusKeys.keyboard();
        v.push_back(vAction(tr(L"華碩按鍵"), !asusKeysHandled ? std::wstring(tr(L"交給華碩軟體")) : !listening ? std::wstring(tr(L"收不到按鍵")) : tr(L"BatteryScope 接手"),
            HwKeysMenuId, !asusKeysHandled ? COLORREF(0) : !listening ? theme.warn : Green));
        size_t running = 0, total = 0, disabled = 0;
        if (!preview) for (const auto& sv : asusServicesNow()) { if (sv.managed) { running += sv.running; ++total; } disabled += sv.startType == SERVICE_DISABLED; }
        else { running = 0; total = 9; }
        v.push_back(vAction(tr(L"華碩服務"), total ? std::to_wstring(running) + L" / " + std::to_wstring(total) + tr(L" 執行中") + (disabled ? L" · " + std::to_wstring(disabled) + tr(L" 個停用") : L"")
            : std::wstring(tr(L"沒有")), HwServicesMenuId, running ? theme.warn : Green));
    }
    if (h.cpuTemp || h.gpuTemp) {
        v.push_back(vHeading(tr(L"溫度")));
        if (h.cpuTemp) v.push_back(vMeter(L"CPU", std::to_wstring(*h.cpuTemp) + L" °C", *h.cpuTemp, 85, 100));
        if (h.gpuTemp) v.push_back(vMeter(tr(L"獨顯"), std::to_wstring(*h.gpuTemp) + L" °C", *h.gpuTemp, 80, 100));
    }
    if (h.cpuFan || h.gpuFan || h.midFan) {
        v.push_back(vHeading(tr(L"風扇")));
        std::vector<VBar> bars;
        if (h.cpuFan) bars.push_back({L"CPU", *h.cpuFan ? std::to_wstring(*h.cpuFan * 100) + L" RPM" : std::wstring(tr(L"停止")), double(*h.cpuFan), 0, 0, {}});
        if (h.gpuFan) bars.push_back({tr(L"獨顯"), *h.gpuFan ? std::to_wstring(*h.gpuFan * 100) + L" RPM" : std::wstring(tr(L"停止")), double(*h.gpuFan), 0, 0, {}});
        if (h.midFan) bars.push_back({tr(L"中間"), *h.midFan ? std::to_wstring(*h.midFan * 100) + L" RPM" : std::wstring(tr(L"停止")), double(*h.midFan), 0, 0, {}});
        v.push_back(vBars(std::move(bars), 70));
    }
    if (!h.limits.empty()) {
        v.push_back(vHeading(tr(L"功耗上限")));
        std::vector<VBar> bars;
        for (const auto& [label, watts] : h.limits) bars.push_back({tr(label), std::to_wstring(watts) + L" W", double(watts), 0, blend(Green, Surface, .3), {}});
        v.push_back(vBars(std::move(bars)));
    }
    if (!preview && asusKeysHandled && FAILED(asusKeys.status()) && !asusKeys.keyboard()) {
        wchar_t code[16]; swprintf(code, 16, L"0x%08lX", static_cast<unsigned long>(asusKeys.status()));
        v.push_back(vNote(tr(L"收不到華碩按鍵的訊號（") + std::wstring(code) + tr(L"）：ASUS System Control Interface 驅動可能沒有安裝。")));
    }
    if (!asusKeysSeen.empty()) {
        std::wstring keys;
        for (auto it = asusKeysSeen.rbegin(); it != asusKeysSeen.rend(); ++it) {
            wchar_t code[16]; swprintf(code, 16, L"0x%02X", it->first);
            const wchar_t* name = bs::asus::keyName(it->first);
            keys += (keys.empty() ? L"" : std::wstring(tr(L"、"))) + (name ? std::wstring(tr(name)) + L" " + code : std::wstring(code));
        }
        v.push_back(vNote(tr(L"最近按下的華碩按鍵：") + keys));
    }
    v.push_back(vNote(tr(L"這台的韌體不會回報效能模式與充電上限：顯示的是 BatteryScope 最後設定的值，開機與睡眠喚醒後會自動重設。效能模式選「自動」時由動態省電管理；手動選的模式在這次用電池期間保留。華碩軟體停止後，亮度、鍵盤背光、觸控板、麥克風、睡眠、Fn 鎖定（Fn+C、Fn+Esc）、效能模式與 ROG 鍵都由 BatteryScope 接手。Fn 鎖定開啟時，F1–F11 不按 Fn 就是快捷功能。")));
    showChartPage(InlinePage::hardware, tr(L"華碩硬體"), hardwareText(h), std::move(v));
}
// ----- The performance dashboard (home) -----
// One look at what the machine is doing and the controls that change it: a battery line, the
// performance mode, CPU (power, temperature, clock, load), GPU (integrated use; the discrete
// GPU's state, and its readings only while it is awake anyway), fans with their preset, the CPU
// power limit preset, and quick switches. The battery itself and the programs using it are on
// the battery tab.
std::vector<MenuItem> fanMenuItems() {
    const auto t = bs::asus::tuning();
    const bool preview = demo || isolatedPreview;
    std::vector<MenuItem> items;
    const std::pair<bs::asus::FanPreset, const wchar_t*> rows[] = {
        {bs::asus::FanPreset::factory, L"原廠（跟隨效能模式）"}, {bs::asus::FanPreset::quiet, L"安靜（靜音模式的曲線）"},
        {bs::asus::FanPreset::standard, L"標準（平衡模式的曲線）"}, {bs::asus::FanPreset::strong, L"強力（Turbo 的曲線）"}};
    for (const auto& [preset, label] : rows) items.push_back(menuItem(FanPreset0Id + int(preset), tr(label), t.fans == preset, preview));
    return items;
}
std::vector<MenuItem> powerLimitMenuItems() {
    const auto t = bs::asus::tuning();
    const bool preview = demo || isolatedPreview;
    std::vector<MenuItem> items;
    items.push_back(menuItem(PowerPreset0Id, tr(L"原廠（跟隨效能模式）"), t.power == bs::asus::PowerPreset::factory, preview));
    const std::pair<bs::asus::PowerPreset, const wchar_t*> rows[] = {
        {bs::asus::PowerPreset::saver, L"省電"}, {bs::asus::PowerPreset::light, L"輕量"}, {bs::asus::PowerPreset::standard, L"標準"}};
    for (const auto& [preset, label] : rows) {
        const auto limits = bs::asus::powerLimits(preset);
        auto item = menuItem(PowerPreset0Id + int(preset), tr(label), t.power == preset, preview);
        item.hint = std::to_wstring(limits.spl) + L"／" + std::to_wstring(limits.fppt) + L" W";
        items.push_back(item);
    }
    return items;
}
std::wstring powerPresetText(bs::asus::PowerPreset preset) {
    if (preset == bs::asus::PowerPreset::factory) return tr(L"原廠");
    return std::to_wstring(bs::asus::powerLimits(preset).spl) + L" W";
}
std::wstring perfPageText() {
    PerfReading perf; { std::lock_guard<std::mutex> lock(perfMutex); perf = perfLatest; }
    std::wstring c = tr(L"CPU 功耗：") + valueText(current.energy.packageWatts, L" W");
    if (perf.cpuMhz) c += tr(L"\nCPU 時脈：") + number(*perf.cpuMhz / 1000, 2) + L" GHz";
    if (perf.igpu) c += tr(L"\n內顯使用率：") + number(*perf.igpu, 0) + L"%";
    c += tr(L"\n獨顯狀態：") + std::to_wstring(perf.dgpuState);
    return c;
}
void showPerformancePage() {
    const bool preview = demo || isolatedPreview;
    PerfReading perf; { std::lock_guard<std::mutex> lock(perfMutex); perf = perfLatest; }
    bs::asus::Snapshot h = asusAvailable ? asusNow() : bs::asus::Snapshot{};
    if (preview) {
        perf.cpuMhz = 3120; perf.igpu = 14; perf.dgpuState = 9;
        h.driver = true; h.modeDevice = true; h.mode = bs::asus::Mode::silent; h.gpuEco = true; h.cpuTemp = 52; h.cpuFan = 28; h.gpuFan = 0;
        h.keyboardDevice = true; h.keyboard = 1;
    }
    std::vector<VBlock> v;
    // The battery in one line.
    if (!current.history.empty()) {
        const auto& p = current.history.back();
        std::vector<std::pair<std::wstring, COLORREF>> chips;
        if (p.percent) chips.push_back({valueText(p.percent, L"%", 0), *p.percent <= 20 ? theme.warn : Green});
        const bool plugged = p.acKnown && (p.onAc || p.charging);
        if (p.charging) chips.push_back({tr(L"充電 ") + valueText(calculateCharging(current.history).watts, L" W"), Green});
        else if (plugged) chips.push_back({tr(L"插電"), 0});
        else chips.push_back({tr(L"耗電 ") + valueText(current.power.currentWatts, L" W"), 0});
        if (!plugged) if (auto left = remainingText(current.power); !left.empty()) chips.push_back({left, 0});
        v.push_back(vChips(std::move(chips)));
    }
    // The performance mode: ASUS's firmware modes, else Windows' power mode for the current source.
    v.push_back(vHeading(tr(L"效能模式")));
    if (asusAvailable && h.modeDevice) {
        const bool autoMode = dynamicEnabled && !governor.held(bs::gov::Setting::firmwareMode);
        std::vector<std::wstring> labels; std::vector<int> commands;
        if (dynamicEnabled) { labels.push_back(tr(L"自動")); commands.push_back(AsusAutoId); }
        labels.push_back(tr(L"靜音")); commands.push_back(AsusSilentId);
        labels.push_back(tr(L"平衡")); commands.push_back(AsusBalancedId);
        labels.push_back(L"Turbo"); commands.push_back(AsusTurboId);
        int selected = -1;
        if (autoMode) selected = 0;
        else if (h.mode) selected = (dynamicEnabled ? 1 : 0) + (*h.mode == bs::asus::Mode::silent ? 0 : *h.mode == bs::asus::Mode::balanced ? 1 : 2);
        v.push_back(vSegments(std::move(labels), std::move(commands), selected));
    } else {
        const bool plugged = !current.history.empty() && current.history.back().acKnown && current.history.back().onAc;
        const auto modes = queryPowerModes();
        const int base = plugged ? ModeAcEfficiencyId : ModeDcEfficiencyId;
        const bool known = plugged ? modes.acKnown : modes.dcKnown;
        const PowerMode mode = plugged ? modes.ac : modes.dc;
        v.push_back(vSegments({tr(L"效率"), tr(L"平衡"), tr(L"效能")}, {base, base + 1, base + 2},
            known && mode != PowerMode::other ? int(mode) : -1));
    }
    // CPU.
    v.push_back(vHeading(L"CPU"));
    {
        std::vector<std::pair<std::wstring, COLORREF>> chips;
        if (h.cpuTemp) chips.push_back({std::to_wstring(*h.cpuTemp) + L" °C", *h.cpuTemp >= 90 ? theme.danger : *h.cpuTemp >= 80 ? theme.warn : COLORREF(0)});
        if (perf.cpuMhz) chips.push_back({number(*perf.cpuMhz / 1000, 2) + L" GHz", 0});
        double busy = 0; for (const auto& core : current.energy.cores) busy += core.busyLogicalCores;
        if (current.energy.ready) chips.push_back({tr(L"負載 ") + number(busy * 100 / std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)), 0) + L"%", 0});
        DWORD boost = 0;
        if (bs::lab::readKnob(bs::lab::Knob::cpuBoost, boost) == ERROR_SUCCESS) chips.push_back({boost ? tr(L"Boost 開") : tr(L"Boost 關"), boost ? theme.warn : Green});
        v.push_back(vChips(std::move(chips)));
        const Value watts = current.energy.packageWatts ? current.energy.packageWatts : current.energy.coreWatts;
        const auto limits = bs::asus::powerLimits(bs::asus::tuning().power);
        v.push_back(vMeter(tr(L"功耗"), valueText(watts, L" W") + (limits.spl ? L" / " + std::to_wstring(limits.spl) + L" W" : std::wstring()),
            watts.value_or(0), limits.spl, std::max(30.0, double(limits.fppt))));
        std::vector<double> series;
        for (const auto& [ms, value] : cpuPowerHistory) if (value && !cpuPowerHistory.empty() && cpuPowerHistory.back().first - ms <= 5 * 60000) series.push_back(*value);
        if (series.size() > 120) series.erase(series.begin(), series.end() - 120);
        if (series.empty() && (demo || isolatedPreview))
            for (int i = 0; i < 100; ++i) series.push_back(5.0 + 3.0 * std::sin(i / 6.0) + (i % 17 == 0 ? 9.0 : 0.0) + (i > 70 && i < 80 ? 12.0 : 0.0));
        v.push_back(vSeries(tr(L"近 5 分鐘"), std::move(series), limits.spl, watts ? tr(L"現在 ") + valueText(watts, L" W") : std::wstring()));
    }
    // GPU.
    v.push_back(vHeading(L"GPU"));
    {
        v.push_back(vMeter(tr(L"內顯使用率"), perf.igpu ? number(*perf.igpu, 0) + L"%" : std::wstring(L"—"), perf.igpu.value_or(0), 0, 100));
        std::vector<std::pair<std::wstring, COLORREF>> chips;
        const bool eco = h.gpuEco.value_or(false) || perf.dgpuState == 9;
        if (eco) chips.push_back({tr(L"獨顯 Eco（斷電）"), Green});
        else if (perf.dgpuState >= 1) chips.push_back({tr(L"獨顯休眠中"), Green});
        else if (perf.dgpuState == 0) chips.push_back({tr(L"獨顯使用中") + (perf.nvLoad ? L" " + number(*perf.nvLoad, 0) + L"%" : std::wstring()), theme.warn});
        else chips.push_back({tr(L"獨顯狀態未知"), 0});
        if (perf.nvTemp) chips.push_back({number(*perf.nvTemp, 0) + L" °C", 0});
        if (perf.nvClock) chips.push_back({number(*perf.nvClock, 0) + L" MHz", 0});
        if (perf.nvWatts) chips.push_back({number(*perf.nvWatts, 1) + L" W", 0});
        v.push_back(vChips(std::move(chips)));
        if (!perf.dgpuApps.empty()) {
            std::wstring apps = perf.dgpuApps;
            std::replace(apps.begin(), apps.end(), L';', L'、');
            v.push_back(vNote(tr(L"正在用獨顯：") + apps));
        }
        if (asusAvailable && h.gpuEco) v.push_back(vAction(tr(L"獨顯模式"), (gpuAuto ? tr(L"自動") + std::wstring(L" · ") : std::wstring()) + (*h.gpuEco ? L"Eco" : tr(L"標準")),
            HwGpuMenuId, *h.gpuEco ? Green : COLORREF(0)));
    }
    // Fans and power limits (ASUS).
    if (asusAvailable && h.driver) {
        v.push_back(vHeading(tr(L"風扇與功耗")));
        if (h.cpuFan || h.gpuFan || h.midFan) {
            std::vector<VBar> bars;
            if (h.cpuFan) bars.push_back({L"CPU", *h.cpuFan ? std::to_wstring(*h.cpuFan * 100) + L" RPM" : std::wstring(tr(L"停止")), double(*h.cpuFan), 0, 0, {}});
            if (h.gpuFan) bars.push_back({tr(L"獨顯"), *h.gpuFan ? std::to_wstring(*h.gpuFan * 100) + L" RPM" : std::wstring(tr(L"停止")), double(*h.gpuFan), 0, 0, {}});
            if (h.midFan) bars.push_back({tr(L"中間"), *h.midFan ? std::to_wstring(*h.midFan * 100) + L" RPM" : std::wstring(tr(L"停止")), double(*h.midFan), 0, 0, {}});
            v.push_back(vBars(std::move(bars), 70));
        }
        const auto t = bs::asus::tuning();
        v.push_back(vAction(tr(L"風扇模式"), tr(bs::asus::fanPresetKey(t.fans)), FanMenuId, t.fans == bs::asus::FanPreset::quiet ? Green : COLORREF(0)));
        v.push_back(vAction(tr(L"CPU 功耗上限"), powerPresetText(t.power), PowerLimitMenuId, t.power == bs::asus::PowerPreset::saver ? Green : COLORREF(0)));
    }
    // Quick switches.
    v.push_back(vHeading(tr(L"快速開關")));
    {
        const int top = pluggedRefresh();
        v.push_back(vToggle(tr(L"用電池時 60 Hz"), top > 60 ? tr(L"插電自動回到 ") + std::to_wstring(top) + L" Hz" : std::wstring(tr(L"插電自動回到最高更新率")), refreshAuto, RefreshAutoId));
        DWORD boost = 0;
        if (bs::lab::readKnob(bs::lab::Knob::cpuBoost, boost) == ERROR_SUCCESS)
            v.push_back(vToggle(tr(L"CPU Boost（用電池時）"), tr(L"關閉可省 2–5 W，短暫運算稍慢"), boost != 0, CpuBoostToggleId));
        v.push_back(vToggle(tr(L"動態省電"), tr(L"依你正在做的事自動調整"), dynamicEnabled, DynamicToggleId));
        if (asusAvailable && h.keyboardDevice) v.push_back(vAction(tr(L"鍵盤背光"), h.keyboard ? std::wstring(keyboardLevelName(*h.keyboard)) : std::wstring(tr(L"未設定")), HwKeyboardMenuId));
    }
    v.push_back(vNote(tr(L"風扇與功耗只提供原廠範圍內的預設：風扇曲線取自韌體各模式自己的曲線，功耗上限不超過平衡模式。每次切換效能模式後會自動重新套用，選「原廠」即還原。獨顯只有在已經醒著時才讀取溫度與時脈，不會為了顯示把它叫醒。")));
    showChartPage(InlinePage::performance, tr(L"效能"), perfPageText(), std::move(v));
}
// The ⋯ menu (also the tray icon's right-click menu).
// The ⋯ menu (also the tray icon's right-click menu): quick switches and tools. The
// sections themselves are the tabs at the bottom of the panel.
std::vector<MenuItem> labSingleItems() {
    const bool preview = demo || isolatedPreview;
    const auto labStatus = labController ? labController->status() : bs::lab::Controller::Status{};
    std::vector<MenuItem> single;
    const wchar_t* experimentNames[] = {tr(L"CPU Boost 開／關"), tr(L"處理器能源偏好"), tr(L"螢幕更新率"), tr(L"電源模式")};
    for (int i = 0; i < bs::lab::ManualKnobs; ++i)
        single.push_back(menuItem(LabExperimentId + i, experimentNames[i], labStatus.running && !labStatus.plan && int(labStatus.knob) == i, preview || labStatus.running));
    single.push_back(menuSeparator());
    single.push_back(menuItem(LabStopId, tr(L"停止實驗並還原"), false, !(labStatus.running && !labStatus.plan)));
    return single;
}
std::vector<MenuItem> mainMenuItems() {
    const bool preview = demo || isolatedPreview;
    std::vector<MenuItem> items;
    if (availableUpdate) {
        auto update = menuItem(UpdateInstallId, tr(L"安裝更新 ") + availableUpdate->version, false, updateBusy);
        update.hint = tr(L"新版本"); update.checked = true;
        items.push_back(update); items.push_back(menuSeparator());
    }
    auto dynamic = menuItem(DynamicToggleId, tr(L"動態省電"), dynamicEnabled, preview);
    if (!dynamicEnabled) dynamic.hint = tr(L"關");
    items.push_back(dynamic);
    if (dynamicEnabled && heldCount()) {
        auto handBack = menuItem(ReleaseHoldsId, tr(L"把手動設定交回動態省電"));
        handBack.hint = std::to_wstring(heldCount()) + tr(L" 項");
        items.push_back(handBack);
    }
    items.push_back(menuItem(PowerModeId, tr(L"電源模式")));
    if (asusAvailable) items.push_back(submenu(tr(L"華碩效能模式"), modeMenuItems()));
    items.push_back(menuSeparator());
    std::vector<MenuItem> tools;
    tools.push_back(menuItem(ExportCsvId, tr(L"匯出 CSV"), false, preview));
    tools.push_back(menuItem(ShareCardId, tr(L"分享今日卡片"), false, preview));
    tools.push_back(menuItem(MiniToggleId, tr(L"迷你浮動視窗"), miniEnabled));
    items.push_back(submenu(tr(L"工具"), tools));
    items.push_back(menuSeparator());
    if (hidden) items.push_back(menuItem(TrayShow, tr(L"顯示面板"))); // from the tray icon
    items.push_back(menuItem(TrayExit, tr(L"結束 BatteryScope"), false, pendingAction));
    return items;
}
void openMenu(POINT point) {
    const auto items = mainMenuItems();
    if (const int chosen = showMenu(items, point, mainWindow)) SendMessageW(mainWindow, WM_COMMAND, chosen, 0);
}
void applyLanguage(bool useEnglish) {
    english = useEnglish;
    makeFonts();
    SetWindowTextW(mainWindow, AppTitle);
    const std::pair<HWND, const wchar_t*> labels[] = {{closeButton, tr(L"正常關閉")}, {killButton, tr(L"強制結束…")},
        {serviceButton, tr(L"停止服務…")}, {detailsButton, tr(L"查看詳情")}, {menuButton, tr(L"設定")},
        {hideButton, tr(L"收回")}, {inlineBack, inlinePage == InlinePage::confirm ? tr(L"取消") : tr(L"返回排行")},
        {inlineConfirm, tr(L"確定強制結束")}};
    for (auto pair : labels)
        SetWindowTextW(pair.first, tr(pair.second));
    refreshPowerButton();
    SetWindowTextW(backgroundCheck, backgroundOnly ? tr(L"範圍：僅背景") : tr(L"範圍：全部程式"));
    SetWindowTextW(allCheck, showAll ? tr(L"顯示：全部程序") : tr(L"顯示：前 10 名"));
    SetWindowTextW(list, tr(L"CPU 耗電排行"));
    for (int i = 0; i < 6; ++i) SetWindowTextW(modeButtons[i], modeName(PowerMode(i % 3)));
    SetWindowTextW(restoreModesButton, tr(L"還原原本設定"));
    SetWindowTextW(energySettingsButton, tr(L"Windows 節能設定"));
    if (inlinePage == InlinePage::process && inlineProcess) showDetails(*inlineProcess);

    if (inlinePage == InlinePage::ledger) showLedger();
    if (inlinePage == InlinePage::powerMode) showPowerModePage();
    if (inlinePage == InlinePage::history) showHistoryPage();
    if (inlinePage == InlinePage::perfApps) showPerformanceApps();
    if (inlinePage == InlinePage::lab) showLabPage();
    if (inlinePage == InlinePage::settings) showSettingsPage();
    if (inlinePage == InlinePage::dynamic) showDynamicPage();
    if (inlinePage == InlinePage::goal) showGoalPage();
    if (inlinePage == InlinePage::confirm && pendingForceRow) showForceConfirmation(*pendingForceRow);
    notice = tr(L"正在讀取電池與程序資料…"); noticeUntil = 0;
    if (tooltip) SendMessageW(tooltip, TTM_POP, 0, 0);
    refreshList(); InvalidateRect(mainWindow, nullptr, FALSE);
    if (trayAdded) updateTray(false);
}
bool saveLanguage() { return writeSetting(L"Language", english ? 1 : 0); }
std::wstring controlTip(HWND control) {
    if (control == inlineBack) return inlinePage == InlinePage::confirm ? tr(L"取消強制結束（Esc）") : tr(L"返回程序排行（Esc）");
    if (control == inlineConfirm) return tr(L"確認後只結束所選程序；未儲存資料可能遺失");
    if (control == powerButton) return tr(L"Windows 目前生效的電源模式；點擊可分別設定插電時與用電池時的模式");
    if (control == hideButton) return tr(L"收回面板（Esc），背景監測持續運作");
    if (control == menuButton) return tr(L"設定：主題、語言、累計耗能、匯出、低電量提醒與快速鍵");
    if (control == backgroundCheck) return tr(L"只列出沒有可見視窗的程序與服務");
    if (control == allCheck) return tr(L"切換前 10 名與全部程序；滾輪可捲動");
    if (control == detailsButton) return tr(L"查看完整名稱、路徑、耗電範圍與操作限制（Enter）");
    if (!selection() || !IsWindowEnabled(control)) return selectionHint();
    if (control == closeButton) return tr(L"請求程式正常關閉；仍可出現儲存提示");
    if (control == killButton) return tr(L"強制結束前會再次確認，未儲存資料可能遺失");
    return tr(L"選擇宿主內要停止的服務，不結束整個宿主");
}
LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == taskbarCreated && taskbarCreated) { trayAdded = updateTray(true); if (!trayAdded && hidden) showMain(); return 0; }
    switch (message) {
    case WM_CREATE: {
        mainWindow = window;
        dpi = GetDpiForWindow(window);
        if (!refreshIcons()) return -1;
        readMotionPreference();
        viewport = CreateWindowExW(WS_EX_CONTROLPARENT, L"STATIC", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_VSCROLL,
            0, 0, 0, 0, window, nullptr, nullptr, nullptr);
        SetWindowSubclass(viewport, viewportProc, 1, 0);
        list = CreateWindowExW(0, WC_LISTVIEWW, tr(L"CPU 耗電排行"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOCOLUMNHEADER | LVS_OWNERDRAWFIXED,
            0, 0, 0, 0, viewport, reinterpret_cast<HMENU>(ListId), nullptr, nullptr);
        SetWindowSubclass(list, listProc, 1, 0);
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        const wchar_t* titles[] = {tr(L"程序與 CPU 估計瓦數")};
        for (int i = 0; i < int(std::size(titles)); ++i) {
            LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT; column.pszText = const_cast<wchar_t*>(titles[i]);
            column.cx = px(120); column.fmt = i >= 3 && i <= 6 ? LVCFMT_RIGHT : LVCFMT_LEFT; ListView_InsertColumn(list, i, &column);
        }
        auto button = [&](const wchar_t* text, int id, DWORD style = BS_OWNERDRAW) {
            HWND control = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(INT_PTR(id)), nullptr, nullptr);
            SetWindowSubclass(control, buttonProc, 1, 0); return control;
        };
        closeButton = button(tr(L"正常關閉"), CloseId); killButton = button(tr(L"強制結束…"), KillId);
        serviceButton = button(tr(L"停止服務…"), ServiceId); detailsButton = button(tr(L"查看詳情"), DetailsId);
        backgroundCheck = button(tr(L"範圍：全部程式"), BackgroundId);
        allCheck = button(tr(L"顯示：前 10 名"), AllId);
        powerButton = button(tr(L"電源模式"), PowerModeId);
        for (int i = 0; i < 6; ++i) { modeButtons[i] = button(modeName(PowerMode(i % 3)), ModeAcEfficiencyId + i); ShowWindow(modeButtons[i], SW_HIDE); }
        restoreModesButton = button(tr(L"還原原本設定"), RestoreModesId); ShowWindow(restoreModesButton, SW_HIDE);
        energySettingsButton = button(tr(L"Windows 節能設定"), EnergySaverSettingsId); ShowWindow(energySettingsButton, SW_HIDE);
        ecoButton = button(tr(L"效率模式"), EcoToggleId); ShowWindow(ecoButton, SW_HIDE);
        perfAppButton = button(tr(L"加入效能清單"), PerfAppToggleId); ShowWindow(perfAppButton, SW_HIDE);
        menuButton = button(tr(L"設定"), MenuId); hideButton = button(tr(L"收回"), HideId);
        inlineText = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            0, 0, 0, 0, window, nullptr, nullptr, nullptr);
        SetWindowSubclass(inlineText, inlineTextProc, 1, 0);
        {
            WNDCLASSEXW vc{}; vc.cbSize = sizeof(vc); vc.lpfnWndProc = visualProc; vc.hInstance = GetModuleHandleW(nullptr);
            vc.lpszClassName = L"BatteryScope.Visual.v1"; vc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            RegisterClassExW(&vc);
            visualView = CreateWindowExW(0, vc.lpszClassName, L"", WS_CHILD | WS_TABSTOP, 0, 0, 0, 0, window, nullptr, nullptr, nullptr);
        }
        SendMessageW(inlineText, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(sp(4), sp(4)));
        settingsList = CreateWindowExW(0, L"LISTBOX", L"", WS_CHILD | WS_VSCROLL | LBS_OWNERDRAWVARIABLE | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(INT_PTR(SettingsListId)), nullptr, nullptr);
        inlineBack = button(tr(L"返回排行"), InlineBackId);
        inlineConfirm = button(tr(L"確定強制結束"), InlineConfirmId);
        ShowWindow(inlineBack, SW_HIDE);
        ShowWindow(inlineConfirm, SW_HIDE);
        tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, window, nullptr, nullptr, nullptr);
        SetWindowTheme(tooltip, L"", L"");
        SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, px(300));
        SendMessageW(tooltip, TTM_SETDELAYTIME, TTDT_INITIAL, 550);
        for (auto control : {closeButton, killButton, serviceButton, detailsButton, backgroundCheck, allCheck,
            powerButton, menuButton, hideButton, inlineBack, inlineConfirm}) {
            TOOLINFOW tool{}; tool.cbSize = sizeof(tool); tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            tool.hwnd = window; tool.uId = reinterpret_cast<UINT_PTR>(control); tool.lpszText = LPSTR_TEXTCALLBACKW;
            SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        }
        autoStart = startupEnabled();
        makeFonts(); layout(); updateButtons(); refreshPowerButton();
        DWORD corners = 2;
        DwmSetWindowAttribute(window, 33, &corners, sizeof(corners));
        applyTheme();
        taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        trayAdded = updateTray(true);
        if (hotkeyEnabled && !demo && !isolatedPreview && !registerPanelHotkey(true)) hotkeyEnabled = false;
        if (!demo && !isolatedPreview) loadAutoOverrides();
        if (miniEnabled && !isolatedPreview) setMiniWindow(true);
        if (!demo && !isolatedPreview) effectiveWatch = watchEffectivePowerMode([](EffectiveMode mode, void* target) {
            PostMessageW(static_cast<HWND>(target), PowerModeMessage, WPARAM(mode), 0);
        }, window);
        refreshPowerButton();
        if (!demo && !isolatedPreview) SetTimer(window, UpdateTimer, 60 * 1000, nullptr); // first check a minute after start
        if (!demo && !isolatedPreview) {
            asusAvailable = bs::asus::Acpi().available();
            if (asusAvailable) {
                // 0.18 kept the charge limit in the main settings; it now lives with the other ASUS settings.
                if (const DWORD old = readSetting(L"ChargeLimit", 0)) { if (!bs::asus::remembered().chargeLimit) bs::asus::setChargeLimit(old); writeSetting(L"ChargeLimit", 0); }
                reapplyAsus(); asusKeys.start(window, AsusKeyMessage); updateKeyboardTimer(); updateFnKeys();
            }
            restoreGovernorAfterCrash(); loadAppRules(); refreshDetectorRules(); updateSensing();
        }
        if (demo || isolatedPreview) asusAvailable = true; // the preview shows the hardware menu
        return 0;
    }
    case WM_MEASUREITEM:
        if (wp == ListId) { reinterpret_cast<MEASUREITEMSTRUCT*>(lp)->itemHeight = px(RowHeight); return TRUE; }
        if (wp == SettingsListId) {
            auto measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
            measure->itemHeight = measure->itemID < std::size(settingRows) && settingRows[measure->itemID].row == Row::section ? sp(34) : sp(46);
            return TRUE;
        }
        break;
    case WM_DRAWITEM: {
        auto item = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (item->CtlID == ListId) drawProcess(*item);
        else if (item->CtlID == SettingsListId) drawSettingRow(*item);
        else if (item->CtlType == ODT_BUTTON) drawButton(*item);
        return TRUE;
    }
    case WM_DPICHANGED: {
        scrollPosition = scrollPosition * HIWORD(wp) / dpi;
        dpi = HIWORD(wp); refreshIcons(); makeFonts(); auto r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(window, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(list, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        layout(); return 0;
    }
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) hideMain(); else layout(); return 0;
    case WM_TIMER:
        if (wp == MotionTimer) { stepMotion(motionNow()); return 0; }
        if (wp == NoticeTimer) { KillTimer(window, NoticeTimer); noticeUntil = 0; InvalidateRect(window, nullptr, FALSE); return 0; }
        if (wp == UpdateTimer) { SetTimer(window, UpdateTimer, 6 * 3600 * 1000, nullptr); maybeCheckForUpdate(); return 0; }
        if (wp == GovernorTimer) { governorTick(); return 0; }
        if (wp == KeyboardTimer) { keyboardTick(); return 0; }
        if (wp == AsusTimer) { KillTimer(window, AsusTimer); reapplyAsus(); asusKeys.stop(); asusKeys.start(window, AsusKeyMessage); return 0; }
        break;
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
        applyTheme(); return 0;
    case WM_SETTINGCHANGE:
        if (wp == SPI_SETHIGHCONTRAST || (lp && CompareStringOrdinal(reinterpret_cast<const wchar_t*>(lp), -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL))
            applyTheme();
        readMotionPreference();
        if (!canAnimate()) {
            panelMotion.duration = 0;
            if (scrollMotion.duration) { applyScroll(scrollMotion.to); stopScroll(); }
            for (auto& row : rowMotions) row.bar.duration = row.hover.duration = row.selected.duration = 0;
            for (auto& button : buttonMotions) button.second.duration = 0;
            for (auto& button : pressMotions) button.second.duration = 0;
            stepMotion(motionNow());
        }
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{}; HDC target = BeginPaint(window, &ps); RECT r{}; GetClientRect(window, &r);
        HDC dc = CreateCompatibleDC(target); HBITMAP bitmap = CreateCompatibleBitmap(target, r.right, r.bottom);
        HGDIOBJ old = SelectObject(dc, bitmap); paint(dc, r); BitBlt(target, 0, 0, r.right, r.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc); EndPaint(window, &ps); return 0;
    }
    case WM_CTLCOLORLISTBOX:
        if (reinterpret_cast<HWND>(lp) == settingsList) { SetDCBrushColor(reinterpret_cast<HDC>(wp), Surface); return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH)); }
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN:
        if (reinterpret_cast<HWND>(lp) == inlineText) {
            SetTextColor(reinterpret_cast<HDC>(wp), Ink);
            SetBkColor(reinterpret_cast<HDC>(wp), Surface);
            SetDCBrushColor(reinterpret_cast<HDC>(wp), Surface);
            return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
        }
        SetBkColor(reinterpret_cast<HDC>(wp), Background); SetDCBrushColor(reinterpret_cast<HDC>(wp), Background);
        return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR*>(lp)->hwndFrom == tooltip && reinterpret_cast<NMHDR*>(lp)->code == TTN_GETDISPINFOW) {
            auto info = reinterpret_cast<NMTTDISPINFOW*>(lp);
            tooltipText = controlTip(reinterpret_cast<HWND>(info->hdr.idFrom)); info->lpszText = tooltipText.data(); return 0;
        }
        if (reinterpret_cast<NMHDR*>(lp)->idFrom == ListId) {
            if (reinterpret_cast<NMHDR*>(lp)->code == LVN_GETEMPTYMARKUP) {
                auto empty = reinterpret_cast<NMLVEMPTYMARKUP*>(lp); empty->dwFlags = EMF_CENTERED;
                wcscpy_s(empty->szMarkup, backgroundOnly ? tr(L"目前沒有符合篩選的程序") : tr(L"正在讀取程序…")); return TRUE;
            }
            if (reinterpret_cast<NMHDR*>(lp)->code == LVN_ITEMCHANGED && !refreshingList) { updateButtons(); updateRowFeedback(); }
            if (reinterpret_cast<NMHDR*>(lp)->code == NM_DBLCLK) { auto row = selection(); if (row) showDetails(*row); releaseInteraction(); }
        }
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        // A command from a section page usually changes what it shows: redraw it afterwards.
        // Only for clicks and menu commands: the page's own text box reports changes as commands too.
        if (sectionPage(inlinePage) && HIWORD(wp) == 0 && reinterpret_cast<HWND>(lp) != inlineText) PostMessageW(window, SectionRefreshMessage, 0, 0);
        if (id == AppRulesMenuId) { if (!demo && !isolatedPreview) showAppRulesMenu(); return 0; }
        if (id == LabSingleMenuId) {
            if (const int chosen = showMenu(labSingleItems(), menuAnchor(), mainWindow)) SendMessageW(window, WM_COMMAND, chosen, 0);
            return 0;
        }
        if (id == LanguageZhId || id == LanguageEnId) {
            applyLanguage(id == LanguageEnId);
            if (!saveLanguage()) setNotice(tr(L"語言已切換，但無法儲存設定。"));
            return 0;
        }
        if (id == PowerDetailsId) { showPowerDetails(); return 0; }
        if (id == LedgerId) { showLedger(); return 0; }
        if (id == ExportCsvId) { if (!demo && !isolatedPreview) chooseCsvExport(); return 0; }
        if (id >= LowBatteryOffId && id <= LowBattery30Id) {
            if (!demo && !isolatedPreview) setLowBatteryThreshold(unsigned(id - LowBatteryOffId) * 10);
            return 0;
        }
        if (id == HistoryId) { showHistoryPage(); return 0; }
        if (id == MascotToggleId) {
            showMascot = !showMascot;
            if (!writeSetting(L"Mascot", showMascot ? 1 : 0)) setNotice(tr(L"無法儲存設定。"));
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (id == ShareCardId) { if (!demo && !isolatedPreview) chooseShareCard(); return 0; }
        if (id == MiniToggleId) {
            miniEnabled = !miniEnabled;
            setMiniWindow(miniEnabled);
            if (!writeSetting(L"MiniWindow", miniEnabled ? 1 : 0)) setNotice(tr(L"無法儲存設定。"));
            return 0;
        }
        if (id == PerfAppsPageId) { showPerformanceApps(); return 0; }
        if (id >= AutoSaverOffId && id <= AutoSaver30Id) {
            if (demo || isolatedPreview) return 0;
            static const unsigned values[] = {0, 20, 30};
            autoSaverThreshold = values[id - AutoSaverOffId];
            if (!writeSetting(L"AutoSaver", autoSaverThreshold)) setNotice(tr(L"無法儲存設定。"));
            else setNotice(autoSaverThreshold ? tr(L"用電池且電量降到 ") + std::to_wstring(autoSaverThreshold) + tr(L"% 時會自動切到效率。") : tr(L"已關閉低電量自動省電。"));
            applyAutoPowerRules();
            return 0;
        }
        if (id == ClearPerfAppsId) {
            if (demo || isolatedPreview) return 0;
            performanceApps.clear(); writeNames(L"PerformanceApps", performanceApps); refreshDetectorRules();
            setNotice(tr(L"已清空效能清單。")); applyAutoPowerRules();
            return 0;
        }
        if (id == EcoToggleId) {
            if (demo || isolatedPreview || !inlineProcess) return 0;
            const ProcessRow row = *inlineProcess;
            auto found = std::find_if(efficientProcesses.begin(), efficientProcesses.end(),
                [&](const EfficientProcess& p) { return p.pid == row.pid && p.created == row.created; });
            DWORD priority = found != efficientProcesses.end() ? found->priority : 0;
            const bool enable = found == efficientProcesses.end();
            const DWORD result = setProcessEfficiency(row, enable, priority);
            if (result == ERROR_SUCCESS || (!enable && result == ERROR_NOT_FOUND)) {
                if (enable) efficientProcesses.push_back({row.pid, row.created, priority}); else efficientProcesses.erase(found);
                governor.forget(row.pid, row.created); // the user's choice: dynamic saving leaves this program alone
                setNotice(enable ? tr(L"已開啟效率模式：降低優先順序並以省電方式執行。") : tr(L"已取消效率模式。"));
            } else setNotice(tr(L"無法變更效率模式：") + errorText(result));
            showDetails(row); refreshList();
            return 0;
        }
        if (id == PerfAppToggleId) {
            // The program's rule, chosen from the same five as Settings → App rules.
            if (demo || isolatedPreview || !inlineProcess || inlineProcess->name.empty()) return 0;
            const ProcessRow row = *inlineProcess;
            const std::wstring name = lowerName(row.name);
            const auto current = performanceApps.count(name) ? bs::gov::AppRule::performance : detector.ruleFor(name);
            std::vector<MenuItem> items;
            for (int r = 0; r < int(bs::gov::AppRule::count); ++r) items.push_back(menuItem(1000 + r, tr(bs::gov::ruleKey(bs::gov::AppRule(r))), current == bs::gov::AppRule(r)));
            RECT button{}; GetWindowRect(perfAppButton, &button);
            const int chosen = showMenu(items, {button.left, button.bottom}, mainWindow);
            if (chosen >= 1000) {
                saveAppRule(name, bs::gov::AppRule(chosen - 1000));
                refreshDetectorRules();
                setNotice(name + L"：" + tr(bs::gov::ruleKey(bs::gov::AppRule(chosen - 1000))));
            }
            showDetails(row);
            return 0;
        }
        if (id == SleepAlertId) {
            if (demo || isolatedPreview) return 0;
            sleepAlerts = !sleepAlerts;
            if (!writeSetting(L"SleepAlert", sleepAlerts ? 1 : 0)) setNotice(tr(L"無法儲存設定。"));
            else setNotice(sleepAlerts ? tr(L"睡眠期間每小時掉電超過 1.5% 時會通知。") : tr(L"已關閉睡眠耗電提醒。"));
            return 0;
        }
        if (id >= ChargeReminderOffId && id <= ChargeReminder100Id) {
            if (demo || isolatedPreview) return 0;
            static const unsigned values[] = {0, 80, 85, 90, 100};
            chargeReminderTarget = values[id - ChargeReminderOffId]; chargeReminder.reset();
            if (!writeSetting(L"ChargeReminder", chargeReminderTarget)) setNotice(tr(L"無法儲存設定。"));
            else setNotice(chargeReminderTarget ? tr(L"充電到 ") + std::to_wstring(chargeReminderTarget) + tr(L"% 時會通知。") : tr(L"已關閉充電提醒。"));
            return 0;
        }
        if (id == DrainAlertId) {
            if (demo || isolatedPreview) return 0;
            drainAlerts = !drainAlerts; drainNotified.clear();
            if (!writeSetting(L"DrainAlert", drainAlerts ? 1 : 0)) setNotice(tr(L"無法儲存設定。"));
            else setNotice(drainAlerts ? tr(L"背景程式持續高耗電（約 2 W 以上、10 分鐘）時會通知。") : tr(L"已關閉背景耗電提醒。"));
            return 0;
        }
        if (id == HistoryToggleId) {
            if (demo || isolatedPreview) return 0;
            historyEnabled = !historyEnabled;
            if (!writeSetting(L"History", historyEnabled ? 1 : 0)) setNotice(tr(L"無法儲存設定。"));
            else setNotice(historyEnabled ? tr(L"已開始記錄每日歷史。") : tr(L"已停止記錄；既有紀錄保留，可另外清除。"));
            return 0;
        }
        if (id == ClearHistoryId) {
            if (demo || isolatedPreview) return 0;
            if (MessageBoxW(window, tr(L"刪除這台電腦上所有 BatteryScope 歷史紀錄？此動作無法復原。"), L"BatteryScope", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
                clearHistoryRequest = true; wake.notify_one();
                setNotice(tr(L"已清除歷史紀錄。"));
            }
            return 0;
        }
        if (id == TrayPercentId) {
            if (demo || isolatedPreview) return 0;
            trayPercent = !trayPercent;
            if (!writeSetting(L"TrayPercent", trayPercent ? 1 : 0)) setNotice(tr(L"無法儲存設定。"));
            if (trayAdded) updateTray(false);
            return 0;
        }
        if (id == HotkeyToggleId) {
            if (demo || isolatedPreview) return 0;
            const bool wanted = !hotkeyEnabled;
            if (!registerPanelHotkey(wanted)) { setNotice(tr(L"Ctrl+Alt+B 已被其他程式使用，未啟用快速鍵。")); return 0; }
            hotkeyEnabled = wanted;
            if (!writeSetting(L"Hotkey", hotkeyEnabled ? 1 : 0)) setNotice(tr(L"無法儲存設定。"));
            else setNotice(hotkeyEnabled ? tr(L"已啟用快速鍵 Ctrl+Alt+B。") : tr(L"已停用快速鍵。"));
            return 0;
        }
        if (id == StylePixelId || id == StyleSmoothId) { setStyle(id == StylePixelId); return 0; }
        if (id == ThemeSystemId || id == ThemeDarkId || id == ThemeLightId) {
            setTheme(id == ThemeSystemId ? ui::ThemeChoice::system : id == ThemeDarkId ? ui::ThemeChoice::dark : ui::ThemeChoice::light);
            return 0;
        }
        if (id == EnergySaverSettingsId) {
            if (demo || isolatedPreview) return 0;
            auto opened = ShellExecuteW(window, L"open", L"ms-settings:batterysaver-settings", nullptr, nullptr, SW_SHOWNORMAL);
            if (reinterpret_cast<INT_PTR>(opened) <= 32)
                opened = ShellExecuteW(window, L"open", L"ms-settings:powersleep", nullptr, nullptr, SW_SHOWNORMAL);
            if (reinterpret_cast<INT_PTR>(opened) <= 32) setNotice(tr(L"無法開啟 Windows 節能設定。"));
            return 0;
        }
        if (id == ReviewCandidatesId) { showReviewCandidates(); return 0; }
        if (id == PowerModeId) {
            // The mode pill in the header opens the power overview while dynamic saving runs.
            if (reinterpret_cast<HWND>(lp) == powerButton && dynamicEnabled) { showMain(); showGoalPage(); return 0; }
            showPowerModePage(); return 0;
        }
        if (isModeButton(UINT(id))) {
            if (demo || isolatedPreview) return 0;
            const bool pluggedIn = id < ModeDcEfficiencyId;
            const auto mode = PowerMode((id - ModeAcEfficiencyId) % 3);
            const DWORD result = setPowerMode(pluggedIn, mode);
            if (result) setNotice(tr(L"無法變更電源模式：") + errorText(result));
            else if (!pluggedIn && dynamicEnabled) {
                // A choice made here wins over dynamic saving for the rest of this battery session.
                governor.hold(bs::gov::Setting::powerMode);
                setNotice(tr(L"已將「用電池時」設為") + std::wstring(modeName(mode)) + tr(L"；這次用電池期間動態省電不會改它，按「交給動態省電」可交回。"));
            } else setNotice((pluggedIn ? tr(L"已將「插電時」設為") : tr(L"已將「用電池時」設為")) + std::wstring(modeName(mode)) + tr(L"。"));
            refreshPowerModeControls();
            return 0;
        }
        if (id == RestoreModesId && powerModeHeld()) {
            governor.release(bs::gov::Setting::powerMode); governorTick();
            setNotice(tr(L"電源模式已交給動態省電。")); refreshPowerModeControls();
            return 0;
        }
        if (id == RestoreModesId) {
            if (demo || isolatedPreview || dynamicEnabled) return 0;
            const DWORD result = restorePowerModes();
            setNotice(result ? tr(L"無法還原電源模式：") + errorText(result) : tr(L"已還原 BatteryScope 變更前的電源模式。"));
            refreshPowerModeControls();
            return 0;
        }
        if (id == InlineBackId || (id == IDCANCEL && inlinePage != InlinePage::none)) { closeInlinePage(); return 0; }
        if (id == InlineConfirmId) {
            if (inlinePage != InlinePage::confirm || !pendingForceRow || demo || pendingAction) return 0;
            ProcessRow target = *pendingForceRow;
            closeInlinePage(true); startAction(target, true); return 0;
        }
        if (id == IDCANCEL || id == HideId) { hideMain(); return 0; }
        if (id == MenuId) { RECT r{}; GetWindowRect(menuButton, &r); openMenu({r.left, r.bottom}); return 0; }
        if (id == TrayShow) { showMain(); return 0; }
        if (id == LabPageId) { showLabPage(); return 0; }
        if (id == SettingsPageId) { showMain(); showSettingsPage(); return 0; }
        if (id == DynamicPageId) { showMain(); showDynamicPage(); return 0; }
        if (id == GoalPageId) { showMain(); showGoalPage(); return 0; }
        if (id == HardwarePageId) { showMain(); showHardwarePage(); return 0; }
        if (id == PerfPageId) { showMain(); showPerformancePage(); return 0; }
        if (id == FanMenuId || id == PowerLimitMenuId) {
            if (!asusAvailable) return 0;
            if (const int chosen = showMenu(id == FanMenuId ? fanMenuItems() : powerLimitMenuItems(), menuAnchor(), mainWindow)) SendMessageW(window, WM_COMMAND, chosen, 0);
            return 0;
        }
        if ((id >= FanPreset0Id && id <= FanPreset0Id + 3) || (id >= PowerPreset0Id && id <= PowerPreset0Id + 3)) {
            if (demo || isolatedPreview || !asusAvailable) return 0;
            auto t = bs::asus::tuning();
            if (id >= PowerPreset0Id) t.power = bs::asus::PowerPreset(id - PowerPreset0Id); else t.fans = bs::asus::FanPreset(id - FanPreset0Id);
            // Writing fans and limits takes up to 3 s (a return to factory re-sets the mode): off the UI thread.
            HWND target = window;
            std::thread([target, t] { PostMessageW(target, TuningDoneMessage, bs::asus::setTuning(t), 0); }).detach();
            setNotice(tr(L"正在套用…"));
            return 0;
        }
        if (id == CpuBoostToggleId) {
            if (demo || isolatedPreview) return 0;
            DWORD boost = 0;
            if (bs::lab::readKnob(bs::lab::Knob::cpuBoost, boost) == ERROR_SUCCESS && bs::lab::applyKnob(bs::lab::Knob::cpuBoost, boost ? 0 : 2) == ERROR_SUCCESS) {
                if (dynamicEnabled) governor.hold(bs::gov::Setting::cpuBoost); // a choice made here holds for this battery session
                setNotice(boost ? tr(L"用電池時 CPU Boost 已關閉。") : tr(L"用電池時 CPU Boost 已開啟。"));
            }
            return 0;
        }
        if (id == HwModeMenuId || id == HwGpuMenuId || id == HwChargeMenuId || id == HwKeyboardMenuId || id == HwKeysMenuId || id == HwServicesMenuId || id == HwSlashMenuId) {
            if (!asusAvailable) return 0;
            const auto items = id == HwModeMenuId ? modeMenuItems() : id == HwGpuMenuId ? gpuMenuItems() : id == HwChargeMenuId ? chargeMenuItems() :
                id == HwKeyboardMenuId ? keyboardMenuItems() : id == HwKeysMenuId ? keysMenuItems() : id == HwSlashMenuId ? slashMenuItems() : servicesMenuItems();
            if (const int chosen = showMenu(items, menuAnchor(), mainWindow)) SendMessageW(window, WM_COMMAND, chosen, 0);
            return 0;
        }
        if ((id >= Keyboard0Id && id <= Keyboard0Id + 3) || id == KeyboardAutoOffId || id == KeysHandleId || id == RogOpenId || id == RogModeId || id == RogNoneId
            || id == FnLockId || id == FnCLockId || id == FnCNoneId) {
            if (demo || isolatedPreview || !asusAvailable) return 0;
            if (id == KeyboardAutoOffId) {
                keyboardAutoOff = !keyboardAutoOff; writeSetting(L"KeyboardAutoOff", keyboardAutoOff ? 1 : 0); updateKeyboardTimer();
                setNotice(keyboardAutoOff ? tr(L"用電池時閒置 30 秒會關閉鍵盤背光，一碰就恢復。") : tr(L"鍵盤背光不再自動關閉。"));
            } else if (id == KeysHandleId) {
                asusKeysHandled = !asusKeysHandled; writeSetting(L"AsusKeysOff", asusKeysHandled ? 0 : 1); updateFnKeys();
                setNotice(asusKeysHandled ? tr(L"華碩軟體不在時，BatteryScope 會處理亮度、背光、觸控板、麥克風、Fn 鎖定、效能模式與 ROG 鍵。") : tr(L"華碩按鍵交回華碩軟體處理。"));
            } else if (id == RogOpenId || id == RogModeId || id == RogNoneId) {
                rogKeyAction = id == RogOpenId ? 0 : id == RogModeId ? 1 : 2; writeSetting(L"RogKey", rogKeyAction);
            } else if (id == FnCLockId || id == FnCNoneId) {
                fnCAction = id == FnCLockId ? 0 : 1; writeSetting(L"FnCKey", fnCAction);
            } else if (id == FnLockId) {
                toggleFnLock();
            } else {
                keyboardDimmed = false;
                const DWORD status = bs::asus::setKeyboardBacklight(unsigned(id - Keyboard0Id));
                setNotice(status == ERROR_SUCCESS ? tr(L"鍵盤背光：") + std::wstring(keyboardLevelName(unsigned(id - Keyboard0Id))) : tr(L"鍵盤沒有接受這個設定：") + errorText(status));
                asusReadAt = 0;
            }
            if (inlinePage == InlinePage::hardware) showHardwarePage();
            return 0;
        }
        if (id == ServicesStopId || id == ServicesStartId || id == ServicesDefaultsId) {
            if (demo || isolatedPreview || !asusAvailable) return 0;
            if (id == ServicesStopId && MessageBoxW(mainWindow, tr(L"停止並停用 Armoury Crate 與華碩背景服務（和 G-Helper 停止的相同）？\n\nArmoury Crate、MyASUS 的部分功能會停止運作，直到你在這裡選「恢復成華碩預設」。效能模式、獨顯、充電上限、鍵盤背光與 Fn 鍵改由 BatteryScope 處理。觸控板與系統控制驅動不受影響。需要管理員權限。"),
                L"BatteryScope", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return 0;
            runAsusServices(id == ServicesStopId ? ServicesAction::stop : id == ServicesDefaultsId ? ServicesAction::defaults : ServicesAction::start);
            setNotice(tr(L"正在處理華碩服務…"));
            return 0;
        }
        if (id == SlashToggleId || id == SlashBatteryId || (id >= SlashBright1Id && id <= SlashBright1Id + 2) || (id >= SlashIntervalId && id <= SlashIntervalId + 5)
            || (id >= SlashModeId && id < SlashModeId + int(bs::asus::slashModes().size()))) {
            if (demo || isolatedPreview || !asusAvailable) return 0;
            auto slash = bs::asus::slashSettings();
            if (id == SlashToggleId) slash.on = !slash.on;
            else if (id == SlashBatteryId) slash.offOnBattery = !slash.offOnBattery;
            else if (id >= SlashModeId) { slash.mode = bs::asus::slashModes()[size_t(id - SlashModeId)]; slash.on = true; }
            else if (id >= SlashIntervalId) slash.interval = unsigned(id - SlashIntervalId);
            else slash.brightness = unsigned(id - SlashBright1Id) + 1;
            const DWORD status = bs::asus::setSlash(slash);
            setNotice(status == ERROR_SUCCESS ? (slash.on ? tr(L"Slash 燈效：") + std::wstring(bs::asus::slashModeName(slash.mode)) : std::wstring(tr(L"Slash 燈效已關閉")))
                : tr(L"燈效控制器沒有回應：") + errorText(status));
            if (inlinePage == InlinePage::hardware) showHardwarePage();
            return 0;
        }
        if (id == AsusAutoId || id == AsusSilentId || id == AsusBalancedId || id == AsusTurboId || id == GpuAutoId || id == GpuEcoId || id == GpuStandardId ||
            id == Charge60Id || id == Charge80Id || id == Charge100Id || id == OverdriveId) {
            if (demo || isolatedPreview || !asusAvailable) return 0;
            DWORD status = ERROR_SUCCESS; std::wstring done;
            if (id == AsusAutoId) {
                governor.release(bs::gov::Setting::firmwareMode); governorTick();
                done = tr(L"效能模式交給動態省電：輕度使用靜音，需要時平衡。");
            } else if (id == AsusSilentId || id == AsusBalancedId || id == AsusTurboId) {
                const auto mode = id == AsusSilentId ? bs::asus::Mode::silent : id == AsusTurboId ? bs::asus::Mode::turbo : bs::asus::Mode::balanced;
                status = bs::asus::setMode(mode); done = tr(L"效能模式：") + std::wstring(tr(bs::asus::modeKey(mode)));
                if (status == ERROR_SUCCESS && dynamicEnabled) { governor.hold(bs::gov::Setting::firmwareMode); done += tr(L"（這次用電池期間動態省電不改它）"); }
            } else if (id == GpuAutoId) {
                gpuAuto = !gpuAuto; writeSetting(L"GpuAuto", gpuAuto ? 1 : 0);
                done = gpuAuto ? tr(L"獨顯自動：用電池時 Eco，插電時標準。") : tr(L"獨顯自動已關閉。");
                gpuAutoDone = -1; applyGpuAuto();
            } else if (id == GpuEcoId) {
                gpuAuto = false; writeSetting(L"GpuAuto", 0);
                requestGpuEco(true, false); return 0; // finishes after checking which programs use it
            } else if (id == GpuStandardId) {
                gpuAuto = false; writeSetting(L"GpuAuto", 0);
                status = bs::asus::setGpuEco(false); done = tr(L"獨顯已切到標準");
            } else if (id == OverdriveId) { const bool on = !(asusNow().overdrive.value_or(false)); status = bs::asus::setOverdrive(on); done = on ? tr(L"螢幕 Overdrive 已開啟") : tr(L"螢幕 Overdrive 已關閉"); }
            else {
                const unsigned percent = id == Charge60Id ? 60 : id == Charge80Id ? 80 : 100;
                status = bs::asus::setChargeLimit(percent); done = tr(L"充電上限：") + std::to_wstring(percent) + L"%"; // remembered and re-applied after restarts
            }
            asusReadAt = 0;
            setNotice(status == ERROR_SUCCESS ? done : tr(L"韌體沒有接受這個設定：") + errorText(status));
            if (inlinePage == InlinePage::hardware) showHardwarePage();
            return 0;
        }
        if (id == GoalCycleId) {
            if (demo || isolatedPreview) return 0;
            // 10/7 W -> 12/8 W -> 8/6 W -> off -> 10/7 W
            if (goalUse == 10) { goalUse = 12; goalStill = 8; } else if (goalUse == 12) { goalUse = 8; goalStill = 6; }
            else if (goalUse == 8) { goalUse = 0; goalStill = 0; } else { goalUse = 10; goalStill = 7; }
            writeSetting(L"GoalUse", goalUse); writeSetting(L"GoalStill", goalStill);
            return 0;
        }
        if (id == ReleaseHoldsId) { governor.releaseAll(); governorTick(); setNotice(tr(L"手動設定已交回動態省電。")); return 0; }
        if (id == DynamicToggleId) { if (!demo && !isolatedPreview) setDynamic(!dynamicEnabled); return 0; }
        if (id == RefreshAutoId || id == DarkModeId || id == AutoSaverCycleId) {
            if (demo || isolatedPreview) return 0;
            if (id == RefreshAutoId) {
                refreshAuto = !refreshAuto; writeSetting(L"RefreshAuto", refreshAuto ? 1 : 0);
                refreshAutoSource = -1; refreshAutoLeft = 0;
                if (refreshAuto) applyRefreshAuto();
                else if (const int top = pluggedRefresh()) bs::lab::applyKnob(bs::lab::Knob::refreshRate, DWORD(top)); // back to the full rate
                setNotice(refreshAuto ? tr(L"用電池時自動切到 60 Hz，插電回到最高更新率。") : tr(L"螢幕更新率不再自動切換。"));
            } else if (id == DarkModeId) {
                DWORD dark = 0;
                if (bs::lab::readKnob(bs::lab::Knob::darkMode, dark) == ERROR_SUCCESS && bs::lab::applyKnob(bs::lab::Knob::darkMode, dark ? 0 : 1) == ERROR_SUCCESS)
                    setNotice(dark ? tr(L"已切回淺色模式。") : tr(L"已切到深色模式：OLED 螢幕上深色畫面較省電。"));
            } else {
                autoSaverThreshold = autoSaverThreshold == 0 ? 20 : autoSaverThreshold == 20 ? 30 : 0;
                writeSetting(L"AutoSaver", autoSaverThreshold); applyAutoPowerRules();
            }

            return 0;
        }
        if (id == DynamicRefreshId || id == DynamicEcoId || id == DynamicTargetId) {
            if (demo || isolatedPreview) return 0;
            if (id == DynamicRefreshId) { dynamicRefresh = !dynamicRefresh; writeSetting(L"DynamicRefresh", dynamicRefresh); }
            if (id == DynamicEcoId) { dynamicEco = !dynamicEco; writeSetting(L"DynamicEco", dynamicEco); }
            if (id == DynamicTargetId) { dynamicTarget = dynamicTarget == 0 ? 3 : dynamicTarget == 3 ? 5 : dynamicTarget == 5 ? 8 : 0; writeSetting(L"DynamicTarget", dynamicTarget); }
            return 0;
        }
        if (id == SettingsListId && HIWORD(wp) == LBN_SELCHANGE) {
            const int index = int(SendMessageW(settingsList, LB_GETCURSEL, 0, 0));
            SendMessageW(settingsList, LB_SETCURSEL, WPARAM(-1), 0);
            activateSettingRow(index);
            return 0;
        }
        if (id == LabRecordId && labController) {
            const bool on = !labController->status().recording;
            labController->setRecording(on);
            if (!writeSetting(L"LabRecording", on ? 1 : 0)) setNotice(tr(L"無法儲存設定。"));
            wake.notify_one(); // switch to the two-second cadence now
            showLabPage();
            return 0;
        }
        if (id >= LabExperimentId && id < LabExperimentId + bs::lab::ManualKnobs && labController) {
            const DWORD result = labController->start(bs::lab::Knob(id - LabExperimentId));
            setNotice(result ? tr(L"無法開始實驗：") + errorText(result) : std::wstring(tr(L"實驗開始：用電池時每 2 分鐘輪流切換設定。")));
            wake.notify_one();
            showLabPage();
            return 0;
        }
        if (id == LabStopId && labController) { labController->stop(); setNotice(tr(L"實驗已停止，設定已還原。")); showLabPage(); return 0; }
        if (id == LabExportId) { exportLab(); return 0; }
        if ((id == LabAutoId || id == LabAutoStartId || id == LabQuickId || id == LabQuickStartId) && labController && labEngine) {
            if (autoTestActive && (id == LabAutoStartId || id == LabQuickStartId)) return 0; // already running
            if (autoTestActive) { // second click: stop
                labController->stop(); labEngine->stop(); autoTestActive = false; updateSensing(); flushDetectionLog();
                setNotice(tr(L"自動測試已停止，設定已還原。"));
                std::thread([target = mainWindow] { bs::lab::writeSystemSnapshot(bs::lab::labFolder()); PostMessageW(target, LabExportMessage, 1, 0); }).detach();
                return 0;
            }
            const bool browser = !bs::lab::WorkloadEngine::browserPath().empty();
            const bool quick = id == LabQuickId || id == LabQuickStartId;
            const DWORD result = labController->startPlan(quick ? bs::lab::quickPlan(browser) : bs::lab::automaticPlan(browser));
            if (result) { setNotice(tr(L"無法開始自動測試：") + errorText(result)); showLabPage(); return 0; }
            autoTestActive = true;
            detectionTally = {}; detectionLog.clear(); lastTallyMs = 0;
            { std::error_code ignored; std::filesystem::remove(std::filesystem::path(bs::lab::labFolder()) / L"detection-log.csv", ignored); }
            updateSensing();
            hideMain();
            labEngine->start(labController.get());
            wake.notify_one();
            return 0;
        }
        if (id == LabFolderId) {
            const std::wstring folder = bs::lab::labFolder();
            if (!folder.empty()) ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        if (id == UpdateCheckId) {
            updateChecks = !updateChecks;
            if (!writeSetting(L"UpdateCheck", updateChecks ? 1 : 0)) setNotice(tr(L"無法儲存設定。"));
            if (updateChecks) maybeCheckForUpdate();
            return 0;
        }
        if (id == UpdateNowId) { checkForUpdate(true); setNotice(tr(L"正在檢查更新…")); return 0; }
        if (id == UpdateAutoId || id == UpdateNotifyId || id == UpdateOffId) {
            if (demo || isolatedPreview) return 0;
            updateChecks = id != UpdateOffId; updateAuto = id == UpdateAutoId;
            const bool saved = writeSetting(L"UpdateCheck", updateChecks ? 1 : 0) && writeSetting(L"UpdateAuto", updateAuto ? 1 : 0);
            if (!saved) setNotice(tr(L"無法儲存設定。"));
            else setNotice(id == UpdateAutoId ? tr(L"有新版本時會自動安裝（測試進行中會等測試結束）。") : id == UpdateNotifyId ? tr(L"有新版本時會通知你。") : tr(L"不再自動檢查更新。"));
            if (updateChecks) maybeCheckForUpdate();
            if (inlinePage == InlinePage::settings) InvalidateRect(settingsList, nullptr, FALSE);
            return 0;
        }
        if (id == UpdatePageId) { openReleasePage(); return 0; }
        if (id == UpdateInstallId) { installUpdate(); return 0; }
        if (id == TrayExit) { if (!pendingAction) DestroyWindow(window); else setNotice(tr(L"請等待目前操作完成後再結束。")); return 0; }
        if (id == BackgroundId || id == AllId) {
            stopScroll(); scrollPosition = 0; listRefreshPending = false;
            if (id == BackgroundId) backgroundOnly = !backgroundOnly; else showAll = !showAll;
            SetWindowTextW(backgroundCheck, backgroundOnly ? tr(L"範圍：僅背景") : tr(L"範圍：全部程式"));
            SetWindowTextW(allCheck, showAll ? tr(L"顯示：全部程序") : tr(L"顯示：前 10 名"));
            refreshList(); InvalidateRect(window, nullptr, FALSE); return 0;
        }
        if (id == StartupId) {
            if (demo) return 0;
            DWORD result = setStartup(!autoStart); autoStart = startupEnabled();
            if (result) setNotice(errorText(result));
            else setNotice(autoStart ? tr(L"已設定登入時啟動至系統匣。") : tr(L"已取消登入時啟動。"));
            return 0;
        }
        auto row = selection(); if (!row) return 0;
        if (id == DetailsId) { showDetails(*row); releaseInteraction(); return 0; }
        if (pendingAction || demo) return 0;
        if (id == CloseId && row->actionable && row->window) startAction(*row, false);
        if (id == KillId && row->actionable) {
            showForceConfirmation(*row);
        }
        if (id == ServiceId && !row->services.empty()) chooseService(*row);
        releaseInteraction();
        return 0;
    }
    case RefreshMessage:
        if (!hidden && listRefreshPending && !deferListRefresh()) refreshList();
        return 0;
    case SampleMessage: {
        {
            std::lock_guard<std::mutex> lock(mailboxMutex);
            if (!mailboxFresh) return 0;
            auto processes = std::move(current.processes);
            current = std::move(mailbox);
            if (!mailboxProcesses) current.processes = std::move(processes);
            mailboxFresh = mailboxProcesses = false;
        }
        if (!hidden) {
            if (deferListRefresh()) listRefreshPending = true;
            else refreshList();
            InvalidateRect(window, nullptr, FALSE);
        }
        if (trayAdded) updateTray(false);
        if (demo || isolatedPreview) refreshPowerButton(); // the preview mode follows the power source
        if (inlinePage == InlinePage::lab && !hidden && GetTickCount64() - labPageShownAt >= 10000) showLabPage();
        if (autoTestActive && labController && labEngine) {
            const bool aborted = labEngine->abortRequested();
            if (aborted) labController->stop();
            if (aborted || !labController->status().running) {
                // Finished or stopped: close the test window, export everything, show the results.
                labEngine->stop(); autoTestActive = false; updateSensing(); flushDetectionLog();
                // A snapshot of power plans, battery report, hardware and processes goes with the export;
                // it runs after the test so it never disturbs a measurement.
                setNotice(tr(L"正在收集系統資訊並匯出…"));
                std::thread([target = mainWindow, aborted] {
                    bs::lab::writeSystemSnapshot(bs::lab::labFolder());
                    PostMessageW(target, LabExportMessage, aborted, 0);
                }).detach();
            }
        }
        if (!current.history.empty()) recordCpuPower(current.history.back().ms, current.energy);
        if (!demo && !isolatedPreview && !current.history.empty() && lowBattery.update(current.history.back(), lowBatteryThreshold))
            showLowBatteryNotice();
        learnFromSample();
        ++sampleTick;
        if (miniWindow) InvalidateRect(miniWindow, nullptr, FALSE);
        checkDrainAlert();
        checkBatteryNotices();
        applyAutoPowerRules();
        recordGoalSample();
        applyGpuAuto();
        applyRefreshAuto();
        if (inlinePage == InlinePage::hardware && !hidden) showHardwarePage();
        if (inlinePage == InlinePage::performance && !hidden) showPerformancePage();
        if (inlinePage == InlinePage::goal && !hidden && GetTickCount64() - goalPageShownAt >= 10000) showGoalPage();
        return 0;
    }
    case ActionMessage: {
        pendingAction = false; if (actionWorker.joinable()) actionWorker.join();
        if (!wp) {
            setNotice(serviceAction ? tr(L"服務已停止。") : forceAction ? tr(L"程序已結束。") : tr(L"已傳送關閉要求；程式可能等待儲存確認。"));
            actionMarks.push_back(GetTickCount64()); while (actionMarks.size() > 100) actionMarks.pop_front();
        } else {
            std::wstring text = wp == ERROR_NOT_FOUND ? tr(L"程序已結束或身分已變更，未執行操作。") :
                wp == ERROR_DEPENDENT_SERVICES_RUNNING ? tr(L"有其他執行中的服務依賴此服務，未停止。") :
                wp == ERROR_ACCESS_DISABLED_BY_POLICY ? tr(L"此服務屬於受保護的系統功能，未停止。") :
                wp == ERROR_CANCELLED ? tr(L"已取消管理員授權。") : tr(L"操作未完成：") + errorText(DWORD(wp));
            setNotice(text);
            showInlinePage(InlinePage::error, tr(L"操作未完成"), text);
        }
        resetSamples = true; wake.notify_one(); updateButtons(); return 0;
    }
    case WM_INPUT: {
        RAWINPUT raw{}; UINT size = sizeof(raw);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) != UINT(-1)) onRawInput(raw);
        return DefWindowProcW(window, message, wp, lp);
    }
    case GovernorNowMessage: governorTick(); return 0;
    case WM_MOUSEMOVE: {
        RECT client{}; GetClientRect(window, &client);
        const int hover = tabAt({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, client);
        if (hover != tabHover) {
            tabHover = hover;
            RECT bar{0, client.bottom - tabBarHeight(), client.right, client.bottom}; InvalidateRect(window, &bar, FALSE);
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0}; TrackMouseEvent(&tracking);
        }
        break;
    }
    case WM_MOUSELEAVE:
        if (tabHover >= 0) { tabHover = -1; RECT client{}; GetClientRect(window, &client); RECT bar{0, client.bottom - tabBarHeight(), client.right, client.bottom}; InvalidateRect(window, &bar, FALSE); }
        break;
    case GpuUsersMessage: finishGpuEco(reinterpret_cast<std::vector<std::wstring>*>(lp)); return 0;
    case AsusKeyMessage: handleAsusKey(unsigned(wp), lp != 0); return 0;
    case TuningDoneMessage:
        setNotice(wp == ERROR_SUCCESS ? std::wstring(tr(L"風扇與功耗設定已套用。")) : wp == ERROR_NOT_SUPPORTED ? std::wstring(tr(L"這台電腦不支援這個設定。"))
            : tr(L"韌體沒有接受這個設定，已回到原廠。"));
        asusReadAt = 0;
        if (inlinePage == InlinePage::performance) showPerformancePage();
        return 0;
    case PopupClickMessage:
        showMain();
        if (BalloonTarget(wp) == BalloonTarget::candidates) showReviewCandidates();
        else if (BalloonTarget(wp) == BalloonTarget::powerDetails) showPowerDetails();
        else if (BalloonTarget(wp) == BalloonTarget::update) installUpdate();
        return 0;
    case OsdMessage: {
        const int level = int(lp);
        if (wp == OsdBrightness) showOsd(level >= 0 ? tr(L"螢幕亮度 ") + std::to_wstring(level) + L"%" : std::wstring(tr(L"螢幕亮度沒有回應")), level >= 0 ? level : -1, theme.accent);
        else if (wp == OsdMicrophone) showOsd(level < 0 ? std::wstring(tr(L"找不到麥克風")) : level ? tr(L"麥克風：靜音") : tr(L"麥克風：開啟"), -1, level > 0 ? theme.warn : Green);
        else if (wp == OsdTouchpad) showOsd(level < 0 ? std::wstring(tr(L"觸控板：切換")) : level ? tr(L"觸控板：開") : tr(L"觸控板：關"));
        return 0;
    }
    case AsusServicesMessage: {
        asusServicesBusy = false; asusServicesAt = 0; asusRunning.at = 0;
        const auto action = ServicesAction(lp);
        if (wp == ERROR_SUCCESS)
            setNotice(action == ServicesAction::stop ? tr(L"華碩背景服務已停止並停用；華碩按鍵改由 BatteryScope 處理。")
                : action == ServicesAction::defaults ? tr(L"華碩服務已恢復成預設並啟動。") : tr(L"華碩服務已恢復停止前的設定。"));
        else setNotice(wp == ERROR_CANCELLED ? tr(L"已取消管理員授權。") : tr(L"部分華碩服務沒有變更：") + errorText(DWORD(wp)));
        if (asusAvailable && !demo && !isolatedPreview) { asusKeys.stop(); asusKeys.start(window, AsusKeyMessage); } // greet the keyboard again
        if (inlinePage == InlinePage::hardware) showHardwarePage();
        return 0;
    }
    case SectionRefreshMessage:
        if (inlinePage == InlinePage::goal) showGoalPage();
        else if (inlinePage == InlinePage::history) showHistoryPage();
        else if (inlinePage == InlinePage::hardware) showHardwarePage();
        else if (inlinePage == InlinePage::performance) showPerformancePage();
        else if (inlinePage == InlinePage::lab) showLabPage();
        else if (inlinePage == InlinePage::settings && settingsList) InvalidateRect(settingsList, nullptr, FALSE);
        InvalidateRect(window, nullptr, FALSE); // the tabs
        return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_POWERSETTINGCHANGE && lp) {
            const auto* setting = reinterpret_cast<const POWERBROADCAST_SETTING*>(lp);
            constexpr GUID ConsoleDisplayState = {0x6FE69556, 0x704A, 0x47A0, {0x8F, 0x24, 0xC2, 0x8D, 0x93, 0x6F, 0xDA, 0x47}};
            if (IsEqualGUID(setting->PowerSetting, ConsoleDisplayState) && setting->DataLength >= sizeof(DWORD)) {
                displayOff = *reinterpret_cast<const DWORD*>(setting->Data) == 0;
                PostMessageW(window, GovernorNowMessage, 0, 0);
            }
            return TRUE;
        }
        if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND || wp == PBT_APMPOWERSTATUSCHANGE) { resetSamples = true; wake.notify_one(); }
        // After sleep the firmware and the keyboard may have forgotten: set things again once they are awake.
        if (wp == PBT_APMRESUMEAUTOMATIC && asusAvailable && !demo && !isolatedPreview) { asusReadAt = 0; SetTimer(window, AsusTimer, 3000, nullptr); }
        return TRUE;
    case PowerModeMessage:
        effectiveMode = EffectiveMode(wp);
        refreshPowerButton();
        if (inlinePage == InlinePage::powerMode) refreshPowerModeControls();
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        const POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        {
            RECT client{}; GetClientRect(window, &client);
            if (const int tab = tabAt(point, client); tab >= 0) {
                const Tab target = visibleTabs()[size_t(tab)];
                if (target != activeTab() || (target == Tab::ranking && inlinePage != InlinePage::none)) SendMessageW(window, WM_COMMAND, tabCommand(target), 0);
                return 0;
            }
        }
        if (PtInRect(&mascotRect, point)) { setNotice(mascotLine()); ++sampleTick; InvalidateRect(window, nullptr, FALSE); return 0; }
        // The battery cards open the battery's history and health.
        if (inlinePage == InlinePage::none && PtInRect(&batteryCardsRect, point)) { SendMessageW(window, WM_COMMAND, HistoryId, 0); return 0; }
        break;
    }
    case WM_HOTKEY:
        if (wp == PanelHotkey) toggleMain();
        else if (int(wp) >= FnKeyHotkey && int(wp) < FnKeyHotkey + 11) handleFnKey(int(wp) - FnKeyHotkey);
        return 0;
    case LabExportMessage:
        exportLab();
        showMain(); showLabPage();
        showBalloon(wp ? tr(L"自動測試已中止") : tr(L"自動測試完成"), tr(L"設定已還原，結果與記錄已匯出到桌面，摘要已複製。"), BalloonTarget::none);
        if (updateDeferred) { updateDeferred = false; installUpdate(); } // an automatic update waited for the test and its export
        return 0;
    case UpdateMessage:
        handleUpdateResult(reinterpret_cast<bs::update::Release*>(lp));
        return 0;
    case UpdateDownloadedMessage:
        finishUpdateDownload(DWORD(wp));
        return 0;
    case TrayMessage:
        if (trayVersion4 ? LOWORD(lp) == NIN_BALLOONUSERCLICK : lp == NIN_BALLOONUSERCLICK) {
            showMain();
            if (balloonTarget == BalloonTarget::candidates) showReviewCandidates();
            else if (balloonTarget == BalloonTarget::powerDetails) showPowerDetails();
            else if (balloonTarget == BalloonTarget::update) installUpdate();
            return 0;
        }
        if ((trayVersion4 && (LOWORD(lp) == NIN_SELECT || LOWORD(lp) == NIN_KEYSELECT)) ||
            (!trayVersion4 && lp == WM_LBUTTONUP)) toggleMain();
        if ((trayVersion4 && LOWORD(lp) == WM_CONTEXTMENU) || (!trayVersion4 && lp == WM_RBUTTONUP)) {
            POINT point{};
            if (trayVersion4) { point = {GET_X_LPARAM(wp), GET_Y_LPARAM(wp)}; if (point.x == -1 && point.y == -1) GetCursorPos(&point); }
            else GetCursorPos(&point);
            openMenu(point);
        }
        return 0;
    case WM_CLOSE: hideMain(); return 0;
    case WM_DESTROY: {
        KillTimer(window, MotionTimer); motionTimerRunning = false;
        KillTimer(window, NoticeTimer); KillTimer(window, UpdateTimer); KillTimer(window, GovernorTimer);
        if (dynamicEnabled && !demo && !isolatedPreview) stopGovernor();
        dynamicEnabled = false; autoTestActive = false; updateSensing();
        asusKeys.stop(); keyWork.stop(); KillTimer(window, KeyboardTimer); KillTimer(window, AsusTimer);
        if (fnKeysRegistered) for (int i = 0; i < 11; ++i) UnregisterHotKey(window, FnKeyHotkey + i);
        closePopups();
        if (keyboardDimmed) bs::asus::setKeyboardBacklight(keyboardBefore);
        if (labEngine) labEngine->stop();
        registerPanelHotkey(false);
        setMiniWindow(false);
        unwatchEffectivePowerMode(effectiveWatch); effectiveWatch = nullptr;
        if (curveDirty) writeBinarySetting(L"ChargeCurve", chargeCurve.save());
        if (autoApplied != AutoMode::none) { undoAutoOverrides(); saveAutoOverrides(); }
        for (const auto& entry : efficientProcesses) {
            ProcessRow row; row.pid = entry.pid; row.created = entry.created; DWORD priority = entry.priority;
            setProcessEfficiency(row, false, priority); // processes that already exited simply fail
        }
        efficientProcesses.clear();
        quit = true; wake.notify_one();
        NOTIFYICONDATAW data{}; data.cbSize = sizeof(data); data.hWnd = window; data.uID = 1;
        if (trayAdded) Shell_NotifyIconW(NIM_DELETE, &data);
        PostQuitMessage(0); return 0;
    }
    }
    return DefWindowProcW(window, message, wp, lp);
}

Snapshot demoSnapshot() {
    Snapshot state; const uint64_t now = GetTickCount64();
    for (int i = 0; i <= 360; ++i) {
        PowerSample p; p.ms = now - uint64_t(360 - i) * 5000; p.present = p.acKnown = true;
        p.watts = 8.5 + std::sin(i / 12.0) * 1.7 + (i > 190 && i < 230 ? 12 : 0);
        p.fullWh = 56; p.remainingWh = 42.6 - i * .007; p.percent = *p.remainingWh / 56 * 100;
        p.designWh = 60; p.cycleCount = 214u; // synthetic: 93% health
        appendPower(state.history, p);
    }
    state.power = calculatePower(state.history);
    const wchar_t* names[] = {L"CloudSync.exe", L"Browser.exe", L"UpdateWorker.exe", L"svchost.exe", L"BatteryScope.exe"};
    for (int i = 0; i < 5; ++i) {
        ProcessRow row; row.pid = 4200 + i; row.created = 1; row.name = names[i];
        row.kind = i == 3 ? L"服務" : i == 1 ? L"應用程式" : L"背景程序";
        row.window = i == 1;
        row.cpu = 8.2 / (i + 1); row.ioMB = 2.5 / (i + 1); row.memoryMB = 180.0 / (i + 1);
        row.activity = ActivityReason::cpu; row.busySeconds = i == 3 ? 0 : 42;
        row.gpuPercent = i == 1 ? 18.0 : 0.0;
        row.measured = row.ioKnown = row.memoryKnown = true;
        state.processes.push_back(row);
    }
    state.energy.ready = true; state.energy.source = EnergySource::perCore; state.energy.coreWatts = 3.2; state.energy.packageWatts = 5.0;
    state.energy.observedCpuCoverage = .82; // Explicitly synthetic preview data.
    state.energy.systemProcessSnapshot = true; state.energy.quality = ObservationQuality::partial; state.energy.sampleSkewMs = 4;
    state.energy.otherSystemWatts = *state.power.currentWatts - 3.2; state.energy.unassignedCpuWatts = .4;
    for (size_t i=0;i<state.processes.size();++i) {
        auto& row=state.processes[i]; row.cpuWatts = row.averageCpuWatts = 1.2 / (i+1);
        row.cpuWattsLow = *row.cpuWatts * .65; row.cpuWattsHigh = *row.cpuWatts * 1.35;
        row.cpuMilliWh = *row.cpuWatts * 100;
    }
    double demoAttributed=0;
    for(const auto& row:state.processes) demoAttributed+=*row.cpuWatts;
    state.energy.unassignedCpuWatts=*state.energy.coreWatts-demoAttributed;
    state.ledger = {{L"CloudSync.exe", 182.4, 1, true}, {L"Browser.exe", 121.7, 6, true}, {L"UpdateWorker.exe", 64.0, 1, true},
        {L"VideoExport.exe", 41.3, 2, false}, {L"svchost.exe", 22.9, 9, true}};
    state.ledgerCoreMilliWh = 610; state.ledgerStartedMs = now - 47 * 60000; // Explicitly synthetic preview data.
    const double demoDays[][3] = {{142, 21.4, 30.2}, {96, 13.8, 20.1}, {0, 0, 12.5}, {188, 27.9, 35.0}, {121, 17.2, 18.4}, {64, 9.6, 14.2}, {47, 6.9, 0}};
    for (int i = 0; i < 7; ++i) {
        if (i == 2) continue; // a day without records
        DayStats day; day.date = localDate(6 - i); day.batteryMinutes = demoDays[i][0]; day.dischargedWh = demoDays[i][1]; day.chargedWh = demoDays[i][2];
        day.apps = {{L"Browser.exe", 820.0 - i * 40}, {L"CloudSync.exe", 410.0 + i * 15}, {L"UpdateWorker.exe", 160}};
        day.sleepMinutes = 420 + i * 10; day.sleepDropPercent = 5 + (i == 4 ? 7 : 0);
        day.healthPercent = 94.2 - i * .15; day.cycles = 208u + unsigned(i);
        state.days.push_back(day);
    }
    SleepRecord sleep; sleep.startMs = now - 8 * 3600000ull; sleep.endMs = now - 3600000ull; sleep.before = 86; sleep.after = 80;
    state.lastSleep = sleep; state.sleepCount = 1;
    state.servicesKnown = true; state.totalProcesses = 5; state.ms = now; return state;
}
bool captureWindow(const std::wstring& path) {
    RECT rect{}; GetClientRect(mainWindow, &rect); int w = rect.right, h = rect.bottom;
    HDC screen = GetDC(mainWindow), dc = CreateCompatibleDC(screen);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr; HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap) { DeleteDC(dc); ReleaseDC(mainWindow, screen); return false; }
    HGDIOBJ old = SelectObject(dc, bitmap);
    SendMessageW(mainWindow, WM_PRINT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
    // The main canvas is owner drawn; WM_PRINTCLIENT supplies its content explicitly.
    paint(dc, rect);
    // Owner-drawn controls are rendered through the same draw functions the
    // window uses, so captures do not depend on WM_PRINT support for them.
    for (auto child : {viewport, closeButton, killButton, serviceButton, detailsButton, backgroundCheck, allCheck,
        powerButton, hideButton, menuButton, inlineText, inlineBack, inlineConfirm, restoreModesButton, energySettingsButton, ecoButton, perfAppButton,
        modeButtons[0], modeButtons[1], modeButtons[2], modeButtons[3], modeButtons[4], modeButtons[5], settingsList, visualView}) {
        if (!child || !(GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE)) continue;
        RECT childRect; GetWindowRect(child, &childRect); MapWindowPoints(nullptr, mainWindow, reinterpret_cast<POINT*>(&childRect), 2);
        const int cw = childRect.right - childRect.left, ch = childRect.bottom - childRect.top;
        SaveDC(dc); SetViewportOrgEx(dc, childRect.left, childRect.top, nullptr);
        IntersectClipRect(dc, 0, 0, cw, ch);
        if (child == visualView) {
            paintVisual(dc, {0, 0, cw, ch});
        } else if (child == settingsList) {
            fill(dc, {0, 0, cw, ch}, Surface);
            const int count = int(SendMessageW(settingsList, LB_GETCOUNT, 0, 0));
            for (int index = 0; index < count; ++index) {
                RECT item{};
                if (SendMessageW(settingsList, LB_GETITEMRECT, WPARAM(index), reinterpret_cast<LPARAM>(&item)) == LB_ERR || item.top >= ch) continue;
                DRAWITEMSTRUCT draw{}; draw.CtlType = ODT_LISTBOX; draw.CtlID = SettingsListId; draw.itemID = UINT(index);
                draw.hwndItem = settingsList; draw.hDC = dc; draw.rcItem = item;
                drawSettingRow(draw);
            }
        } else if (child == viewport) {
            fill(dc, {0, 0, cw, ch}, Background);
            for (int index = 0; index < int(displayed.size()); ++index) {
                RECT item{}; ListView_GetItemRect(list, index, &item, LVIR_BOUNDS);
                MapWindowPoints(list, viewport, reinterpret_cast<POINT*>(&item), 2);
                if (item.bottom <= 0 || item.top >= ch) continue;
                DRAWITEMSTRUCT draw{}; draw.CtlType = ODT_LISTVIEW; draw.CtlID = ListId; draw.itemID = UINT(index);
                draw.hwndItem = list; draw.hDC = dc; draw.rcItem = item;
                if (ListView_GetItemState(list, index, LVIS_SELECTED)) draw.itemState |= ODS_SELECTED;
                drawProcess(draw);
            }
        } else if ((GetWindowLongPtrW(child, GWL_STYLE) & BS_TYPEMASK) == BS_OWNERDRAW && child != inlineText) {
            DRAWITEMSTRUCT draw{}; draw.CtlType = ODT_BUTTON; draw.CtlID = UINT(GetDlgCtrlID(child));
            draw.hwndItem = child; draw.hDC = dc; draw.rcItem = {0, 0, cw, ch};
            if (!IsWindowEnabled(child)) draw.itemState |= ODS_DISABLED;
            if (GetFocus() == child) draw.itemState |= ODS_FOCUS;
            drawButton(draw);
        } else SendMessageW(child, WM_PRINT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_NONCLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
        RestoreDC(dc, -1);
    }
    BITMAPFILEHEADER file{}; file.bfType = 0x4d42; file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER); file.bfSize = file.bfOffBits + w * h * 4;
    std::ofstream output(std::filesystem::path(path), std::ios::binary);
    output.write(reinterpret_cast<char*>(&file), sizeof(file)); output.write(reinterpret_cast<char*>(&info.bmiHeader), sizeof(info.bmiHeader));
    output.write(reinterpret_cast<char*>(pixels), w * h * 4);
    bool ok = output.good(); SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc); ReleaseDC(mainWindow, screen); return ok;
}
int diagnostic(const std::wstring& path) {
    Collector collector; Snapshot state;
    EnergyReader energyReader; CpuAttributor attributor; energyReader.sample();
    state.processes = collector.collect(state.totalProcesses, state.inaccessible); energyReader.sampleThreadPlacement(state.processes, collector.threadSnapshot()); Sleep(2100);
    state.energy = energyReader.sample();
    state.processes = collector.collect(state.totalProcesses, state.inaccessible);
    alignProcessObservation(state.energy, collector.intervalSeconds(), collector.sampleMs(), collector.systemSnapshot());
    energyReader.sampleThreadPlacement(state.processes, collector.threadSnapshot());
    attributor.apply(state.processes,state.energy,collector.sampleMs(),GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    auto power = readPower();
    std::ofstream out{std::filesystem::path(path)};
    out << "BatteryScope live diagnostic\n" << "battery_present=" << power.present << "\nac_known=" << power.acKnown << "\non_ac=" << power.onAc
        << "\nbattery_percent=" << (power.percent ? std::to_string(*power.percent) : "unavailable")
        << "\nbattery_signed_discharge_watts=" << (power.watts ? std::to_string(*power.watts) : "unavailable")
        << "\nremaining_wh=" << (power.remainingWh ? std::to_string(*power.remainingWh) : "unavailable")
        << "\nprocesses=" << state.processes.size() << "\ninaccessible=" << state.inaccessible << "\nservices_known=" << collector.servicesKnown() << "\n";
    unsigned measured = 0, actionable = 0, serviceProcesses = 0;
    for (const auto& row : state.processes) { measured += row.measured; actionable += row.actionable; serviceProcesses += !row.services.empty(); }
    out << "measured_processes=" << measured << "\nactionable_processes=" << actionable << "\nservice_processes=" << serviceProcesses << "\n";
    const char* sourceNames[] = {"none", "per_core", "core_total", "package"};
    OSVERSIONINFOW os{}; os.dwOSVersionInfoSize = sizeof(os);
    using RtlGetVersion = LONG (WINAPI*)(OSVERSIONINFOW*);
    FARPROC versionSymbol = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    RtlGetVersion getVersion = nullptr; std::memcpy(&getVersion, &versionSymbol, sizeof(getVersion));
    if (getVersion) getVersion(&os);
    out << "windows_build=" << os.dwMajorVersion << "." << os.dwMinorVersion << "." << os.dwBuildNumber
        << "\nlogical_processors=" << GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) << "\nprocessor_groups=" << GetActiveProcessorGroupCount()
        << "\nenergy_source=" << sourceNames[int(state.energy.source)] << "\nemi_channel_names=";
    for (const auto& name : energyReader.channelNames()) {
        std::string utf8(size_t(WideCharToMultiByte(CP_UTF8, 0, name.c_str(), int(name.size()), nullptr, 0, nullptr, nullptr)), '\0');
        WideCharToMultiByte(CP_UTF8, 0, name.c_str(), int(name.size()), utf8.data(), int(utf8.size()), nullptr, nullptr);
        out << utf8 << ";";
    }
    out << "\nemi_channels=" << state.energy.channels << "\nenergy_ready=" << state.energy.ready
        << "\ncpu_core_watts=" << (state.energy.coreWatts?std::to_string(*state.energy.coreWatts):"unavailable")
        << "\ncpu_package_watts=" << (state.energy.packageWatts?std::to_string(*state.energy.packageWatts):"unavailable")
        << "\nunassigned_cpu_watts=" << (state.energy.unassignedCpuWatts?std::to_string(*state.energy.unassignedCpuWatts):"unavailable")
        << "\nobserved_cpu_coverage=" << state.energy.observedCpuCoverage << "\ntiming_mismatch=" << state.energy.timingMismatch
        << "\nprocess_interval_seconds=" << state.energy.processIntervalSeconds << "\nsample_skew_ms=" << state.energy.sampleSkewMs
        << "\nsystem_process_snapshot=" << state.energy.systemProcessSnapshot << "\nobservation_quality=" << int(state.energy.quality)
        << "\nprocess_snapshot_ms=" << collector.snapshotDurationMs() << "\n";
    double sum=0; unsigned estimated=0;
    for (const auto& row:state.processes) if(row.cpuWatts) { ++estimated; sum+=*row.cpuWatts; }
    out << "estimated_processes=" << estimated << "\nattributed_cpu_watts=" << sum << "\n";
    return out.good() && measured > 0 && collector.servicesKnown() ? 0 : 1;
}
bool panelSelfTest(const std::wstring& path) {
    std::ofstream out{std::filesystem::path(path)}; bool ok = true;
    auto check = [&](const char* name, bool result) { out << name << "=" << (result ? "PASS" : "FAIL") << "\n"; ok &= result; };
    check("executable_contains_shell_icon", ExtractIconExW(executablePath().c_str(), -1, nullptr, nullptr, 0) == 1);
    bool sizesOK = true;
    for (int size : {16,20,24,32,40,48,64,128,256}) {
        HICON icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
        ICONINFO info{}; BITMAP bitmap{};
        const bool read = icon && GetIconInfo(icon, &info);
        sizesOK &= read && GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap) && bitmap.bmWidth == size && bitmap.bmHeight == size;
        if (info.hbmColor) DeleteObject(info.hbmColor);
        if (info.hbmMask) DeleteObject(info.hbmMask);
        if (icon) DestroyIcon(icon);
    }
    check("all_nine_icon_sizes_load", sizesOK);
    {
        // The custom menu renders every level and keeps the panel's look.
        const auto items = mainMenuItems();
        const auto file = std::filesystem::temp_directory_path() / L"BatteryScope-menu-check.bmp";
        bool menuOK = !items.empty();
        for (int i = -1; i < int(items.size()); ++i)
            if (i < 0 || !items[size_t(i)].children.empty()) menuOK &= captureMenu(items, file.wstring(), i) && std::filesystem::file_size(file) > 1000;
        std::error_code ignored; std::filesystem::remove(file, ignored);
        check("custom_menu_renders_all_levels", menuOK);
    }
    const UINT actualDpi = dpi;
    bool dpiIconsOK = true;
    for (UINT testDpi : {96u,120u,144u,192u}) {
        dpi = testDpi;
        dpiIconsOK &= refreshIcons();
        ICONINFO info{}; BITMAP bitmap{};
        const bool read = GetIconInfo(smallIcon, &info) != FALSE;
        dpiIconsOK &= read && GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap) && bitmap.bmWidth == GetSystemMetricsForDpi(SM_CXSMICON, dpi);
        if (info.hbmColor) DeleteObject(info.hbmColor);
        if (info.hbmMask) DeleteObject(info.hbmMask);
    }
    dpi = actualDpi; refreshIcons();
    check("tray_icon_matches_100_125_150_200_percent_dpi", dpiIconsOK);
    check("window_uses_embedded_large_and_small_icons", reinterpret_cast<HICON>(SendMessageW(mainWindow, WM_GETICON, ICON_BIG, 0)) == appIcon &&
        reinterpret_cast<HICON>(SendMessageW(mainWindow, WM_GETICON, ICON_SMALL, 0)) == smallIcon && appIcon && smallIcon);
    if (!isolatedPreview) {
        check("notification_icon_registered", trayAdded);
        NOTIFYICONIDENTIFIER icon{}; icon.cbSize = sizeof(icon); icon.hWnd = mainWindow; icon.uID = 1;
        RECT rect{}; HRESULT located = Shell_NotifyIconGetRect(&icon, &rect);
        check("notification_icon_rect_available", SUCCEEDED(located));
    }
    check("portrait_tool_window", (GetWindowLongPtrW(mainWindow, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) &&
        !(GetWindowLongPtrW(mainWindow, GWL_STYLE) & WS_CAPTION));
    hidden = true; ShowWindow(mainWindow, SW_HIDE); trayVersion4 = true;
    SendMessageW(mainWindow, TrayMessage, 0, MAKELPARAM(NIN_SELECT, 1));
    check("first_click_opens", IsWindowVisible(mainWindow) && !hidden);
    SendMessageW(mainWindow, TrayMessage, 0, MAKELPARAM(WM_LBUTTONUP, 1));
    check("raw_mouse_event_does_not_double_toggle_v4", IsWindowVisible(mainWindow) && !hidden);
    SendMessageW(mainWindow, TrayMessage, 0, MAKELPARAM(NIN_SELECT, 1));
    check("second_click_hides", !IsWindowVisible(mainWindow) && hidden);
    SendMessageW(mainWindow, TrayMessage, 0, MAKELPARAM(NIN_KEYSELECT, 1));
    check("keyboard_activation_opens", IsWindowVisible(mainWindow) && !hidden);
    SendMessageW(mainWindow, WM_COMMAND, IDCANCEL, 0);
    check("escape_command_hides", !IsWindowVisible(mainWindow) && hidden);
    trayVersion4 = false;
    SendMessageW(mainWindow, TrayMessage, 1, WM_LBUTTONUP);
    check("legacy_click_opens", IsWindowVisible(mainWindow) && !hidden);
    SendMessageW(mainWindow, TrayMessage, 1, WM_LBUTTONUP);
    check("legacy_second_click_hides", !IsWindowVisible(mainWindow) && hidden);
    showMain();
    RECT bounds{}; GetWindowRect(mainWindow, &bounds);
    MONITORINFO screen{}; screen.cbSize = sizeof(screen); GetMonitorInfoW(MonitorFromWindow(mainWindow, MONITOR_DEFAULTTONEAREST), &screen);
    check("panel_inside_work_area", bounds.left >= screen.rcWork.left && bounds.top >= screen.rcWork.top &&
        bounds.right <= screen.rcWork.right && bounds.bottom <= screen.rcWork.bottom);
    RECT first{}; ListView_GetItemRect(list, 0, &first, LVIR_BOUNDS);
    check("readable_card_row_height", first.bottom - first.top == px(RowHeight));
    RECT listBounds{}; GetClientRect(list, &listBounds);
    check("watts_column_fits_viewport", ListView_GetColumnWidth(list, 0) <= listBounds.right && !(GetWindowLongPtrW(list, GWL_STYLE) & WS_HSCROLL));
    check("observation_quality_is_not_labeled_accuracy", qualityText(ObservationQuality::partial) == tr(L"部分觀測"));
    ListView_SetItemState(list, 1, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    auto structuralBefore = listStructuralChanges;
    auto before = selection(); refreshList(); auto after = selection();
    check("refresh_reuses_native_items", listStructuralChanges == structuralBefore);
    if (after) showDetails(*after);
    check("details_open_inside_panel", inlinePage == InlinePage::process && IsWindowVisible(inlineText) &&
        !IsWindowVisible(viewport) && IsWindowVisible(inlineBack));
    int textLength = GetWindowTextLengthW(inlineText);
    std::wstring detailText(size_t(textLength) + 1, L'\0');
    GetWindowTextW(inlineText, detailText.data(), int(detailText.size()));
    check("inline_details_keep_paragraphs", detailText.find(L"\r\n") != std::wstring::npos);
    check("details_offer_efficiency_and_performance_list_read_only_in_demo", IsWindowVisible(ecoButton) && IsWindowVisible(perfAppButton) &&
        !IsWindowEnabled(ecoButton) && !IsWindowEnabled(perfAppButton));
    check("details_estimate_battery_gain_when_closed", after && after->averageCpuWatts &&
        extraMinutesIfStopped(current.power, current.history.back(), *after->averageCpuWatts).has_value() &&
        detailText.find(tr(L"\n若關閉此程式：CPU 部分約可多用 ")) != std::wstring::npos);
    applyLanguage(true);
    check("inline_details_switch_language", inlineTitle == tr(L"程序詳情"));
    applyLanguage(false);
    SendMessageW(mainWindow, WM_COMMAND, IDCANCEL, 0);
    check("escape_returns_to_ranking", inlinePage == InlinePage::none && IsWindowVisible(viewport) && IsWindowVisible(mainWindow));
    showPowerDetails();
    check("power_details_reuse_panel", inlinePage == InlinePage::history && IsWindowVisible(visualView) && !visualBlocks.empty());
    auto pageText = [] {
        std::wstring text(size_t(GetWindowTextLengthW(inlineText)) + 1, L'\0');
        GetWindowTextW(inlineText, text.data(), int(text.size()));
        return text;
    };
    check("power_details_show_last_sleep_and_health_trend", current.lastSleep && pageText().find(sleepText(*current.lastSleep)) != std::wstring::npos &&
        pageText().find(healthTrendText()) != std::wstring::npos && !healthTrendText().empty());
    SendMessageW(mainWindow, WM_COMMAND, InlineBackId, 0);
    showReviewCandidates();
    check("candidate_review_reuses_panel", inlinePage == InlinePage::history && IsWindowVisible(visualView));
    SendMessageW(mainWindow, WM_COMMAND, InlineBackId, 0);
    showLedger();
    std::wstring ledgerText(size_t(GetWindowTextLengthW(inlineText)) + 1, L'\0');
    GetWindowTextW(inlineText, ledgerText.data(), int(ledgerText.size()));
    check("session_energy_reuses_panel", inlinePage == InlinePage::history && IsWindowVisible(visualView) &&
        ledgerText.find(L"CloudSync.exe") != std::wstring::npos && ledgerText.find(tr(L" · 已結束")) != std::wstring::npos);
    SendMessageW(mainWindow, WM_COMMAND, InlineBackId, 0);
    {
        // The percentage icon: generated for the demo battery, sized for the tray, with opaque digits and badge.
        HICON icon = currentTrayIcon(nullptr);
        ICONINFO info{}; BITMAP bitmap{}; size_t opaque = 0, distinct = 0;
        const bool read = icon && icon != smallIcon && GetIconInfo(icon, &info) && GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap);
        if (read) {
            std::vector<DWORD> pixels(size_t(bitmap.bmWidth) * bitmap.bmHeight);
            BITMAPINFO format{}; format.bmiHeader.biSize = sizeof(format.bmiHeader); format.bmiHeader.biWidth = bitmap.bmWidth;
            format.bmiHeader.biHeight = -bitmap.bmHeight; format.bmiHeader.biPlanes = 1; format.bmiHeader.biBitCount = 32;
            HDC screen = GetDC(nullptr);
            GetDIBits(screen, info.hbmColor, 0, UINT(bitmap.bmHeight), pixels.data(), &format, DIB_RGB_COLORS);
            ReleaseDC(nullptr, screen);
            std::set<DWORD> colors;
            for (DWORD pixel : pixels) if (pixel >> 24) { ++opaque; colors.insert(pixel & 0xFFFFFF); }
            distinct = colors.size();
        }
        if (info.hbmColor) DeleteObject(info.hbmColor);
        if (info.hbmMask) DeleteObject(info.hbmMask);
        check("tray_icon_shows_battery_percentage", read && bitmap.bmWidth == GetSystemMetricsForDpi(SM_CXSMICON, dpi) &&
            opaque > size_t(bitmap.bmWidth * bitmap.bmHeight / 2) && distinct >= 3 && currentTrayIcon(nullptr) == icon);
    }
    showHistoryPage();
    std::wstring historyBody(size_t(GetWindowTextLengthW(inlineText)) + 1, L'\0');
    GetWindowTextW(inlineText, historyBody.data(), int(historyBody.size()));
    check("history_page_lists_days_newest_first", inlinePage == InlinePage::history && current.days.size() == 6 &&
        historyBody.find(dayLabel(current.days.back().date) + tr(L"（今天）")) < historyBody.find(dayLabel(current.days.front().date) + tr(L" · 用電池 ")) &&
        historyBody.find(L"Browser.exe") != std::wstring::npos);
    SendMessageW(mainWindow, WM_COMMAND, InlineBackId, 0);
    // Settings page: every option in one list, grouped; clicking in demo changes nothing.
    SendMessageW(mainWindow, WM_COMMAND, SettingsPageId, 0);
    const bool styleBefore = pixelStyle;
    check("settings_page_lists_every_option", inlinePage == InlinePage::settings && IsWindowVisible(settingsList) && !IsWindowVisible(inlineText) &&
        SendMessageW(settingsList, LB_GETCOUNT, 0, 0) == LRESULT(std::size(settingRows)));
    activateSettingRow(int(std::find_if(std::begin(settingRows), std::end(settingRows), [](const SettingRow& r) { return r.row == Row::style; }) - std::begin(settingRows)));
    check("settings_rows_do_not_change_demo_settings", pixelStyle == styleBefore);
    SendMessageW(mainWindow, WM_COMMAND, InlineBackId, 0);
    check("settings_list_hides_on_return", inlinePage == InlinePage::none || inlineClosing || !IsWindowVisible(settingsList));
    SendMessageW(mainWindow, WM_COMMAND, DynamicPageId, 0);
    std::wstring dynamicBody(size_t(GetWindowTextLengthW(inlineText)) + 1, L'\0');
    GetWindowTextW(inlineText, dynamicBody.data(), int(dynamicBody.size()));
    check("dynamic_page_explains_state", inlinePage == InlinePage::goal && dynamicBody.find(L"BatteryScope") == std::wstring::npos && dynamicBody.find(L"G-Helper") != std::wstring::npos);
    SendMessageW(mainWindow, WM_COMMAND, InlineBackId, 0);
    SendMessageW(mainWindow, WM_COMMAND, GoalPageId, 0);
    std::wstring goalBody(size_t(GetWindowTextLengthW(inlineText)) + 1, L'\0');
    GetWindowTextW(inlineText, goalBody.data(), int(goalBody.size()));
    check("goal_page_shows_goal_and_tips", inlinePage == InlinePage::goal && goalBody.find(L"10 W") != std::wstring::npos && goalBody.find(L"1. ") != std::wstring::npos);
    SendMessageW(mainWindow, WM_COMMAND, InlineBackId, 0);
    {
        // The default panel has room for the 30-minute chart, and the list starts below it with no gap.
        RECT client{}; GetClientRect(mainWindow, &client);
        check("default_panel_shows_chart_without_gap", !compactLayout(client.bottom - tabBarHeight()) && listTop() == sp(366));
    }
    // Home: the performance dashboard, with the mode as segments, fans and power presets; the battery tab holds history.
    SendMessageW(mainWindow, WM_COMMAND, PerfPageId, 0);
    check("performance_page_is_its_own_tab", inlinePage == InlinePage::performance && activeTab() == Tab::performance && visibleTabs().front() == Tab::performance
        && std::any_of(visualBlocks.begin(), visualBlocks.end(), [](const VBlock& b) { return b.kind == VBlock::Kind::segments && b.commands.size() >= 3; })
        && std::any_of(visualBlocks.begin(), visualBlocks.end(), [](const VBlock& b) { return b.kind == VBlock::Kind::action && int(b.amount) == FanMenuId; }));
    SendMessageW(mainWindow, WM_COMMAND, HistoryId, 0);
    check("battery_history_belongs_to_the_battery_tab", activeTab() == Tab::ranking);
    SendMessageW(mainWindow, WM_COMMAND, InlineBackId, 0);
    SendMessageW(mainWindow, WM_COMMAND, GoalPageId, 0);
    const auto switches = std::count_if(visualBlocks.begin(), visualBlocks.end(), [](const VBlock& b) { return b.kind == VBlock::Kind::action && b.toggle >= 0; });
    check("saving_tab_switches_each_saving", inlinePage == InlinePage::goal && switches >= 3);
    SendMessageW(mainWindow, WM_COMMAND, InlineBackId, 0);
    // The ASUS page (preview data): every control is a row, and the key display shows without taking focus.
    SendMessageW(mainWindow, WM_COMMAND, HardwarePageId, 0);
    auto hasAction = [](const std::wstring& title) {
        return std::any_of(visualBlocks.begin(), visualBlocks.end(), [&](const VBlock& b) { return b.kind == VBlock::Kind::action && b.title == title; });
    };
    check("hardware_page_offers_every_control", inlinePage == InlinePage::hardware && hasAction(tr(L"效能模式")) && hasAction(tr(L"充電上限"))
        && hasAction(tr(L"鍵盤背光")) && hasAction(tr(L"Fn 鎖定")) && hasAction(tr(L"Slash 燈效")) && hasAction(tr(L"華碩按鍵")) && hasAction(tr(L"華碩服務")));
    SendMessageW(mainWindow, WM_COMMAND, InlineBackId, 0);
    showOsd(L"Test", 50);
    check("key_display_shows_without_focus", osdPopup.window && IsWindowVisible(osdPopup.window) && GetForegroundWindow() != osdPopup.window
        && (GetWindowLongPtrW(osdPopup.window, GWL_EXSTYLE) & WS_EX_NOACTIVATE));
    setNotice(L"Notice test");
    RECT noticeRect{}, panelRect{}; if (noticePopup.window) GetWindowRect(noticePopup.window, &noticeRect); GetWindowRect(mainWindow, &panelRect);
    check("notice_card_rests_above_the_tabs", noticePopup.window && IsWindowVisible(noticePopup.window) && noticeRect.left > panelRect.left
        && noticeRect.right < panelRect.right && noticeRect.bottom <= panelRect.bottom - tabBarHeight());
    closePopups();
    {
        RECT client{}; GetClientRect(mainWindow, &client);
        HDC screen = GetDC(mainWindow), dc = CreateCompatibleDC(screen);
        HBITMAP surface = CreateCompatibleBitmap(screen, client.right, client.bottom);
        HGDIOBJ old = SelectObject(dc, surface); paint(dc, client); SelectObject(dc, old);
        DeleteObject(surface); DeleteDC(dc); ReleaseDC(mainWindow, screen);
        check("mascot_drawn_in_header_and_speaks", showMascot && mascotRect.right > mascotRect.left && !mascotLine().empty());
    }
    {
        wchar_t folder[MAX_PATH]{}; GetTempPathW(MAX_PATH, folder);
        const std::wstring cardPath = std::wstring(folder) + L"BatteryScope-card-" + std::to_wstring(GetCurrentProcessId()) + L".png";
        const bool exported = exportShareCard(cardPath, false);
        std::ifstream card(std::filesystem::path(cardPath), std::ios::binary);
        std::string head(8, '\0'); card.read(head.data(), 8);
        card.seekg(0, std::ios::end);
        check("share_card_saves_png", exported && head == "\x89PNG\r\n\x1a\n" && card.tellg() > 2000);
        card.close(); DeleteFileW(cardPath.c_str());
    }
    SendMessageW(mainWindow, WM_COMMAND, MiniToggleId, 0);
    const bool miniShown = miniWindow && IsWindowVisible(miniWindow) && (GetWindowLongPtrW(miniWindow, GWL_EXSTYLE) & WS_EX_TOPMOST);
    SendMessageW(mainWindow, WM_COMMAND, MiniToggleId, 0);
    check("mini_window_toggles", miniShown && !miniWindow);
    const auto tip = trayText();
    check("tray_tooltip_shows_level_power_and_time", tip.find(L"%") != std::wstring::npos && tip.find(L" W") != std::wstring::npos &&
        tip.find(remainingText(current.power)) != std::wstring::npos && tip.size() < 128);
    const auto savedTheme = themeChoice;
    const COLORREF darkBackground = ui::palette(ui::ThemeChoice::dark).background;
    SendMessageW(mainWindow, WM_COMMAND, ThemeLightId, 0);
    const bool lightApplied = themeChoice == ui::ThemeChoice::light &&
        (theme.highContrast || (!theme.dark && Background != darkBackground)) && ListView_GetBkColor(list) == Background;
    SendMessageW(mainWindow, WM_COMMAND, ThemeDarkId, 0);
    check("theme_switch_updates_palette_and_list", lightApplied && themeChoice == ui::ThemeChoice::dark &&
        (theme.highContrast || theme.dark) && ListView_GetBkColor(list) == Background);
    themeChoice = savedTheme; applyTheme();
    {
        // Every UI string, Chinese and English, must render in the embedded pixel
        // font; run tools/subset-pixel-font.py after changing UI text.
        size_t uncovered = 0;
        for (const auto& entry : translations) uncovered += !ui::pixelCovers(std::wstring(entry.first)) + !ui::pixelCovers(entry.second);
        check("pixel_font_covers_all_ui_strings", ui::pixelFontLoaded() && uncovered == 0 &&
            ui::pixelCovers(L"BatteryScope 0123456789.%W") && !ui::pixelCovers(L"\u0E01"));
        const bool savedStyle = pixelStyle;
        SendMessageW(mainWindow, WM_COMMAND, StyleSmoothId, 0);
        RECT smoothList{}; GetWindowRect(list, &smoothList);
        const bool smoothFonts = !pixelStyle && fallbackFonts.empty() && systemFont == normalFont;
        SendMessageW(mainWindow, WM_COMMAND, StylePixelId, 0);
        RECT pixelList{}; GetWindowRect(list, &pixelList);
        check("style_switch_swaps_fonts_without_moving_layout", smoothFonts && pixelStyle && fallbackFonts.count(normalFont) &&
            systemFont != normalFont && EqualRect(&smoothList, &pixelList));
        if (pixelStyle != savedStyle) setStyle(savedStyle);
    }
    {
        wchar_t folder[MAX_PATH]{}; GetTempPathW(MAX_PATH, folder);
        const std::wstring csvPath = std::wstring(folder) + L"BatteryScope-panel-" + std::to_wstring(GetCurrentProcessId()) + L".csv";
        const auto powerPath = exportCsv(csvPath);
        auto readFile = [](const std::wstring& file) {
            std::ifstream in(std::filesystem::path(file), std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in), {});
        };
        const std::string table = readFile(csvPath), power = powerPath ? readFile(*powerPath) : std::string();
        check("csv_export_writes_ranking_and_history", powerPath && table.rfind("\xEF\xBB\xBFrank,name,pid", 0) == 0 &&
            table.find("CloudSync.exe") != std::string::npos && power.find("seconds_ago,percent") != std::string::npos &&
            std::count(power.begin(), power.end(), '\n') == std::ptrdiff_t(current.history.size() + 1));
        DeleteFileW(csvPath.c_str()); if (powerPath) DeleteFileW(powerPath->c_str());
    }
    if (after) showForceConfirmation(*after);
    RECT compactConfirmation{}; GetWindowRect(mainWindow, &compactConfirmation);
    check("force_confirmation_reuses_panel", inlinePage == InlinePage::confirm &&
        IsWindowVisible(inlineConfirm) && IsWindowVisible(inlineBack) && GetFocus() == inlineBack &&
        compactConfirmation.bottom - compactConfirmation.top == px(320));
    SendMessageW(mainWindow, WM_COMMAND, IDCANCEL, 0);
    RECT restoredConfirmation{}; GetWindowRect(mainWindow, &restoredConfirmation);
    check("force_confirmation_cancel_is_safe", inlinePage == InlinePage::none && !pendingForceRow && !pendingAction);
    check("ranking_height_restored", restoredConfirmation.bottom - restoredConfirmation.top == px(PanelHeight));
    showInlinePage(InlinePage::error, tr(L"操作未完成"), tr(L"此服務目前不接受停止要求。"));
    RECT compactError{}; GetWindowRect(mainWindow, &compactError);
    check("action_error_reuses_panel", inlinePage == InlinePage::error && IsWindowVisible(inlineText) &&
        compactError.bottom - compactError.top == px(360));
    SendMessageW(mainWindow, WM_COMMAND, InlineBackId, 0);
    check("selection_survives_refresh", before && after && before->pid == after->pid && before->created == after->created);
    SendMessageW(mainWindow, WM_COMMAND, BackgroundId, 0);
    check("background_filter", backgroundOnly && displayed.size() == current.processes.size() - 1);
    SendMessageW(mainWindow, WM_COMMAND, AllId, 0);
    check("all_processes_toggle", showAll);
    check("demo_destructive_actions_disabled", !IsWindowEnabled(killButton) && !IsWindowEnabled(serviceButton));
    SendMessageW(mainWindow, WM_COMMAND, PowerModeId, 0);
    check("power_mode_page_shows_both_sources_read_only_in_demo", inlinePage == InlinePage::powerMode &&
        IsWindowVisible(modeButtons[0]) && IsWindowVisible(modeButtons[5]) && !IsWindowEnabled(modeButtons[0]) &&
        !IsWindowEnabled(restoreModesButton) && modeSelected(ModeAcPerformanceId) && modeSelected(ModeDcEfficiencyId) &&
        !modeSelected(ModeAcEfficiencyId) && IsWindowEnabled(powerButton));
    SendMessageW(mainWindow, WM_COMMAND, InlineBackId, 0);
    check("power_mode_page_closes_cleanly", inlinePage == InlinePage::none && !IsWindowVisible(modeButtons[0]) && IsWindowVisible(viewport));
    RECT saverBounds{}; GetWindowRect(powerButton, &saverBounds);
    check("saver_button_visible_in_compact_header", IsWindowVisible(powerButton) &&
        saverBounds.right > saverBounds.left && saverBounds.bottom > saverBounds.top);
    for (int height : {520, PanelHeight}) {
        SetWindowPos(mainWindow, nullptr, 0, 0, px(PanelWidth), px(height), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        RECT listRect{}, actions{}; GetWindowRect(list, &listRect); GetWindowRect(detailsButton, &actions);
        check(height == 520 ? "compact_controls_do_not_overlap" : "full_controls_do_not_overlap",
            listRect.bottom <= actions.top && listRect.bottom - listRect.top >= px(RowHeight));
    }
    hideMain();
    Transition reversible; reversible.move(1, 1000, 220);
    double middle = reversible.value(1080); reversible.move(0, 1080, 150);
    check("animation_reversal_has_no_position_jump", middle > 0 && middle < 1 && std::abs(reversible.value(1080) - middle) < .000001);
    check("animation_reaches_exact_endpoint", reversible.value(1230) == 0 && !reversible.active(1230));
    auto pump = [](uint64_t milliseconds) {
        uint64_t until = GetTickCount64() + milliseconds;
        while (GetTickCount64() < until) {
            MSG event{};
            while (PeekMessageW(&event, nullptr, 0, 0, PM_REMOVE)) {
                // Test gestures use synchronous SendMessage. Do not let an
                // unrelated physical pointer restart hover during idle checks.
                if ((event.message >= WM_MOUSEFIRST && event.message <= WM_MOUSELAST) || event.message == WM_MOUSELEAVE) continue;
                TranslateMessage(&event); DispatchMessageW(&event);
            }
            Sleep(1);
        }
    };
    bool preference = motionEnabled; suppressMotion = false; motionEnabled = true;
    motionTicks.clear(); recordMotion = true;
    showMain();
    check("animated_show_starts_without_blocking", panelMotion.active(motionNow()) && panelTargetShown && IsWindowVisible(mainWindow));
    pump(80);
    BYTE alpha = 255; DWORD flags = 0; GetLayeredWindowAttributes(mainWindow, nullptr, &alpha, &flags);
    check("visible_intermediate_fade_frame", alpha > 0 && alpha < 255);
    hideMain(); pump(30); toggleMain();
    check("rapid_reopen_reverses_exit", panelTargetShown && !hidden && IsWindowVisible(mainWindow) && panelMotion.active(motionNow()));
    pump(320);
    GetLayeredWindowAttributes(mainWindow, nullptr, &alpha, &flags);
    check("animated_open_finishes_opaque", alpha == 255 && IsWindowVisible(mainWindow) && !panelMotion.active(motionNow()));
    showForceConfirmation(current.processes.front());
    check("confirmation_height_animates", panelHeightMotion.active(motionNow()) && inlinePage == InlinePage::confirm);
    pump(240);
    RECT animatedCompact{}; GetWindowRect(mainWindow, &animatedCompact);
    check("confirmation_height_settles", animatedCompact.bottom - animatedCompact.top == px(320));
    SendMessageW(mainWindow, WM_COMMAND, IDCANCEL, 0);
    check("ranking_waits_for_expand", inlineClosing && inlinePage == InlinePage::confirm && panelHeightMotion.active(motionNow()));
    pump(240);
    RECT animatedRestored{}; GetWindowRect(mainWindow, &animatedRestored);
    check("ranking_returns_after_expand", !inlineClosing && inlinePage == InlinePage::none &&
        animatedRestored.bottom - animatedRestored.top == px(PanelHeight) && IsWindowVisible(viewport));
    current.processes.front().averageCpuWatts = 2.0; refreshList();
    check("power_bars_animate_between_samples", std::any_of(rowMotions.begin(), rowMotions.end(), [](const RowMotion& row) { return row.bar.active(motionNow()); }));
    check("numeric_watts_remain_latest_measurement", !displayed.empty() && displayed.front().averageCpuWatts == 2.0);
    pump(300);
    SendMessageW(detailsButton, WM_MOUSEMOVE, 0, MAKELPARAM(4, 4));
    check("button_hover_transition_started", buttonMotions[detailsButton].active(motionNow()));
    SendMessageW(detailsButton, WM_MOUSELEAVE, 0, 0); pump(170);
    check("idle_animation_timer_stops", !motionTimerRunning);
    backgroundOnly = false; showAll = true;
    for (int i = 0; i < 25; ++i) {
        ProcessRow row = current.processes.back(); row.pid = 6000 + i; row.name = L"ScrollTest.exe";
        current.processes.push_back(row);
    }
    refreshList(); pump(300);
    scrollTo(px(RowHeight) * 4 + px(17));
    pump(65);
    check("scroll_has_intermediate_pixel_position", scrollPosition > 0 && scrollPosition < scrollMotion.to && scrollMotion.active(motionNow()));
    RECT itemRect{}; ListView_GetItemRect(list, 0, &itemRect, LVIR_BOUNDS);
    MapWindowPoints(list, viewport, reinterpret_cast<POINT*>(&itemRect), 2);
    check("fractional_scroll_geometry_matches_content", std::abs(itemRect.top + int(std::lround(scrollPosition))) <= 1);
    double prior = scrollPosition; scrollTo(0);
    check("scroll_reverse_does_not_jump", std::abs(scrollPosition - prior) < .001 && scrollMotion.to == 0);
    pump(220);
    check("scroll_reversal_finishes_at_top", scrollPosition == 0 && !scrollMotion.active(motionNow()));
    scrollTo(px(RowHeight) * 3 + px(13), true);
    prior = scrollPosition; refreshList();
    check("refresh_preserves_fractional_scroll", std::abs(scrollPosition - prior) < .001);
    ListView_SetItemState(list, int(scrollPosition / px(RowHeight)), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    const auto selectedBeforeLanguage = selection();
    applyLanguage(true);
    wchar_t label[128]{}; GetWindowTextW(detailsButton, label, 128);
    check("language_switch_updates_controls", std::wstring(label) == L"Details");
    check("language_switch_preserves_scroll", std::abs(scrollPosition - prior) < .001);
    auto selectedAfterLanguage = selection();
    check("language_switch_preserves_selected_identity", selectedBeforeLanguage && selectedAfterLanguage && selectedBeforeLanguage->pid == selectedAfterLanguage->pid && selectedBeforeLanguage->created == selectedAfterLanguage->created);
    check("english_tooltips_are_translated", controlTip(hideButton).find(L"Hide panel") != std::wstring::npos);
    applyLanguage(false); GetWindowTextW(detailsButton, label, 128);
    check("language_switch_returns_to_chinese", std::wstring(label) == L"查看詳情");
    pump(200);
    hoveredRow = 3; updateRowFeedback();
    check("row_hover_transition_started", rowMotions[3].hover.active(motionNow()));
    hoveredRow = -1; updateRowFeedback(); pump(180);
    check("row_hover_settles_without_idle_timer", !motionTimerRunning);
    const auto rowsBeforeHold = displayed.size();
    {
        std::lock_guard<std::mutex> lock(mailboxMutex);
        mailbox = current; mailbox.processes.pop_back(); mailboxFresh = mailboxProcesses = true;
    }
    pointerDown = true; SendMessageW(mainWindow, SampleMessage, 0, 0);
    check("sampling_does_not_replace_rows_during_press", displayed.size() == rowsBeforeHold && listRefreshPending);
    releaseInteraction(); pump(320);
    check("deferred_rows_refresh_after_release", displayed.size() + 1 == rowsBeforeHold && !listRefreshPending);
    const auto processesBeforePowerOnly = current.processes.size();
    {
        std::lock_guard<std::mutex> lock(mailboxMutex);
        mailbox = current; mailbox.processes.clear(); mailboxFresh = true; mailboxProcesses = false;
    }
    SendMessageW(mainWindow, SampleMessage, 0, 0);
    check("power_only_sample_keeps_process_list", processesBeforePowerOnly > 0 && current.processes.size() == processesBeforePowerOnly);
    SendMessageW(mainWindow, SampleMessage, 0, 0);
    check("repeated_sample_message_keeps_snapshot", current.processes.size() == processesBeforePowerOnly && !current.history.empty());
    SetWindowTextW(hideButton, L"test"); applyLanguage(false);
    GetWindowTextW(hideButton, label, 128);
    check("icon_button_has_accessible_name", std::wstring(label) == L"收回");
    auto anchorPid = displayed[size_t(scrollPosition / px(RowHeight))].pid;
    std::swap(current.processes[2], current.processes[3]); refreshList();
    check("refresh_preserves_visible_process_anchor", displayed[size_t(scrollPosition / px(RowHeight))].pid == anchorPid);
    scrollTo(scrollMaximum() + px(500), true);
    ListView_GetItemRect(list, int(displayed.size()) - 1, &itemRect, LVIR_BOUNDS);
    MapWindowPoints(list, viewport, reinterpret_cast<POINT*>(&itemRect), 2);
    RECT clip{}; GetClientRect(viewport, &clip);
    check("bottom_clamps_without_blank_overscroll", scrollPosition == scrollMaximum() && std::abs(itemRect.bottom - clip.bottom) <= 1);
    LVHITTESTINFO hit{}; hit.pt = {px(20), px(12)}; MapWindowPoints(viewport, list, &hit.pt, 1);
    int hitRow = ListView_HitTest(list, &hit);
    check("pixel_scroll_hit_testing_matches_visible_row", hitRow == int((scrollPosition + px(12)) / px(RowHeight)));
    SendMessageW(list, WM_KEYDOWN, VK_HOME, 0); pump(220);
    check("keyboard_home_selects_and_scrolls", selection() && selection()->pid == displayed.front().pid && scrollPosition == 0);
    SendMessageW(list, WM_KEYDOWN, VK_END, 0); pump(220);
    check("keyboard_end_reveals_last_row", selection() && selection()->pid == displayed.back().pid && scrollPosition == scrollMaximum());
    scrollTo(px(RowHeight), true);
    UINT wheelLines = 3; SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &wheelLines, 0);
    SendMessageW(list, WM_MOUSEWHEEL, MAKEWPARAM(0, -15), 0);
    check("partial_wheel_delta_is_not_discarded", wheelLines == 0 || scrollMotion.to > scrollPosition);
    pump(220);
    motionEnabled = false; scrollTo(0);
    check("reduced_motion_scroll_is_immediate", scrollPosition == 0 && !scrollMotion.active(motionNow()));
    motionEnabled = true; scrollTo(px(RowHeight) * 2); pump(40); hideMain();
    check("hide_cancels_pending_scroll", !scrollMotion.active(motionNow()));
    pump(220); showMain(); pump(320);
    scrollTo(px(RowHeight)); pump(240);
    check("scroll_completion_stops_timer", !motionTimerRunning);
    hideMain(); pump(220);
    check("animated_hide_finishes_and_stops_timer", !IsWindowVisible(mainWindow) && hidden && !motionTimerRunning);
    motionEnabled = false; showMain();
    check("reduced_motion_shows_immediately", IsWindowVisible(mainWindow) && !panelMotion.active(motionNow()));
    hideMain();
    check("reduced_motion_hides_immediately", !IsWindowVisible(mainWindow) && !motionTimerRunning);
    out << "animation_steps_observed=" << motionTicks.size() << "\n";
    std::vector<double> intervals;
    for (size_t i = 1; i < motionTicks.size(); ++i) {
        double dt = motionTicks[i] - motionTicks[i - 1]; if (dt > 1 && dt < 100) intervals.push_back(dt);
    }
    if (!intervals.empty()) { std::sort(intervals.begin(), intervals.end()); out << "active_tick_median_ms=" << intervals[intervals.size()/2] << "\n"; }
    recordMotion = false; motionEnabled = preference; suppressMotion = true;
    return ok && out.good();
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    int argc = 0; wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args; for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]); LocalFree(argv);
    if (args.size() == 4 && args[0] == L"--stop-service") {
        if (args[2].empty() || args[3].empty() || args[2].find_first_not_of(L"0123456789") != std::wstring::npos || args[3].find_first_not_of(L"0123456789") != std::wstring::npos) return ERROR_INVALID_PARAMETER;
        try {
            uint64_t pid = std::stoull(args[2]), created = std::stoull(args[3]);
            if (pid > MAXDWORD) return ERROR_INVALID_PARAMETER;
            return int(bs::stopService(unhexName(args[1]), DWORD(pid), created));
        } catch (...) { return ERROR_INVALID_PARAMETER; }
    }
    if (args.size() == 2 && args[0] == L"--diagnose") return diagnostic(args[1]);
    // Self-test of the automatic test's local server and browser launch: runs the
    // engine without a test for the given seconds and writes the port to stdout.
    // Prints what the activity detector sees once a second (no GPU engines, no
    // raw input): a quick check that every signal can be read on this PC.
    if (args.size() == 2 && args[0] == L"--asus-services") {
        // Run elevated by BatteryScope itself (see runAsusServices).
        return int(args[1] == L"stop" ? bs::asus::stopServices(true) : args[1] == L"defaults" ? bs::asus::restoreServiceDefaults() : bs::asus::startServices());
    }
    if (args.size() == 1 && args[0] == L"--asus-dump") {
        // Every ASUS firmware value BatteryScope reads, for checking a new model.
        printf("%s", utf8(bs::asus::dump(bs::asus::read())).c_str());
        return 0;
    }
    if (args.size() == 2 && args[0] == L"--detect-test") {
        bs::lab::SignalReader reader; bs::gov::Detector probe;
        const int seconds = std::clamp(_wtoi(args[1].c_str()), 1, 600);
        for (int i = 0; i < seconds; ++i) {
            const auto o = reader.observe(std::nullopt);
            const auto a = probe.assess(o);
            printf("%s %.2f fg=%s full=%d idle=%.0f fps=%.0f audio=%d fgaudio=%d call=%d locked=%d d3d=%d | %s\n",
                utf8(bs::gov::activityKey(a.activity)).c_str(), a.confidence, utf8(o.foregroundExe).c_str(), int(o.foregroundCoversScreen), o.idleSeconds,
                o.framesPerSecond.value_or(-1), int(o.audio), int(o.foregroundAudio), int(o.call), int(o.locked), int(o.exclusiveFullscreen), utf8(a.reason).c_str());
            fflush(stdout);
            Sleep(1000);
        }
        return 0;
    }
    if (args.size() == 2 && args[0] == L"--lab-engine-test") {
        bs::lab::WorkloadEngine engine;
        engine.start(nullptr);
        printf("port=%d browser=%d\n", engine.port(), int(!bs::lab::WorkloadEngine::browserPath().empty()));
        fflush(stdout);
        Sleep(DWORD(std::clamp(_wtoi(args[1].c_str()), 1, 120)) * 1000);
        const bool aborted = engine.abortRequested();
        engine.stop();
        printf("aborted=%d\n", int(aborted));
        return 0;
    }
    // Used by the installer: close any running copy (any version) cleanly, so its
    // efficiency mode and power mode changes are undone before files are replaced.
    if (args.size() == 1 && args[0] == L"--quit-running") {
        for (int attempt = 0; attempt < 3; ++attempt) {
            HWND running = FindWindowW(L"BatteryScope.Main.v1", nullptr);
            if (!running) return 0;
            DWORD pid = 0; GetWindowThreadProcessId(running, &pid);
            Handle process(OpenProcess(SYNCHRONIZE, FALSE, pid));
            PostMessageW(running, WM_COMMAND, TrayExit, 0);
            if (process) WaitForSingleObject(process, 5000); else Sleep(1000);
        }
        return FindWindowW(L"BatteryScope.Main.v1", nullptr) ? 1 : 0;
    }
    // Used by the installer after copying files: shortcuts, pins and sign-in launch
    // that point to an older BatteryScope.exe now open this one.
    if (args.size() == 1 && args[0] == L"--migrate-shortcuts") { bs::update::migrateShortcuts(executablePath()); return 0; }
    bool screenshot = args.size() >= 2 && (args[0] == L"--screenshot" || args[0] == L"--demo-screenshot");
    bool trayTest = args.size() == 2 && args[0] == L"--tray-test";
    bool uiTest = trayTest || (args.size() == 2 && args[0] == L"--ui-test");
    demo = uiTest || (!args.empty() && (args[0] == L"--demo" || args[0] == L"--demo-screenshot"));
    isolatedPreview = screenshot || (uiTest && !trayTest);
    suppressMotion = screenshot || uiTest;
    auto option = [&](const wchar_t* value) { return std::find(args.begin(), args.end(), value) != args.end(); };
    if (!demo && !isolatedPreview) loadSettings();
    else historyEnabled = false; // previews and tests never read or write history
    if (option(L"--english")) english = true;
    if (option(L"--chinese")) english = false;
    if (option(L"--light")) themeChoice = ui::ThemeChoice::light;
    if (option(L"--dark")) themeChoice = ui::ThemeChoice::dark;
    previewContrast = isolatedPreview && option(L"--high-contrast");
    if (option(L"--smooth")) pixelStyle = false;
    if (option(L"--pixel")) pixelStyle = true;
    theme = ui::palette(themeChoice, previewContrast);
    ui::startGraphics();
    ui::loadPixelFont(instance, PixelFontResource);
    if (isolatedPreview || demo) ClassName = L"BatteryScope.Preview.v3";
    Handle singleton(CreateMutexW(nullptr, FALSE, isolatedPreview || demo ? L"Local\\BatteryScope.Preview.v3" : L"Local\\BatteryScope.Main.v1"));
    if (!singleton) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS && !isolatedPreview && !demo) {
        HWND existing = FindWindowW(ClassName, nullptr);
        // --autotest starts the power lab's automatic test in the copy that is already running.
        if (existing) PostMessageW(existing, WM_COMMAND, option(L"--autotest") ? LabAutoStartId : option(L"--quicktest") ? LabQuickStartId : TrayShow, 0);
        if (existing && !option(L"--autotest") && !option(L"--quicktest")) SetForegroundWindow(existing);
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES}; InitCommonControlsEx(&controls);
    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.hInstance = instance; wc.lpfnWndProc = windowProc;
    wc.lpszClassName = ClassName; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&wc)) return 2;
    if (isolatedPreview || demo) MiniClassName = L"BatteryScope.MiniPreview.v1";
    WNDCLASSEXW mini{}; mini.cbSize = sizeof(mini); mini.hInstance = instance; mini.lpfnWndProc = miniProc;
    mini.lpszClassName = MiniClassName; mini.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
    RegisterClassExW(&mini);
    WNDCLASSEXW popup{}; popup.cbSize = sizeof(popup); popup.hInstance = instance; popup.lpfnWndProc = popupProc;
    if (isolatedPreview || demo) PopupClassName = L"BatteryScope.PopupPreview.v1";
    popup.lpszClassName = PopupClassName; popup.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&popup);
    dpi = GetDpiForSystem();
    RECT bounds{0, 0, px(PanelWidth), px(PanelHeight)};
    if (screenshot && option(L"--compact")) bounds.bottom = px(520);
    if (screenshot && option(L"--minimal")) bounds.bottom = px(460);
    MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor);
    int initialWidth = std::min(bounds.right, monitor.rcWork.right - monitor.rcWork.left - px(20));
    int initialHeight = std::min(bounds.bottom, monitor.rcWork.bottom - monitor.rcWork.top - px(20));
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, ClassName, AppTitle, WS_POPUP | WS_CLIPCHILDREN,
        monitor.rcWork.right - initialWidth - px(10), monitor.rcWork.bottom - initialHeight - px(10), initialWidth, initialHeight, nullptr, nullptr, instance, nullptr);
    if (!window) return 3;
    SetLayeredWindowAttributes(window, 0, 255, LWA_ALPHA);
    if (demo) {
        current = demoSnapshot();
        if (option(L"--charging")) for (size_t i = 0; i < current.history.size(); ++i) {
            auto& p = current.history[i]; p.onAc = p.charging = true; p.watts = -24.0;
            p.remainingWh = 30 + i * .02; p.percent = *p.remainingWh / *p.fullWh * 100;
        }
        if (option(L"--plugged")) for (auto& p : current.history) { p.onAc = true; p.charging = false; p.watts = 0; }
        if (option(L"--desktop")) for (auto& p : current.history) { // a PC without a battery
            p.present = false; p.onAc = true; p.charging = false;
            p.percent.reset(); p.watts.reset(); p.remainingWh.reset(); p.fullWh.reset(); p.designWh.reset(); p.cycleCount.reset();
        }
        if (option(L"--desktop")) for (size_t i = 0; i < current.history.size(); ++i) { // synthetic CPU package power
            CpuEnergyReport sample; sample.ready = true;
            sample.packageWatts = 9 + 6 * std::sin(i / 9.0) + (i % 40 < 6 ? 18 : 0);
            recordCpuPower(current.history[i].ms, sample);
        }
        if (option(L"--empty")) current.processes.clear();
        if (option(L"--no-energy")) { current.energy.ready = false; for (auto& row : current.processes) { row.averageCpuWatts.reset(); row.cpuWattsLow.reset(); row.cpuWattsHigh.reset(); } }
        current.power = calculatePower(current.history);
        if (!current.processes.empty()) efficientProcesses.push_back({current.processes[2].pid, current.processes[2].created, NORMAL_PRIORITY_CLASS});
        performanceApps = {L"browser.exe"}; // synthetic preview list
        for (int band = 0; band < ChargeCurve::Bands; ++band) // synthetic preview curve: tapers after 80%
            for (int i = 0; i < 8; ++i) chargeCurve.add(band * 5 + 2, band < 16 ? 24 : 9);
        refreshList();
    }
    else {
        if (!isolatedPreview) {
            labController = std::make_unique<bs::lab::Controller>();
            labEngine = std::make_unique<bs::lab::WorkloadEngine>();
            if (readSetting(L"LabRecording", 0) == 1) labController->setRecording(true);
        }
        worker = std::thread(sampleLoop);
        if (option(L"--autotest") && labController) PostMessageW(window, WM_COMMAND, LabAutoStartId, 0);
        if (option(L"--quicktest") && labController) PostMessageW(window, WM_COMMAND, LabQuickStartId, 0);
    }
    applyLanguage(english);
    if (!demo && !isolatedPreview && startupState() == StartupState::missingCopy) {
        // A portable folder was moved: point launch at sign-in at this copy. An
        // entry for another copy that still exists is left alone.
        if (setStartup(true) == ERROR_SUCCESS) setNotice(tr(L"已將登入時啟動更新為目前位置。"));
        autoStart = startupEnabled();
    }
    if (screenshot && option(L"--details") && !current.processes.empty()) showDetails(current.processes.front());
    if (screenshot && option(L"--power-details")) showPowerDetails();
    if (screenshot && option(L"--candidates")) showReviewCandidates();
    if (screenshot && option(L"--ledger")) showLedger();
    if (screenshot && option(L"--power-mode")) showPowerModePage();
    if (screenshot && option(L"--history")) showHistoryPage();
    if (screenshot && option(L"--settings")) showSettingsPage();
    if (screenshot && option(L"--dynamic")) showDynamicPage();
    if (screenshot && option(L"--goal")) showGoalPage();
    if (screenshot && option(L"--hardware")) showHardwarePage();
    if (screenshot && option(L"--performance")) showPerformancePage();
    if (screenshot && option(L"--lab")) showLabPage();
    if (demo) for (size_t i = 0; i + 1 < args.size(); ++i) if (args[i] == L"--popups-bmp") {
        const bool saved = capturePopups(args[i + 1]);
        DestroyWindow(window);
        return saved ? 0 : 5;
    }
    if (demo) for (size_t i = 0; i + 1 < args.size(); ++i) if (args[i] == L"--card-png") {
        const bool saved = exportShareCard(args[i + 1], false);
        DestroyWindow(window);
        return saved ? 0 : 5;
    }
    if (screenshot && option(L"--confirm") && !current.processes.empty()) showForceConfirmation(current.processes.front());
    if (screenshot && option(L"--scrolled")) scrollTo(px(17), true);
    if (screenshot && option(L"--selected") && !displayed.empty()) ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    bool trayOnly = !args.empty() && args[0] == L"--tray";
    if (screenshot || uiTest) ShowWindow(window, SW_HIDE);
    else if (trayOnly && trayAdded) { hidden = true; ShowWindow(window, SW_HIDE); }
    else { (void)show; showMain(); }
    UpdateWindow(window);
    if (screenshot) SetTimer(window, 91, demo ? 750 : 6500, nullptr);
    if (trayTest) SetTimer(window, 91, 750, nullptr);
    bool shotOK = true;
    if (uiTest && !trayTest) { shotOK = panelSelfTest(args[1]); DestroyWindow(window); }
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_TIMER && message.wParam == 91) {
            KillTimer(window, 91);
            if (screenshot && (option(L"--menu") || option(L"--menu-lab"))) {
                // The ⋯ menu, or its lab submenu opened in place.
                const auto items = mainMenuItems();
                int open = -1;
                for (int i = 0; option(L"--menu-lab") && i < int(items.size()); ++i) if (items[size_t(i)].label == tr(L"省電實驗室")) open = i;
                shotOK = captureMenu(items, args[1], open);
            } else shotOK = trayTest ? panelSelfTest(args[1]) : captureWindow(args[1]);
            DestroyWindow(window); continue;
        }
        if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    if (worker.joinable()) worker.join();
    if (actionWorker.joinable()) actionWorker.join();
    for (auto f : {normalFont, smallFont, strongFont, titleFont, valueFont, metricFont}) if (f) DeleteObject(f);
    for (auto f : fallbackOwned) DeleteObject(f);
    ui::unloadPixelFont();
    DestroyIcon(appIcon); DestroyIcon(smallIcon); if (headerIcon) DestroyIcon(headerIcon); if (trayIcon) DestroyIcon(trayIcon);
    processIcons.clear();
    ui::stopGraphics();
    return shotOK ? 0 : 4;
}
