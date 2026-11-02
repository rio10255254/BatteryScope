#include "platform.h"
#include "energy.h"
#include "motion.h"
#include "i18n.h"
#include "version.h"
#include "ui.h"
#include <commctrl.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <sstream>
#include <thread>

namespace {
using namespace bs;
constexpr UINT SampleMessage = WM_APP + 1, TrayMessage = WM_APP + 2, ActionMessage = WM_APP + 3, RefreshMessage = WM_APP + 4;
constexpr int ListId = 100, CloseId = 101, KillId = 102, ServiceId = 103, DetailsId = 104,
    BackgroundId = 105, AllId = 106, StartupId = 107, HideId = 108, MenuId = 109,
    LanguageZhId = 110, LanguageEnId = 111, PowerDetailsId = 112, SaverModeId = 113,
    EnergySaverSettingsId = 114, ReviewCandidatesId = 115, InlineBackId = 116, InlineConfirmId = 117,
    LedgerId = 118, ExportCsvId = 119, LowBatteryOffId = 120, LowBattery10Id = 121, LowBattery20Id = 122, LowBattery30Id = 123,
    HotkeyToggleId = 124, ThemeSystemId = 125, ThemeDarkId = 126, ThemeLightId = 127, StylePixelId = 128, StyleSmoothId = 129,
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
    detailsButton = nullptr, backgroundCheck = nullptr, allCheck = nullptr, hideButton = nullptr, menuButton = nullptr, saverButton = nullptr,
    inlineText = nullptr, inlineBack = nullptr, inlineConfirm = nullptr;
HFONT normalFont = nullptr, smallFont = nullptr, strongFont = nullptr, titleFont = nullptr, valueFont = nullptr, metricFont = nullptr;
HFONT systemFont = nullptr;              // dynamic text: details pages and tooltips
std::map<HFONT, HFONT> fallbackFonts;    // pixel font -> system font for uncovered text
std::vector<HFONT> fallbackOwned;
HICON appIcon = nullptr, smallIcon = nullptr, headerIcon = nullptr;
bool updateTray(bool add);
UINT dpi = 96, taskbarCreated = 0;
std::atomic<bool> quit{false}, hidden{false}, resetSamples{false};
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
bool saverActive = false;
bool isolatedPreview = false;
enum class InlinePage { none, process, power, candidates, ledger, confirm, error };
unsigned lowBatteryThreshold = 0; // percent; 0 = off
LowBatteryMonitor lowBattery;
bool hotkeyEnabled = false, hotkeyRegistered = false;
InlinePage inlinePage = InlinePage::none;
bool inlineClosing = false;
std::wstring inlineTitle;
std::optional<ProcessRow> inlineProcess;
std::optional<ProcessRow> pendingForceRow;
std::wstring notice = tr(L"正在讀取電池與程序資料…");
uint64_t noticeUntil = 0;
std::deque<uint64_t> actionMarks;
constexpr UINT_PTR MotionTimer = 92, NoticeTimer = 93;
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
void refreshSaverButton();
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
        hideButton, menuButton, saverButton, inlineBack, inlineConfirm})
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
std::wstring trayText() {
    std::wstring text = L"BatteryScope";
    if (current.history.empty()) return text;
    const auto& p = current.history.back();
    text += L" · " + valueText(p.percent, L"%", 0);
    if (p.charging) {
        const auto charge = calculateCharging(current.history);
        text += tr(L" · 充電 ") + valueText(charge.watts, L" W");
        if (auto full = durationText(charge.hoursToFull); !full.empty()) text += english ? L" · " + full + L" to full" : L" · " + full + L"充滿";
    } else if (p.onAc) text += tr(L" · 插電中");
    else {
        text += L" · " + valueText(current.power.currentWatts, L" W");
        if (auto left = durationText(current.power.hoursRemaining); !left.empty()) text += english ? L" · " + left + L" left" : L" · 續航" + left;
    }
    return text;
}
bool updateTray(bool add) {
    if (isolatedPreview) return false;
    NOTIFYICONDATAW data{}; data.cbSize = sizeof(data); data.hWnd = mainWindow; data.uID = 1;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP; data.uCallbackMessage = TrayMessage; data.hIcon = smallIcon;
    wcsncpy_s(data.szTip, trayText().c_str(), _TRUNCATE);
    if (!Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &data)) return false;
    if (add) { data.uVersion = NOTIFYICON_VERSION_4; trayVersion4 = Shell_NotifyIconW(NIM_SETVERSION, &data) != FALSE; }
    return true;
}
// One Windows notification when the battery reaches the chosen threshold.
void showLowBatteryNotice() {
    if (!trayAdded || current.history.empty()) return;
    const auto& p = current.history.back();
    std::wstring body = tr(L"目前耗電 ") + valueText(current.power.currentWatts, L" W");
    if (auto left = durationText(current.power.hoursRemaining); !left.empty()) body += english ? L" · " + left + L" left" : L" · 續航" + left;
    for (const auto& row : current.processes) {
        if (!row.averageCpuWatts || *row.averageCpuWatts < .3 || row.pid <= 4) continue;
        body += tr(L"\n耗電最高：") + (row.services.size() == 1 ? row.services.front().display : row.name) +
            L" (" + valueText(row.averageCpuWatts, L" W", 1) + L")";
        break;
    }
    NOTIFYICONDATAW data{}; data.cbSize = sizeof(data); data.hWnd = mainWindow; data.uID = 1;
    data.uFlags = NIF_INFO; data.dwInfoFlags = NIIF_WARNING | NIIF_RESPECT_QUIET_TIME;
    wcsncpy_s(data.szInfoTitle, (tr(L"電量偏低 · ") + valueText(p.percent, L"%", 0)).c_str(), _TRUNCATE);
    wcsncpy_s(data.szInfo, body.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &data);
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
    refreshSaverButton();
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
void refreshSaverButton() {
    if (!saverButton) return;
    if (demo || isolatedPreview) {
        saverActive = false;
        SetWindowTextW(saverButton, tr(L"省電"));
        EnableWindow(saverButton, FALSE);
        return;
    }
    const auto state = querySaverMode();
    saverActive = !state.error && state.efficient;
    SetWindowTextW(saverButton, state.restorable ? tr(L"還原") : state.efficient ? tr(L"已省電") : tr(L"省電"));
    EnableWindow(saverButton, !demo && !isolatedPreview && !state.error && (!state.efficient || state.restorable));
    InvalidateRect(saverButton, nullptr, FALSE);
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
    MoveWindow(saverButton, w - sp(88), sp(47), sp(68), sp(22), TRUE);
    MoveWindow(menuButton, w - sp(92), sp(16), sp(32), sp(30), TRUE);
    MoveWindow(hideButton, w - sp(52), sp(16), sp(32), sp(30), TRUE);
    const bool fullPage = inlinePage == InlinePage::process || inlinePage == InlinePage::confirm || inlinePage == InlinePage::error;
    const int textTop = fullPage ? sp(128) : top - sp(37);
    MoveWindow(inlineText, margin + sp(12), textTop, inner - sp(24), std::max(sp(55), h - sp(94) - textTop), TRUE);
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
        (page == InlinePage::candidates && lines.size() > 500));
    SendMessageW(inlineText, EM_SETSEL, 0, 0);
    SendMessageW(inlineText, EM_SCROLLCARET, 0, 0);
    for (auto control : {viewport, closeButton, killButton, serviceButton, detailsButton, backgroundCheck, allCheck}) ShowWindow(control, SW_HIDE);
    ShowWindow(inlineText, SW_SHOW); ShowWindow(inlineBack, SW_SHOW);
    ShowWindow(inlineConfirm, page == InlinePage::confirm ? SW_SHOW : SW_HIDE);
    SetWindowTextW(inlineBack, page == InlinePage::confirm ? tr(L"取消") : tr(L"返回排行"));
    layout(); SetFocus(page == InlinePage::confirm ? inlineBack : inlineText);
}
void finishInlineClose() {
    inlinePage = InlinePage::none; inlineTitle.clear(); inlineProcess.reset(); pendingForceRow.reset();
    inlineClosing = false;
    ShowWindow(inlineText, SW_HIDE); ShowWindow(inlineBack, SW_HIDE); ShowWindow(inlineConfirm, SW_HIDE);
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
    const bool inlineButton = id == InlineBackId || id == InlineConfirmId;
    const COLORREF behind = inlineButton ? Surface : Background;
    fill(dc, r, behind);
    const double hover = disabled ? 0 : buttonMotions[item.hwndItem].value(motionNow());
    const double press = pressMotions[item.hwndItem].value(motionNow());
    const int inset = int(std::lround(px(1) * press));
    InflateRect(&r, -inset, -inset);
    wchar_t title[128]{}; GetWindowTextW(item.hwndItem, title, int(std::size(title)));
    const float radius = id == SaverModeId ? (r.bottom - r.top) / 2.0f : float(sp(9));
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
        const bool checked = (id == BackgroundId && backgroundOnly) || (id == AllId && showAll) || (id == SaverModeId && saverActive);
        if (disabled) {
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
        } else if (id == BackgroundId || id == AllId || id == SaverModeId) {
            if (checked) {
                face = id == SaverModeId ? Green : theme.accentSoft;
                edgeColor = id == SaverModeId ? CLR_INVALID : blend(Green, behind, .35);
                text = id == SaverModeId ? theme.accentInk : Green;
                if (id == SaverModeId) face = blend(Green, Ink, hover * .14);
            } else {
                face = blend(id == SaverModeId ? theme.accentSoft : Surface, theme.raised, hover);
                edgeColor = id == SaverModeId ? CLR_INVALID : Border;
                text = id == SaverModeId ? Green : Muted;
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
    const std::wstring kind = tr(row.kind.c_str());
    RECT chip{left, y + sp(31), left + textWidth(dc, kind, smallFont) + sp(12), y + sp(45)};
    rounded(dc, chip, inverted ? tileColor : blend(tileColor, Ink, theme.dark ? .08 : .06), 5);
    if (theme.highContrast) outline(dc, chip, sub, 5);
    drawText(dc, kind, chip, sub, smallFont, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    drawText(dc, L"PID " + std::to_wstring(row.pid), {chip.right + sp(6), y + sp(29), right - sp(100), y + sp(47)}, sub, smallFont);
    std::wstring range = row.cpuWattsLow && row.cpuWattsHigh ?
        number(*row.cpuWattsLow, 2) + L"–" + number(*row.cpuWattsHigh, 2) + L" W" :
        L"CPU " + (row.measured ? number(row.cpu) + L"%" : L"—");
    drawText(dc, range, {right - sp(102), y + sp(29), right, y + sp(47)}, sub, smallFont, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
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
    drawText(dc, demo ? tr(L"示範資料 · 關閉操作已停用") : tr(L"電池與背景活動"), {margin + sp(30), sp(39), w - sp(96), sp(60)}, Muted, smallFont);
    if (inlinePage == InlinePage::process || inlinePage == InlinePage::confirm || inlinePage == InlinePage::error) {
        card(dc, {margin, sp(74), right, h - sp(14)}, Surface, 14);
        drawText(dc, inlineTitle, {margin + sp(14), sp(84), right - sp(14), sp(114)}, Ink, titleFont);
        fill(dc, {margin + sp(14), sp(119), right - sp(14), sp(119) + 1}, Border);
        if (inlinePage != InlinePage::error && GetTickCount64() < noticeUntil)
            drawText(dc, notice, {margin + sp(14), h - sp(84), right - sp(14), h - sp(64)}, Green, smallFont);
        return;
    }
    PowerSample p; if (!current.history.empty()) p = current.history.back();
    const auto& m = current.power;
    const auto charge = calculateCharging(current.history);
    const bool plugged = p.acKnown && (p.onAc || p.charging);
    std::wstring currentW = current.history.empty() ? tr(L"讀取中") : !p.present ? tr(L"無電池") : p.charging ? valueText(charge.watts, L" W") : plugged ? tr(L"已接電") : valueText(m.currentWatts, L" W");
    std::wstring rateLabel = !p.present ? tr(L"仍可監測程序 CPU") : p.charging ? tr(L"電池端 · 非充電器輸出") : plugged ? tr(L"接頭類型未提供") :
        m.estimated ? tr(L"依電量變化估算") : tr(L"電池回報的整機放電");
    if (minimal && m.percentPerHour) rateLabel = number(*m.percentPerHour) + tr(L" 百分點 / 小時");
    // Hero card: power on the left, battery level on the right.
    card(dc, {margin, sp(72), right, sp(180)}, Surface, 16);
    drawText(dc, p.charging ? tr(L"充電功率") : plugged ? tr(L"外部電源") : tr(L"目前耗電"), {margin + sp(16), sp(84), right - sp(122), sp(103)}, Muted, smallFont);
    const int digitUnit = std::max(3, px(4));
    if (!pixelStyle || !ui::drawPixelDigits(dc, currentW, margin + sp(16), (sp(102) + sp(150)) / 2 - digitUnit * 7 / 2, digitUnit, Ink))
        drawText(dc, currentW, {margin + sp(14), sp(102), right - sp(119), sp(150)}, Ink, valueFont);
    drawText(dc, rateLabel, {margin + sp(16), sp(152), right - sp(150), sp(172)}, Muted, smallFont);
    const bool low = p.percent && !plugged && *p.percent <= 20;
    const COLORREF level = !p.percent ? Muted : p.charging ? Green : low && *p.percent <= 10 ? Danger : low ? theme.warn : Green;
    const std::wstring percentText = valueText(p.percent, L"%", 0);
    const int percentUnit = pixelUnit(), percentWidth = ui::pixelDigitsWidth(percentText, percentUnit);
    if (!pixelStyle || !ui::drawPixelDigits(dc, percentText, right - sp(16) - percentWidth, (sp(84) + sp(116)) / 2 - percentUnit * 7 / 2,
        percentUnit, p.percent ? Ink : Muted))
        drawText(dc, percentText, {right - sp(112), sp(84), right - sp(16), sp(116)},
            p.percent ? Ink : Muted, titleFont, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    if (pixelStyle) ui::pixelBattery(dc, {right - sp(16) - sp(64), sp(120), right - sp(16), sp(144)}, p.percent ? *p.percent / 100 : 0,
        blend(Muted, Surface, .25), level, blend(Surface, Border, .7), p.charging, Ink, Surface, pixelUnit());
    else ui::drawBattery(dc, {right - sp(16) - sp(60), sp(121), right - sp(16), sp(143)}, p.percent ? *p.percent / 100 : 0,
        blend(Muted, Surface, .25), level, p.charging, Ink, Surface);
    drawText(dc, p.charging ? tr(L"充電中") : plugged ? (p.percent && *p.percent >= 99.5 ? tr(L"已充滿") : tr(L"未充電")) : tr(L"剩餘電量"),
        {right - sp(146), sp(151), right - sp(16), sp(172)}, Muted, smallFont, DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    if (!minimal) {
        const int gap = sp(8), half = (right - margin - gap) / 2;
        const RECT rateCard{margin, sp(187), margin + half, sp(228)}, timeCard{margin + half + gap, sp(187), right, sp(228)};
        card(dc, rateCard, Surface, 12); card(dc, timeCard, Surface, 12);
        drawText(dc, p.charging ? tr(L"充電速度") : plugged ? tr(L"電池狀態") : tr(L"耗電速度"),
            {rateCard.left + sp(12), rateCard.top + sp(4), rateCard.right - sp(8), rateCard.top + sp(20)}, Muted, smallFont);
        drawText(dc, plugged && !p.charging ? (p.percent && *p.percent >= 99.5 ? tr(L"已充滿") : tr(L"已接電，暫未充電")) : valueText(p.charging ? charge.percentPerHour : m.percentPerHour, tr(L" 百分點 / 小時")),
            {rateCard.left + sp(12), rateCard.top + sp(19), rateCard.right - sp(8), rateCard.bottom - sp(3)}, Ink, strongFont);
        std::wstring remaining = tr(L"估計續航 —");
        if (plugged) remaining = p.charging ? tr(L"等待穩定充電資料") : L"—";
        if (auto text = durationText(p.charging ? charge.hoursToFull : m.hoursRemaining); !text.empty()) remaining = text;
        drawText(dc, plugged ? tr(L"至充滿 · 依目前速度") : tr(L"近 5 分鐘 ") + valueText(m.averageWatts, L" W"),
            {timeCard.left + sp(12), timeCard.top + sp(4), timeCard.right - sp(8), timeCard.top + sp(20)}, Muted, smallFont);
        drawText(dc, remaining, {timeCard.left + sp(12), timeCard.top + sp(19), timeCard.right - sp(8), timeCard.bottom - sp(3)}, Ink, strongFont);
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
        drawText(dc, chargingChart ? tr(L"30 分鐘 · 充電 W") : tr(L"30 分鐘 · 放電 W"), {chart.left + sp(12), chart.top + sp(5), chart.right - sp(90), chart.top + sp(21)}, Muted, smallFont);
        RECT plot{chart.left + sp(12), chart.top + sp(24), chart.right - sp(12), chart.bottom - sp(6)};
        double maximum = 10;
        uint64_t last = current.history.empty() ? GetTickCount64() : current.history.back().ms;
        for (const auto& point : current.history) if (auto watts = chartPower(point)) maximum = std::max(maximum, std::ceil(*watts / 5) * 5);
        const int middle = (plot.top + plot.bottom) / 2;
        if (pixelStyle) ui::pixelDottedLine(dc, {plot.left, middle}, {plot.right, middle}, blend(Border, Surface, .1), pixelUnit());
        else ui::drawDashedLine(dc, {plot.left, middle}, {plot.right, middle}, blend(Border, Surface, .2), 1.0f);
        fill(dc, {plot.left, plot.bottom, plot.right, plot.bottom + (pixelStyle ? pixelUnit() / 2 : 1)}, Border);
        std::vector<std::vector<POINT>> segments;
        uint64_t previous = 0; bool connected = false;
        for (const auto& point : current.history) {
            auto watts = chartPower(point);
            if (!watts) { connected = false; continue; }
            POINT at{plot.right - int((last - point.ms) / 1800000.0 * (plot.right - plot.left)),
                plot.bottom - int(std::min(*watts, maximum) / maximum * (plot.bottom - plot.top))};
            if (!connected || point.ms - previous > 30000) segments.emplace_back();
            segments.back().push_back(at);
            connected = true; previous = point.ms;
        }
        bool any = false;
        if (pixelStyle) {
            // Pixel bar chart: two-unit columns with a one-unit gap, each the mean of its time slice.
            const int u = pixelUnit(), pitch = u * 3, columns = std::max(1, int(plot.right - plot.left) / pitch);
            const int start = plot.right - columns * pitch;
            std::vector<double> sum(size_t(columns), 0), count(size_t(columns), 0);
            for (const auto& point : current.history) {
                auto watts = chartPower(point);
                if (!watts || point.ms > last || last - point.ms > 1800000) continue;
                const int column = std::clamp(columns - 1 - int((last - point.ms) / 1800000.0 * columns), 0, columns - 1);
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
        for (auto time : actionMarks) if (!chargingChart && time <= last && last - time <= 1800000) {
            int x = plot.right - int((last - time) / 1800000.0 * (plot.right - plot.left));
            if (pixelStyle) ui::pixelDottedLine(dc, {x, plot.top}, {x, plot.bottom}, theme.warn, pixelUnit());
            else ui::drawDashedLine(dc, {x, plot.top}, {x, plot.bottom}, theme.warn, 1.0f);
        }
        if (!any) drawText(dc, chargingChart ? tr(L"等待充電資料") : plugged ? tr(L"插電期間暫停放電曲線") : tr(L"等待放電資料"), plot, Muted, smallFont, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
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
    drawText(dc, energyReady ? tr(L"30 秒平滑 · ") + qualityText(current.energy.quality) + L" " + number(current.energy.observedCpuCoverage * 100, 0) + L"%" : tr(L"能源暫不可用，依 CPU / I/O 活動排序"),
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
    text += tr(L"\n至充滿（依目前速度）：") + valueText(charge.hoursToFull, tr(L" 小時"), 2);
    text += tr(L"\n\n充電功率是電池吸收的淨功率，不是插座功率、充電器額定功率或 USB PD 合約瓦數。");
    text += tr(L"\n充滿時間需至少 60 秒連續穩定充電資料。接近滿電時會減速；電池保護上限也可能讓充電提前停止，因此不是保證時間。");
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
void loadSettings() {
    english = readSetting(L"Language", 0) == 1;
    const DWORD storedTheme = readSetting(L"Theme", 0);
    themeChoice = storedTheme <= 2 ? ui::ThemeChoice(storedTheme) : ui::ThemeChoice::system;
    const DWORD threshold = readSetting(L"LowBattery", 0);
    lowBatteryThreshold = threshold == 10 || threshold == 20 || threshold == 30 ? threshold : 0;
    hotkeyEnabled = readSetting(L"Hotkey", 0) == 1;
    pixelStyle = readSetting(L"Style", 1) != 0;
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
        L"session_cpu_mwh,memory_mb,io_mb_per_s,services,path\r\n";
    for (size_t i = 0; i < current.processes.size(); ++i) {
        const auto& row = current.processes[i];
        std::wstring services;
        for (const auto& service : row.services) services += (services.empty() ? L"" : L"; ") + service.name;
        processes += std::to_wstring(i + 1) + L"," + csvField(row.name) + L"," + std::to_wstring(row.pid) + L"," + kindToken(row.kind) + L"," +
            (row.measured ? number(row.cpu, 2) : L"") + L"," + (row.measured ? number(row.averageCpu, 2) : L"") + L"," +
            csvNumber(row.cpuWatts, 3) + L"," + csvNumber(row.averageCpuWatts, 3) + L"," + csvNumber(row.cpuWattsLow, 3) + L"," +
            csvNumber(row.cpuWattsHigh, 3) + L"," + csvNumber(row.cpuMilliWh, 2) + L"," +
            (row.memoryKnown ? number(row.memoryMB, 1) : L"") + L"," + (row.ioKnown && row.measured ? number(row.ioMB, 3) : L"") + L"," +
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
void showReviewCandidates() {
    const auto candidates = reviewCandidates(current);
    std::wstring content = tr(L"依持續 CPU 活動、可讀身分與前景／服務關聯，列出值得檢查的背景程序。這不是安全關閉判定，也不是關閉後可省電量。\n");
    if (candidates.empty()) content += tr(L"\n目前沒有符合條件的候選，或觀測資料尚不足。\n");
    for (size_t index : candidates) {
        const auto& row = current.processes[index];
        content += L"\n• " + row.name + L" · PID " + std::to_wstring(row.pid) + L" · " +
            valueText(row.averageCpuWatts, L" CPU W", 2) + L" · " + std::to_wstring(int(row.busySeconds)) + tr(L" 秒持續活動");
    }
    content += tr(L"\n\n可能有同步、下載、音訊、未儲存資料或其他程序依賴。請先查看詳情；程式不會自動休眠或關閉候選。" );
    showInlinePage(InlinePage::candidates, tr(L"節能候選 · 需人工判斷"), content);
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
void showDetails(const ProcessRow& row) {
    if (!inlineProcess || &row != &*inlineProcess) inlineProcess = row;
    std::wstring content = row.name + L"\nPID: " + std::to_wstring(row.pid) +
        tr(L"\n路徑：") + (row.path.empty() ? tr(L"未知") : row.path) +
        tr(L"\n\nCPU（全機比例）: ") + number(row.cpu) + tr(L"%\n30 秒平滑 CPU: ") + number(row.averageCpu) +
        L"%\nI/O: " + (row.ioKnown ? number(row.ioMB, 2) + L" MB/s" : tr(L"未知")) +
        tr(L"\n記憶體: ") + (row.memoryKnown ? number(row.memoryMB) + tr(L" MB（") + number(row.memoryAgeSeconds, 0) + tr(L" 秒前）") : tr(L"未知")) +
        tr(L"\n活動: ") + activityText(row);
    if (row.cpuWatts) {
        content += tr(L"\n\n本次 CPU 估計: ") + valueText(row.cpuWatts, L" W", 2) +
            tr(L"\n30 秒平滑 CPU 估計: ") + valueText(row.averageCpuWatts, L" W", 2) +
            tr(L"\n30 秒平滑的核心分攤模型範圍: ") + valueText(row.cpuWattsLow, L"", 2) + L"–" + valueText(row.cpuWattsHigh, L" W", 2) +
            tr(L"\n監測期間 CPU 估計能量: ") + valueText(row.cpuMilliWh, L" mWh", 2);
        content += tr(L"\n本次固定核心資訊覆蓋: ") + number(row.placementCoverage*100,0) + L"%";
        content += tr(L"\n全機 CPU 時間覆蓋: ") + number(current.energy.observedCpuCoverage*100,0) +
            tr(L"%（可讀程序時間／系統忙碌時間，非準確率）");
        content += tr(L"\n觀測品質: ") + qualityText(current.energy.quality) +
            tr(L" · 採樣端點差 ") + number(current.energy.sampleSkewMs, 0) + L" ms";
        if (!current.history.empty() && current.history.back().fullWh && row.averageCpuWatts)
            content += tr(L"\nCPU 部分估計耗電速度: ") + number(*row.averageCpuWatts / *current.history.back().fullWh * 100, 2) + tr(L" 百分點／小時");
        content += tr(L"\n\n用 EMI 量到的核心能源約束分攤。持續固定在同一核心的執行緒優先按該核心分配；其餘依 CPU 時間估計。範圍反映未知的核心排程位置，並非統計信賴區間；不同指令與 SMT 仍可能產生誤差。");
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
    showInlinePage(InlinePage::process, tr(L"程序詳情"), content);
}
void showForceConfirmation(const ProcessRow& row) {
    if (!pendingForceRow || &row != &*pendingForceRow) pendingForceRow = row;
    std::wstring text = tr(L"強制結束 ") + row.name + tr(L"（PID ") + std::to_wstring(row.pid) +
        tr(L"）？\n\n未儲存資料可能遺失。此操作僅結束所選程序，不包含其他子程序。");
    showInlinePage(InlinePage::confirm, tr(L"確認強制結束"), text);
}

void sampleLoop() {
    Collector collector;
    EnergyReader energyReader;
    CpuAttributor attributor;
    EnergyLedger ledger;
    SamplingPolicy policy;
    Snapshot state;
    uint64_t nextProcess = 0, nextPower = 0;
    bool previousHidden = false, qosInitialized = false;
    while (!quit) {
        auto now = GetTickCount64(); bool background = hidden.load(), processSampled = false;
        if (resetSamples.exchange(false)) { collector.reset(); energyReader.reset(); policy.reset(); nextProcess = nextPower = 0; }
        if (!qosInitialized || previousHidden != background) {
            state.samplerEcoQoS = setSamplerEcoQoS(background) && background;
            qosInitialized = true; policy.reset(); nextProcess = nextPower = 0; previousHidden = background;
        }
        bool changed = false;
        if (now >= nextPower) {
            appendPower(state.history, readPower()); state.power = calculatePower(state.history);
            nextPower = now + (background ? 10000 : 6000); changed = true;
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
            nextProcess = now + state.samplingSeconds * 1000;
            if (background) nextPower = nextProcess;
            changed = processSampled = true;
        }
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
void openMenu(POINT point) {
    const bool preview = demo || isolatedPreview;
    HMENU menu = CreatePopupMenu();
    HMENU languages = CreatePopupMenu();
    AppendMenuW(languages, MF_STRING | (!english ? MF_CHECKED : 0), LanguageZhId, L"繁體中文");
    AppendMenuW(languages, MF_STRING | (english ? MF_CHECKED : 0), LanguageEnId, L"English");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(languages), tr(L"語言 / Language"));
    HMENU themes = CreatePopupMenu();
    AppendMenuW(themes, MF_STRING | (themeChoice == ui::ThemeChoice::system ? MF_CHECKED : 0), ThemeSystemId, tr(L"跟隨 Windows"));
    AppendMenuW(themes, MF_STRING | (themeChoice == ui::ThemeChoice::dark ? MF_CHECKED : 0), ThemeDarkId, tr(L"深色"));
    AppendMenuW(themes, MF_STRING | (themeChoice == ui::ThemeChoice::light ? MF_CHECKED : 0), ThemeLightId, tr(L"淺色"));
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(themes), tr(L"主題"));
    HMENU styles = CreatePopupMenu();
    AppendMenuW(styles, MF_STRING | (pixelStyle ? MF_CHECKED : 0) | (ui::pixelFontLoaded() ? 0 : MF_GRAYED), StylePixelId, tr(L"像素"));
    AppendMenuW(styles, MF_STRING | (!pixelStyle ? MF_CHECKED : 0), StyleSmoothId, tr(L"平滑"));
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(styles), tr(L"風格"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, PowerDetailsId, tr(L"電池與充電詳情"));
    AppendMenuW(menu, MF_STRING, LedgerId, tr(L"本次累計耗能"));
    AppendMenuW(menu, MF_STRING, ReviewCandidatesId, tr(L"查看節能候選…"));
    AppendMenuW(menu, MF_STRING | (preview ? MF_GRAYED : 0), ExportCsvId, tr(L"匯出 CSV…"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (preview ? MF_GRAYED : 0), EnergySaverSettingsId, tr(L"Windows 節能設定…"));
    const auto saver = querySaverMode();
    const bool saverActionable = !preview && !saver.error && (!saver.efficient || saver.restorable);
    AppendMenuW(menu, MF_STRING | (saverActionable ? 0 : MF_GRAYED) | (saver.efficient ? MF_CHECKED : 0),
        SaverModeId, saver.restorable ? tr(L"還原原本電池電源模式") : saver.efficient ? tr(L"已使用最佳效率") : tr(L"一鍵省電 · 最佳效率"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    HMENU alerts = CreatePopupMenu();
    const std::pair<int, unsigned> levels[] = {{LowBatteryOffId, 0}, {LowBattery10Id, 10}, {LowBattery20Id, 20}, {LowBattery30Id, 30}};
    for (const auto& level : levels)
        AppendMenuW(alerts, MF_STRING | (lowBatteryThreshold == level.second ? MF_CHECKED : 0) | (preview ? MF_GRAYED : 0), level.first,
            level.second ? (std::to_wstring(level.second) + L"%").c_str() : tr(L"關閉"));
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(alerts), tr(L"低電量提醒"));
    AppendMenuW(menu, MF_STRING | (hotkeyEnabled ? MF_CHECKED : 0) | (preview ? MF_GRAYED : 0), HotkeyToggleId, tr(L"快速鍵 Ctrl+Alt+B"));
    AppendMenuW(menu, MF_STRING | (autoStart ? MF_CHECKED : 0) | (demo ? MF_GRAYED : 0), StartupId, tr(L"登入時啟動"));
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
    refreshSaverButton();
    SetWindowTextW(backgroundCheck, backgroundOnly ? tr(L"僅背景：開") : tr(L"僅背景：關"));
    SetWindowTextW(allCheck, showAll ? tr(L"顯示：全部程序") : tr(L"顯示：前 10 名"));
    SetWindowTextW(list, tr(L"CPU 耗電排行"));
    if (inlinePage == InlinePage::process && inlineProcess) showDetails(*inlineProcess);
    if (inlinePage == InlinePage::power) showPowerDetails();
    if (inlinePage == InlinePage::candidates) showReviewCandidates();
    if (inlinePage == InlinePage::ledger) showLedger();
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
    if (control == saverButton) return tr(L"切換電池供電時的最佳效率模式；可還原原本設定，不會結束程序。這不是 Windows 節能工具開關。");
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
        backgroundCheck = button(tr(L"僅背景：關"), BackgroundId);
        allCheck = button(tr(L"顯示：前 10 名"), AllId);
        saverButton = button(tr(L"省電"), SaverModeId);
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
            saverButton, menuButton, hideButton, inlineBack, inlineConfirm}) {
            TOOLINFOW tool{}; tool.cbSize = sizeof(tool); tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            tool.hwnd = window; tool.uId = reinterpret_cast<UINT_PTR>(control); tool.lpszText = LPSTR_TEXTCALLBACKW;
            SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        }
        autoStart = startupEnabled();
        makeFonts(); layout(); updateButtons(); refreshSaverButton();
        DWORD corners = 2;
        DwmSetWindowAttribute(window, 33, &corners, sizeof(corners));
        applyTheme();
        taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        trayAdded = updateTray(true);
        if (hotkeyEnabled && !demo && !isolatedPreview && !registerPanelHotkey(true)) hotkeyEnabled = false;
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
        if (id == SaverModeId) {
            if (demo || isolatedPreview) return 0;
            const auto before = querySaverMode();
            if (before.error || (before.efficient && !before.restorable)) {
                setNotice(tr(L"無法切換電池電源模式。")); return 0;
            }
            DWORD result = toggleSaverMode();
            if (result) setNotice(tr(L"無法切換電池電源模式：") + errorText(result));
            else if (before.efficient) setNotice(tr(L"已還原原本的電池電源模式。"));
            else {
                PowerSample power = readPower();
                setNotice(power.acKnown && power.onAc ? tr(L"已設定電池供電時使用最佳效率。") : tr(L"已切換至最佳效率；可在選單還原。"));
            }
            refreshSaverButton();
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
        if (id == TrayExit) { if (!pendingAction) DestroyWindow(window); else setNotice(tr(L"請等待目前操作完成後再結束。")); return 0; }
        if (id == BackgroundId || id == AllId) {
            stopScroll(); scrollPosition = 0; listRefreshPending = false;
            if (id == BackgroundId) backgroundOnly = !backgroundOnly; else showAll = !showAll;
            SetWindowTextW(backgroundCheck, backgroundOnly ? tr(L"僅背景：開") : tr(L"僅背景：關"));
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
        if (!demo && !isolatedPreview && !current.history.empty() && lowBattery.update(current.history.back(), lowBatteryThreshold))
            showLowBatteryNotice();
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
    case WM_HOTKEY:
        if (wp == PanelHotkey) toggleMain();
        return 0;
    case TrayMessage:
        if (trayVersion4 ? LOWORD(lp) == NIN_BALLOONUSERCLICK : lp == NIN_BALLOONUSERCLICK) { showMain(); return 0; }
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
        KillTimer(window, NoticeTimer);
        registerPanelHotkey(false);
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
        row.measured = row.ioKnown = row.memoryKnown = true;
        state.processes.push_back(row);
    }
    state.energy.ready = true; state.energy.coreWatts = 3.2; state.energy.packageWatts = 5.0;
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
        saverButton, hideButton, menuButton, inlineText, inlineBack, inlineConfirm}) {
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
    out << "emi_channels=" << state.energy.channels << "\nenergy_ready=" << state.energy.ready
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
    applyLanguage(true);
    check("inline_details_switch_language", inlineTitle == tr(L"程序詳情"));
    applyLanguage(false);
    SendMessageW(mainWindow, WM_COMMAND, IDCANCEL, 0);
    check("escape_returns_to_ranking", inlinePage == InlinePage::none && IsWindowVisible(viewport) && IsWindowVisible(mainWindow));
    showPowerDetails();
    check("power_details_reuse_panel", inlinePage == InlinePage::power && IsWindowVisible(inlineText));
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
    const auto tip = trayText();
    check("tray_tooltip_shows_level_power_and_time", tip.find(L"%") != std::wstring::npos && tip.find(L" W") != std::wstring::npos &&
        tip.find(durationText(current.power.hoursRemaining)) != std::wstring::npos && tip.size() < 128);
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
    check("demo_power_mode_action_disabled", !IsWindowEnabled(saverButton));
    RECT saverBounds{}; GetWindowRect(saverButton, &saverBounds);
    check("saver_button_visible_in_compact_header", IsWindowVisible(saverButton) &&
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
    bool screenshot = args.size() >= 2 && (args[0] == L"--screenshot" || args[0] == L"--demo-screenshot");
    bool trayTest = args.size() == 2 && args[0] == L"--tray-test";
    bool uiTest = trayTest || (args.size() == 2 && args[0] == L"--ui-test");
    demo = uiTest || (!args.empty() && (args[0] == L"--demo" || args[0] == L"--demo-screenshot"));
    isolatedPreview = screenshot || (uiTest && !trayTest);
    suppressMotion = screenshot || uiTest;
    auto option = [&](const wchar_t* value) { return std::find(args.begin(), args.end(), value) != args.end(); };
    if (!demo && !isolatedPreview) loadSettings();
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
        if (existing) { PostMessageW(existing, WM_COMMAND, TrayShow, 0); SetForegroundWindow(existing); }
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES}; InitCommonControlsEx(&controls);
    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.hInstance = instance; wc.lpfnWndProc = windowProc;
    wc.lpszClassName = ClassName; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&wc)) return 2;
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
        if (option(L"--empty")) current.processes.clear();
        if (option(L"--no-energy")) { current.energy.ready = false; for (auto& row : current.processes) { row.averageCpuWatts.reset(); row.cpuWattsLow.reset(); row.cpuWattsHigh.reset(); } }
        current.power = calculatePower(current.history);
        refreshList();
    }
    else worker = std::thread(sampleLoop);
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
    DestroyIcon(appIcon); DestroyIcon(smallIcon); if (headerIcon) DestroyIcon(headerIcon);
    processIcons.clear();
    ui::stopGraphics();
    return shotOK ? 0 : 4;
}
