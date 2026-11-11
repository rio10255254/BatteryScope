#include "platform.h"
#include "energy.h"
#include "motion.h"
#include "i18n.h"
#include "version.h"
#include "update.h"
#include "lab.h"
#include "ui.h"
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <cwctype>
#include <set>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <sstream>
#include <thread>

namespace {
using namespace bs;
constexpr UINT SampleMessage = WM_APP + 1, TrayMessage = WM_APP + 2, ActionMessage = WM_APP + 3, RefreshMessage = WM_APP + 4,
    PowerModeMessage = WM_APP + 5, UpdateMessage = WM_APP + 6, UpdateDownloadedMessage = WM_APP + 7;
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
    TrayShow = 200, TrayExit = 201;
constexpr int PanelHotkey = 1;
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
enum class InlinePage { none, process, power, candidates, ledger, powerMode, history, perfApps, lab, confirm, error };
unsigned lowBatteryThreshold = 0; // percent; 0 = off
LowBatteryMonitor lowBattery;
bool hotkeyEnabled = false, hotkeyRegistered = false;
bool trayPercent = true;          // show the battery percentage as the notification icon
bool drainAlerts = false;         // notify about sustained high-power background work
std::set<std::pair<uint32_t, uint64_t>> drainNotified;
uint64_t lastDrainAlert = 0;
enum class BalloonTarget { none, candidates, powerDetails, update };
bool updateChecks = true; // daily update check, on unless turned off
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
// The power lab; absent in demo and preview modes.
std::unique_ptr<bs::lab::Controller> labController;
std::unique_ptr<bs::lab::WorkloadEngine> labEngine; // the automatic test's loads and window
bool autoTestActive = false;
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
constexpr UINT_PTR MotionTimer = 92, NoticeTimer = 93, UpdateTimer = 94;
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
int listTop() {
    RECT r{}; GetClientRect(mainWindow, &r);
    return sp(r.bottom < px(480) ? 270 : r.bottom < px(600) ? 308 : 366);
}
void setNotice(std::wstring text) {
    notice = std::move(text); noticeUntil = GetTickCount64() + 15000;
    SetTimer(mainWindow, NoticeTimer, 15000, nullptr);
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
    for (HWND control : {viewport, list, inlineText}) if (control) SetWindowTheme(control, controls, nullptr);
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
}
void hideMain() {
    if (!panelTargetShown && hidden) return;
    if (!trayAdded && !isolatedPreview) trayAdded = updateTray(true);
    if (!trayAdded && !isolatedPreview) { setNotice(tr(L"系統匣無法使用，面板會保持開啟。")); return; }
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
    SetWindowTextW(powerButton, effectiveName(effectiveMode));
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
    const int w = r.right, h = r.bottom, margin = sp(18), top = listTop();
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
    const bool fullPage = inlinePage == InlinePage::process || inlinePage == InlinePage::powerMode || inlinePage == InlinePage::history || inlinePage == InlinePage::lab ||
        inlinePage == InlinePage::confirm || inlinePage == InlinePage::error;
    const int textTop = inlinePage == InlinePage::powerMode ? sp(318) : inlinePage == InlinePage::history ? sp(262) : fullPage ? sp(128) : top - sp(37);
    {
        const int pageLeft = margin + sp(12), pageWidth = inner - sp(24), modeGap = sp(6);
        const int third = (pageWidth - 2 * modeGap) / 3, pageHalf = (pageWidth - modeGap) / 2;
        for (int i = 0; i < 6; ++i)
            MoveWindow(modeButtons[i], pageLeft + (i % 3) * (third + modeGap), i < 3 ? sp(176) : sp(234), third, sp(30), TRUE);
        MoveWindow(restoreModesButton, pageLeft, sp(276), pageHalf, sp(30), TRUE);
        MoveWindow(energySettingsButton, pageLeft + pageHalf + modeGap, sp(276), pageWidth - pageHalf - modeGap, sp(30), TRUE);
    }
    const int textBottom = inlinePage == InlinePage::process ? h - sp(140) : h - sp(94);
    MoveWindow(inlineText, margin + sp(12), textTop, inner - sp(24), std::max(sp(55), textBottom - textTop), TRUE);
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
        page == InlinePage::powerMode || page == InlinePage::history || page == InlinePage::lab ||
        (page == InlinePage::candidates && lines.size() > 500));
    SendMessageW(inlineText, EM_SETSEL, 0, 0);
    SendMessageW(inlineText, EM_SCROLLCARET, 0, 0);
    for (auto control : {viewport, closeButton, killButton, serviceButton, detailsButton, backgroundCheck, allCheck}) ShowWindow(control, SW_HIDE);
    ShowWindow(inlineText, SW_SHOW); ShowWindow(inlineBack, SW_SHOW);
    ShowWindow(inlineConfirm, page == InlinePage::confirm ? SW_SHOW : SW_HIDE);
    for (HWND control : modeButtons) ShowWindow(control, page == InlinePage::powerMode ? SW_SHOW : SW_HIDE);
    for (HWND control : {ecoButton, perfAppButton}) ShowWindow(control, page == InlinePage::process ? SW_SHOW : SW_HIDE);
    for (HWND control : {restoreModesButton, energySettingsButton}) ShowWindow(control, page == InlinePage::powerMode ? SW_SHOW : SW_HIDE);
    SetWindowTextW(inlineBack, page == InlinePage::confirm ? tr(L"取消") : tr(L"返回排行"));
    layout();
    HWND focus = page == InlinePage::confirm ? inlineBack : inlineText;
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
    const int w = bounds.right, h = bounds.bottom, margin = sp(18), right = w - margin;
    const bool compact = h < px(600), minimal = h < px(480);
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
    if (inlinePage == InlinePage::process || inlinePage == InlinePage::powerMode || inlinePage == InlinePage::history ||
        inlinePage == InlinePage::lab || inlinePage == InlinePage::confirm || inlinePage == InlinePage::error) {
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
        if (inlinePage == InlinePage::history) {
            // Seven calendar days ending today; days without records show an empty slot.
            const RECT plot{margin + sp(18), sp(146), right - sp(18), sp(228)};
            drawText(dc, tr(L"每日放電 Wh"), {margin + sp(14), sp(124), right - sp(14), sp(142)}, Muted, smallFont);
            double peak = 1;
            int dates[7];
            for (int i = 0; i < 7; ++i) dates[i] = localDate(6 - i);
            auto find = [&](int date) -> const DayStats* {
                for (const auto& day : current.days) if (day.date == date) return &day;
                return nullptr;
            };
            for (int date : dates) if (auto day = find(date)) peak = std::max(peak, day->dischargedWh);
            const int gap = sp(6), slot = (int(plot.right - plot.left) - 6 * gap) / 7;
            for (int i = 0; i < 7; ++i) {
                const int x = plot.left + i * (slot + gap);
                const DayStats* day = find(dates[i]);
                const double wh = day ? day->dischargedWh : 0;
                const int height = wh > 0 ? std::max(sp(3), int((plot.bottom - plot.top - sp(16)) * wh / peak)) : 0;
                const RECT bar{x, plot.bottom - height, x + slot, plot.bottom};
                fill(dc, {x, plot.bottom, x + slot, plot.bottom + std::max(1, pixelStyle ? pixelUnit() / 2 : 1)}, Border);
                if (height) {
                    rounded(dc, bar, i == 6 ? Green : blend(Green, Surface, theme.highContrast ? 0 : .45), 3);
                    drawText(dc, number(wh, wh < 10 ? 1 : 0), {x - sp(6), bar.top - sp(16), x + slot + sp(6), bar.top - sp(1)}, Muted, smallFont,
                        DT_CENTER | DT_SINGLELINE | DT_BOTTOM);
                }
                drawText(dc, dayLabel(dates[i]), {x - sp(6), plot.bottom + sp(3), x + slot + sp(6), plot.bottom + sp(20)},
                    i == 6 ? Ink : Muted, smallFont, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            }
        }
        if (inlinePage != InlinePage::error && GetTickCount64() < noticeUntil) {
            const int noticeTop = inlinePage == InlinePage::process ? h - sp(134) : h - sp(84);
            drawText(dc, notice, {margin + sp(14), noticeTop, right - sp(14), noticeTop + sp(20)}, Green, smallFont);
        }
        return;
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
        // Plugged in without charging, battery health is more useful than a rate.
        const auto health = batteryHealth(p);
        const bool showHealth = plugged && !p.charging && health;
        std::wstring healthText = showHealth ? number(*health, 0) + L"%" : L"";
        if (showHealth && p.cycleCount) healthText += L" · " + std::to_wstring(*p.cycleCount) + tr(L" 次循環");
        if (desktop) { // no battery: show the CPU energy split instead
            drawText(dc, tr(L"CPU 核心"), {rateCard.left + sp(12), rateCard.top + sp(4), rateCard.right - sp(8), rateCard.top + sp(20)}, Muted, smallFont);
            drawText(dc, energyLive ? valueText(current.energy.coreWatts, L" W") : std::wstring(L"—"),
                {rateCard.left + sp(12), rateCard.top + sp(19), rateCard.right - sp(8), rateCard.bottom - sp(3)}, Ink, strongFont);
            drawText(dc, tr(L"未歸因給程式"), {timeCard.left + sp(12), timeCard.top + sp(4), timeCard.right - sp(8), timeCard.top + sp(20)}, Muted, smallFont);
            drawText(dc, energyLive ? valueText(current.energy.unassignedCpuWatts, L" W") : std::wstring(L"—"),
                {timeCard.left + sp(12), timeCard.top + sp(19), timeCard.right - sp(8), timeCard.bottom - sp(3)}, Ink, strongFont);
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
            {timeCard.left + sp(12), timeCard.top + sp(4), timeCard.right - sp(8), timeCard.top + sp(20)}, Muted, smallFont);
        drawText(dc, remaining, {timeCard.left + sp(12), timeCard.top + sp(19), timeCard.right - sp(8), timeCard.bottom - sp(3)}, Ink, strongFont);
        }
    }
    if (!compact) {
        RECT chart{margin, sp(236), right, sp(284)};
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
        if (GetTickCount64() < noticeUntil)
            drawText(dc, notice, {margin + sp(14), h - sp(84), right - sp(14), h - sp(64)}, Green, smallFont);
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
    std::wstring status = GetTickCount64() < noticeUntil || current.processes.empty() ? notice : selectionHint();
    drawText(dc, status, {margin, footer, right, footer + sp(19)}, GetTickCount64() < noticeUntil ? Green : Muted, smallFont);
    drawText(dc, tr(L"個別瓦數僅估計 CPU，非程式總耗電"),
        {margin, footer + sp(21), right, footer + sp(39)}, Muted, smallFont);
}
void showPowerDetails() {
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
    showInlinePage(InlinePage::power, tr(L"電池與充電詳情"), text);
    releaseInteraction();
}
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
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, MiniOpenId, tr(L"開啟面板"));
        AppendMenuW(menu, MF_STRING, MiniToggleId, tr(L"隱藏迷你視窗"));
        POINT point{}; GetCursorPos(&point); SetForegroundWindow(window);
        const int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, window, nullptr);
        DestroyMenu(menu);
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
    std::vector<uint8_t> curve(512); DWORD curveBytes = DWORD(curve.size());
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\BatteryScope", L"ChargeCurve", RRF_RT_REG_BINARY, nullptr, curve.data(), &curveBytes) == ERROR_SUCCESS) {
        curve.resize(curveBytes); chargeCurve.load(curve);
    }
    const DWORD limit = readSetting(L"ChargeLimit", 0);
    if (limit >= 40 && limit <= 96) chargeLimit = int(limit);
}
void showLedger() {
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
    showInlinePage(InlinePage::ledger, tr(L"本次累計耗能"), content);
    releaseInteraction();
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
    EnableWindow(restoreModesButton, editable && powerModes.restorable);
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
void showHistoryPage() {
    showInlinePage(InlinePage::history, tr(L"歷史紀錄 · 7 天"), historyText());
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
void showLabPage() {
    const std::wstring c = labPageText();
    // Keep the reader's place when the page refreshes itself.
    const LRESULT firstLine = inlinePage == InlinePage::lab ? SendMessageW(inlineText, EM_GETFIRSTVISIBLELINE, 0, 0) : 0;
    showInlinePage(InlinePage::lab, tr(L"省電實驗室"), c);
    if (firstLine > 0) SendMessageW(inlineText, EM_LINESCROLL, 0, firstLine);
    labPageShownAt = GetTickCount64();
}
void showPowerModePage() {
    refreshPowerModeControls();
    showInlinePage(InlinePage::powerMode, tr(L"電源模式"), powerModeText());
    releaseInteraction();
}
void showReviewCandidates() {
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
    showInlinePage(InlinePage::candidates, tr(L"耗電的背景程式 · 需自行判斷"), content);
    releaseInteraction();
}
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
    SetWindowTextW(perfAppButton, listed ? tr(L"移出效能清單") : tr(L"加入效能清單"));
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
    uint64_t nextProcess = 0, nextPower = 0;
    bool previousHidden = false, qosInitialized = false;
    // Power lab: undo a setting left changed by a crash, then sample every two seconds while it is on.
    if (labController) bs::lab::restoreInterrupted();
    std::unique_ptr<bs::lab::Probe> labProbe;
    uint64_t labLast = 0;
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
            if (background && !labActive) gpu.reset();
            else if (gpu.available()) {
                std::unordered_map<uint32_t, double> usage;
                if (gpu.sample(usage)) {
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
                if (labEngine) sample.workload = labEngine->take();
                labController->onSample(sample, labLast && now > labLast ? (now - labLast) / 1000.0 : 0);
                labLast = now;
            } else labLast = 0;
            changed = processSampled = true;
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
void openMenu(POINT point) {
    const bool preview = demo || isolatedPreview;
    HMENU menu = CreatePopupMenu();
    if (availableUpdate) {
        AppendMenuW(menu, MF_STRING | (updateBusy ? MF_GRAYED : 0), UpdateInstallId, (tr(L"安裝更新 ") + availableUpdate->version + L"…").c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(menu, MF_STRING, PowerModeId, tr(L"電源模式…"));
    AppendMenuW(menu, MF_STRING, PowerDetailsId, tr(L"電池與充電詳情"));
    AppendMenuW(menu, MF_STRING, LedgerId, tr(L"本次累計耗能"));
    AppendMenuW(menu, MF_STRING, HistoryId, tr(L"歷史紀錄（7 天）"));
    AppendMenuW(menu, MF_STRING, ReviewCandidatesId, tr(L"找出耗電的背景程式…"));
    AppendMenuW(menu, MF_STRING | (preview ? MF_GRAYED : 0), ExportCsvId, tr(L"匯出 CSV…"));
    AppendMenuW(menu, MF_STRING | (preview ? MF_GRAYED : 0), ShareCardId, tr(L"分享今日卡片…"));
    AppendMenuW(menu, MF_STRING | (miniEnabled ? MF_CHECKED : 0), MiniToggleId, tr(L"迷你浮動視窗"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    HMENU appearance = CreatePopupMenu();
    AppendMenuW(appearance, MF_STRING | (themeChoice == ui::ThemeChoice::system ? MF_CHECKED : 0), ThemeSystemId, tr(L"跟隨 Windows"));
    AppendMenuW(appearance, MF_STRING | (themeChoice == ui::ThemeChoice::dark ? MF_CHECKED : 0), ThemeDarkId, tr(L"深色"));
    AppendMenuW(appearance, MF_STRING | (themeChoice == ui::ThemeChoice::light ? MF_CHECKED : 0), ThemeLightId, tr(L"淺色"));
    AppendMenuW(appearance, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(appearance, MF_STRING | (pixelStyle ? MF_CHECKED : 0) | (ui::pixelFontLoaded() ? 0 : MF_GRAYED), StylePixelId, tr(L"像素風格"));
    AppendMenuW(appearance, MF_STRING | (!pixelStyle ? MF_CHECKED : 0), StyleSmoothId, tr(L"平滑風格"));
    AppendMenuW(appearance, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(appearance, MF_STRING | (showMascot ? MF_CHECKED : 0), MascotToggleId, tr(L"顯示電池小精靈"));
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(appearance), tr(L"外觀"));
    HMENU languages = CreatePopupMenu();
    AppendMenuW(languages, MF_STRING | (!english ? MF_CHECKED : 0), LanguageZhId, L"繁體中文");
    AppendMenuW(languages, MF_STRING | (english ? MF_CHECKED : 0), LanguageEnId, L"English");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(languages), tr(L"語言 / Language"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    HMENU alerts = CreatePopupMenu();
    const std::pair<int, unsigned> levels[] = {{LowBatteryOffId, 0}, {LowBattery10Id, 10}, {LowBattery20Id, 20}, {LowBattery30Id, 30}};
    for (const auto& level : levels)
        AppendMenuW(alerts, MF_STRING | (lowBatteryThreshold == level.second ? MF_CHECKED : 0) | (preview ? MF_GRAYED : 0), level.first,
            level.second ? (std::to_wstring(level.second) + L"%").c_str() : tr(L"關閉"));
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(alerts), tr(L"低電量提醒"));
    AppendMenuW(menu, MF_STRING | (drainAlerts ? MF_CHECKED : 0) | (preview ? MF_GRAYED : 0), DrainAlertId, tr(L"背景耗電提醒"));
    AppendMenuW(menu, MF_STRING | (sleepAlerts ? MF_CHECKED : 0) | (preview ? MF_GRAYED : 0), SleepAlertId, tr(L"睡眠耗電提醒"));
    HMENU reminders = CreatePopupMenu();
    const std::pair<int, unsigned> targets[] = {{ChargeReminderOffId, 0}, {ChargeReminder80Id, 80}, {ChargeReminder85Id, 85},
        {ChargeReminder90Id, 90}, {ChargeReminder100Id, 100}};
    for (const auto& target : targets)
        AppendMenuW(reminders, MF_STRING | (chargeReminderTarget == target.second ? MF_CHECKED : 0) | (preview ? MF_GRAYED : 0), target.first,
            target.second ? (std::to_wstring(target.second) + L"%").c_str() : tr(L"關閉"));
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(reminders), tr(L"充電提醒"));
    HMENU automatic = CreatePopupMenu();
    const std::pair<int, unsigned> savers[] = {{AutoSaverOffId, 0}, {AutoSaver20Id, 20}, {AutoSaver30Id, 30}};
    for (const auto& item : savers)
        AppendMenuW(automatic, MF_STRING | (autoSaverThreshold == item.second ? MF_CHECKED : 0) | (preview ? MF_GRAYED : 0), item.first,
            item.second ? (tr(L"低電量省電：") + std::to_wstring(item.second) + L"%").c_str() : tr(L"低電量省電：關閉"));
    AppendMenuW(automatic, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(automatic, MF_STRING, PerfAppsPageId, (tr(L"效能清單（") + std::to_wstring(performanceApps.size()) + tr(L" 個）…")).c_str());
    AppendMenuW(automatic, MF_STRING | (preview || performanceApps.empty() ? MF_GRAYED : 0), ClearPerfAppsId, tr(L"清空效能清單"));
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(automatic), tr(L"自動電源模式"));
    HMENU records = CreatePopupMenu();
    AppendMenuW(records, MF_STRING | (historyEnabled ? MF_CHECKED : 0) | (preview ? MF_GRAYED : 0), HistoryToggleId, tr(L"記錄每日歷史"));
    AppendMenuW(records, MF_STRING | (preview ? MF_GRAYED : 0), ClearHistoryId, tr(L"清除歷史紀錄…"));
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(records), tr(L"歷史紀錄設定"));
    AppendMenuW(menu, MF_STRING | (trayPercent ? MF_CHECKED : 0) | (preview ? MF_GRAYED : 0), TrayPercentId, tr(L"系統匣顯示電量數字"));
    AppendMenuW(menu, MF_STRING | (hotkeyEnabled ? MF_CHECKED : 0) | (preview ? MF_GRAYED : 0), HotkeyToggleId, tr(L"快速鍵 Ctrl+Alt+B"));
    AppendMenuW(menu, MF_STRING | (autoStart ? MF_CHECKED : 0) | (demo ? MF_GRAYED : 0), StartupId, tr(L"登入時啟動"));
    HMENU labMenu = CreatePopupMenu();
    const auto labStatus = labController ? labController->status() : bs::lab::Controller::Status{};
    AppendMenuW(labMenu, MF_STRING | (preview || (labStatus.running && !labStatus.plan) ? MF_GRAYED : 0), LabAutoId,
        labStatus.running && labStatus.plan ? tr(L"停止自動測試") : tr(L"一鍵自動測試（約 1 小時，結束自動匯出）"));
    AppendMenuW(labMenu, MF_STRING, LabPageId, tr(L"查看實驗室…"));
    AppendMenuW(labMenu, MF_STRING | (labStatus.recording ? MF_CHECKED : 0) | (preview ? MF_GRAYED : 0), LabRecordId, tr(L"記錄詳細資料"));
    AppendMenuW(labMenu, MF_SEPARATOR, 0, nullptr);
    const wchar_t* experimentNames[] = {tr(L"實驗：CPU Boost 開／關"), tr(L"實驗：處理器能源偏好"), tr(L"實驗：螢幕更新率"), tr(L"實驗：電源模式 平衡／效率")};
    AppendMenuW(labMenu, MF_SEPARATOR, 0, nullptr);
    for (int i = 0; i < int(bs::lab::Knob::count); ++i)
        AppendMenuW(labMenu, MF_STRING | (preview || labStatus.running ? MF_GRAYED : 0) |
            (labStatus.running && int(labStatus.knob) == i ? MF_CHECKED : 0), LabExperimentId + i, experimentNames[i]);
    AppendMenuW(labMenu, MF_STRING | (labStatus.running ? 0 : MF_GRAYED), LabStopId, tr(L"停止實驗並還原"));
    AppendMenuW(labMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(labMenu, MF_STRING | (preview ? MF_GRAYED : 0), LabExportId, tr(L"一鍵匯出（壓縮檔＋複製摘要）"));
    AppendMenuW(labMenu, MF_STRING | (preview ? MF_GRAYED : 0), LabFolderId, tr(L"開啟資料夾"));
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(labMenu), tr(L"省電實驗室"));
    HMENU updates = CreatePopupMenu();
    AppendMenuW(updates, MF_STRING | (updateChecks ? MF_CHECKED : 0) | (preview ? MF_GRAYED : 0), UpdateCheckId, tr(L"每天自動檢查"));
    AppendMenuW(updates, MF_STRING | (preview || updateBusy ? MF_GRAYED : 0), UpdateNowId, tr(L"立即檢查更新"));
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(updates), tr(L"更新"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, TrayShow, tr(L"顯示面板"));
    AppendMenuW(menu, MF_STRING | (pendingAction ? MF_GRAYED : 0), TrayExit, tr(L"結束 BatteryScope"));
    SetForegroundWindow(mainWindow);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, mainWindow, nullptr);
    DestroyMenu(menu); PostMessageW(mainWindow, WM_NULL, 0, 0);
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
    if (inlinePage == InlinePage::power) showPowerDetails();
    if (inlinePage == InlinePage::candidates) showReviewCandidates();
    if (inlinePage == InlinePage::ledger) showLedger();
    if (inlinePage == InlinePage::powerMode) showPowerModePage();
    if (inlinePage == InlinePage::history) showHistoryPage();
    if (inlinePage == InlinePage::perfApps) showPerformanceApps();
    if (inlinePage == InlinePage::lab) showLabPage();
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
        SendMessageW(inlineText, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(sp(4), sp(4)));
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
        return 0;
    }
    case WM_MEASUREITEM:
        if (wp == ListId) { reinterpret_cast<MEASUREITEMSTRUCT*>(lp)->itemHeight = px(RowHeight); return TRUE; }
        break;
    case WM_DRAWITEM: {
        auto item = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (item->CtlID == ListId) drawProcess(*item); else if (item->CtlType == ODT_BUTTON) drawButton(*item);
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
            performanceApps.clear(); writeNames(L"PerformanceApps", performanceApps);
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
                setNotice(enable ? tr(L"已開啟效率模式：降低優先順序並以省電方式執行。") : tr(L"已取消效率模式。"));
            } else setNotice(tr(L"無法變更效率模式：") + errorText(result));
            showDetails(row); refreshList();
            return 0;
        }
        if (id == PerfAppToggleId) {
            if (demo || isolatedPreview || !inlineProcess || inlineProcess->name.empty()) return 0;
            const ProcessRow row = *inlineProcess;
            const std::wstring name = lowerName(row.name);
            const bool add = !performanceApps.count(name);
            if (add) performanceApps.insert(name); else performanceApps.erase(name);
            if (!writeNames(L"PerformanceApps", performanceApps)) setNotice(tr(L"無法儲存設定。"));
            else setNotice(add ? tr(L"已加入效能清單：執行時會自動切到效能模式。") : tr(L"已移出效能清單。"));
            applyAutoPowerRules();
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
        if (id == PowerModeId) { showPowerModePage(); return 0; }
        if (isModeButton(UINT(id))) {
            if (demo || isolatedPreview) return 0;
            const bool pluggedIn = id < ModeDcEfficiencyId;
            const auto mode = PowerMode((id - ModeAcEfficiencyId) % 3);
            const DWORD result = setPowerMode(pluggedIn, mode);
            if (result) setNotice(tr(L"無法變更電源模式：") + errorText(result));
            else setNotice((pluggedIn ? tr(L"已將「插電時」設為") : tr(L"已將「用電池時」設為")) + std::wstring(modeName(mode)) + tr(L"。"));
            refreshPowerModeControls();
            return 0;
        }
        if (id == RestoreModesId) {
            if (demo || isolatedPreview) return 0;
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
        if (id == LabRecordId && labController) {
            const bool on = !labController->status().recording;
            labController->setRecording(on);
            if (!writeSetting(L"LabRecording", on ? 1 : 0)) setNotice(tr(L"無法儲存設定。"));
            wake.notify_one(); // switch to the two-second cadence now
            showLabPage();
            return 0;
        }
        if (id >= LabExperimentId && id < LabExperimentId + int(bs::lab::Knob::count) && labController) {
            const DWORD result = labController->start(bs::lab::Knob(id - LabExperimentId));
            setNotice(result ? tr(L"無法開始實驗：") + errorText(result) : std::wstring(tr(L"實驗開始：用電池時每 2 分鐘輪流切換設定。")));
            wake.notify_one();
            showLabPage();
            return 0;
        }
        if (id == LabStopId && labController) { labController->stop(); setNotice(tr(L"實驗已停止，設定已還原。")); showLabPage(); return 0; }
        if (id == LabExportId) { exportLab(); return 0; }
        if ((id == LabAutoId || id == LabAutoStartId) && labController && labEngine) {
            if (autoTestActive && id == LabAutoStartId) return 0; // already running
            if (autoTestActive) { // second click: stop
                labController->stop(); labEngine->stop(); autoTestActive = false;
                setNotice(tr(L"自動測試已停止，設定已還原。")); exportLab(); showLabPage();
                return 0;
            }
            const DWORD result = labController->startPlan(bs::lab::automaticPlan(!bs::lab::WorkloadEngine::browserPath().empty()));
            if (result) { setNotice(tr(L"無法開始自動測試：") + errorText(result)); showLabPage(); return 0; }
            autoTestActive = true;
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
                labEngine->stop(); autoTestActive = false;
                exportLab();
                showMain(); showLabPage();
                showBalloon(aborted ? tr(L"自動測試已中止") : tr(L"自動測試完成"), tr(L"設定已還原，結果與記錄已匯出到桌面，摘要已複製。"), BalloonTarget::none);
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
    case WM_POWERBROADCAST:
        if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND || wp == PBT_APMPOWERSTATUSCHANGE) { resetSamples = true; wake.notify_one(); }
        return TRUE;
    case PowerModeMessage:
        effectiveMode = EffectiveMode(wp);
        refreshPowerButton();
        if (inlinePage == InlinePage::powerMode) refreshPowerModeControls();
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        const POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (PtInRect(&mascotRect, point)) { setNotice(mascotLine()); ++sampleTick; InvalidateRect(window, nullptr, FALSE); return 0; }
        break;
    }
    case WM_HOTKEY:
        if (wp == PanelHotkey) toggleMain();
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
        KillTimer(window, NoticeTimer); KillTimer(window, UpdateTimer);
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
        modeButtons[0], modeButtons[1], modeButtons[2], modeButtons[3], modeButtons[4], modeButtons[5]}) {
        if (!(GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE)) continue;
        RECT childRect; GetWindowRect(child, &childRect); MapWindowPoints(nullptr, mainWindow, reinterpret_cast<POINT*>(&childRect), 2);
        const int cw = childRect.right - childRect.left, ch = childRect.bottom - childRect.top;
        SaveDC(dc); SetViewportOrgEx(dc, childRect.left, childRect.top, nullptr);
        IntersectClipRect(dc, 0, 0, cw, ch);
        if (child == viewport) {
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
    check("power_details_reuse_panel", inlinePage == InlinePage::power && IsWindowVisible(inlineText));
    auto pageText = [] {
        std::wstring text(size_t(GetWindowTextLengthW(inlineText)) + 1, L'\0');
        GetWindowTextW(inlineText, text.data(), int(text.size()));
        return text;
    };
    check("power_details_show_last_sleep_and_health_trend", current.lastSleep && pageText().find(sleepText(*current.lastSleep)) != std::wstring::npos &&
        pageText().find(healthTrendText()) != std::wstring::npos && !healthTrendText().empty());
    SendMessageW(mainWindow, WM_COMMAND, InlineBackId, 0);
    showReviewCandidates();
    check("candidate_review_reuses_panel", inlinePage == InlinePage::candidates && IsWindowVisible(inlineText));
    SendMessageW(mainWindow, WM_COMMAND, InlineBackId, 0);
    showLedger();
    std::wstring ledgerText(size_t(GetWindowTextLengthW(inlineText)) + 1, L'\0');
    GetWindowTextW(inlineText, ledgerText.data(), int(ledgerText.size()));
    check("session_energy_reuses_panel", inlinePage == InlinePage::ledger && IsWindowVisible(inlineText) &&
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
        if (existing) PostMessageW(existing, WM_COMMAND, option(L"--autotest") ? LabAutoStartId : TrayShow, 0);
        if (existing && !option(L"--autotest")) SetForegroundWindow(existing);
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
            shotOK = trayTest ? panelSelfTest(args[1]) : captureWindow(args[1]); DestroyWindow(window); continue;
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
