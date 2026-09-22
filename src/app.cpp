#include "platform.h"
#include "energy.h"
#include <commctrl.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>

namespace {
using namespace bs;
constexpr UINT SampleMessage = WM_APP + 1, TrayMessage = WM_APP + 2, ActionMessage = WM_APP + 3;
constexpr int ListId = 100, CloseId = 101, KillId = 102, ServiceId = 103, DetailsId = 104,
    BackgroundId = 105, AllId = 106, StartupId = 107, HideId = 108, MenuId = 109, TrayShow = 200, TrayExit = 201;
const wchar_t* ClassName = L"BatteryScope.Main.v1";
const COLORREF Ink = RGB(235, 242, 246), Muted = RGB(146, 161, 174), Green = RGB(104, 225, 188),
    Background = RGB(15, 20, 27), Surface = RGB(24, 32, 42), Border = RGB(41, 53, 65),
    Selected = RGB(30, 57, 57), Danger = RGB(245, 146, 146);
HWND mainWindow = nullptr, viewport = nullptr, list = nullptr, closeButton = nullptr, killButton = nullptr, serviceButton = nullptr,
    detailsButton = nullptr, backgroundCheck = nullptr, allCheck = nullptr, hideButton = nullptr, menuButton = nullptr;
HFONT normalFont = nullptr, smallFont = nullptr, titleFont = nullptr, valueFont = nullptr, metricFont = nullptr;
HICON appIcon = nullptr;
UINT dpi = 96, taskbarCreated = 0;
std::atomic<bool> quit{false}, hidden{false}, resetSamples{false};
std::mutex mailboxMutex, wakeMutex;
std::condition_variable wake;
Snapshot mailbox, current;
std::vector<ProcessRow> displayed;
std::vector<std::wstring> displayedLabels;
uint64_t listStructuralChanges = 0;
std::thread worker, actionWorker;
bool pendingAction = false, forceAction = false, serviceAction = false, trayAdded = false, demo = false;
bool trayVersion4 = false, backgroundOnly = false, showAll = false, autoStart = false;
bool isolatedPreview = false;
std::wstring notice = L"正在讀取電池與程序資料…";
uint64_t noticeUntil = 0;
std::deque<uint64_t> actionMarks;
constexpr UINT_PTR MotionTimer = 92;
constexpr int PanelWidth = 360, PanelHeight = 660, RowHeight = 51;
bool motionEnabled = true, suppressMotion = false, panelTargetShown = false, motionTimerRunning = false;
struct Transition {
    double from = 0, to = 0;
    uint64_t started = 0, duration = 0;
    bool active(uint64_t now) const { return duration && now < started + duration; }
    double value(uint64_t now) const {
        if (!active(now)) return to;
        double t = std::clamp(double(now - started) / duration, 0.0, 1.0);
        double ease = 1 - std::pow(1 - t, 3);
        return from + (to - from) * ease;
    }
    void move(double target, uint64_t now, uint64_t ms) {
        from = value(now); to = target; started = now; duration = ms;
    }
};
Transition panelMotion;
Transition scrollMotion;
double scrollPosition = 0;
bool scrollingThumb = false, listRefreshPending = false, applyingScroll = false;
void applyScroll(double position);
void stopScroll();
double appliedPanelProgress = -1;
RECT panelRest{};
POINT panelOffset{};
struct RowMotion { uint32_t pid; uint64_t created; Transition bar; };
std::vector<RowMotion> rowMotions;
std::map<HWND, Transition> buttonMotions;
bool recordMotion = false;
std::vector<uint64_t> motionTicks;
bool canAnimate() { return motionEnabled && !suppressMotion; }
void stepMotion(uint64_t now);
void armMotionTimer() {
    if (!motionTimerRunning) motionTimerRunning = SetTimer(mainWindow, MotionTimer, 16, nullptr) != 0;
    if (!motionTimerRunning) {
        panelMotion.duration = 0;
        scrollMotion.duration = 0;
        for (auto& row : rowMotions) row.bar.duration = 0;
        for (auto& button : buttonMotions) button.second.duration = 0;
        stepMotion(GetTickCount64());
    }
}
void readMotionPreference() {
    BOOL enabled = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0)) motionEnabled = enabled != FALSE;
}

int px(int value) { return MulDiv(value, int(dpi), 96); }
int sp(int value) { return px(MulDiv(value, 85, 100)); }
COLORREF blend(COLORREF a, COLORREF b, double t) {
    return RGB(int(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t),
        int(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t), int(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t));
}
std::wstring number(double value, int precision = 1) {
    std::wostringstream stream; stream << std::fixed << std::setprecision(precision) << value; return stream.str();
}
std::wstring valueText(Value value, const wchar_t* suffix, int precision = 1) {
    return value ? number(*value, precision) + suffix : L"—";
}
std::wstring qualityText(ObservationQuality quality) {
    switch (quality) {
    case ObservationQuality::strong: return L"觀測完整";
    case ObservationQuality::partial: return L"部分觀測";
    case ObservationQuality::limited: return L"觀測有限";
    default: return L"資料不足";
    }
}
void font(HDC dc, HFONT value) { SelectObject(dc, value); }
void drawText(HDC dc, const std::wstring& text, RECT r, COLORREF color, HFONT face, UINT format = DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS) {
    font(dc, face); SetTextColor(dc, color); SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text.c_str(), -1, &r, format | DT_NOPREFIX);
}
void fill(HDC dc, RECT rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color); FillRect(dc, &rect, brush); DeleteObject(brush);
}
void rounded(HDC dc, RECT rect, COLORREF color, int radius = 12) {
    HBRUSH brush = CreateSolidBrush(color); auto oldBrush = SelectObject(dc, brush);
    auto oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, px(radius), px(radius));
    SelectObject(dc, oldPen); SelectObject(dc, oldBrush); DeleteObject(brush);
}
int listTop() {
    RECT r{}; GetClientRect(mainWindow, &r);
    return sp(r.bottom < px(480) ? 270 : r.bottom < px(600) ? 308 : 390);
}
void setNotice(std::wstring text) {
    notice = std::move(text); noticeUntil = GetTickCount64() + 15000;
    InvalidateRect(mainWindow, nullptr, FALSE);
}
void makeFonts() {
    for (auto f : {normalFont, smallFont, titleFont, valueFont, metricFont}) if (f) DeleteObject(f);
    auto make = [](int height, int weight) {
        return CreateFontW(-px(height), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft JhengHei UI");
    };
    normalFont = make(12, FW_NORMAL); smallFont = make(11, FW_NORMAL);
    titleFont = make(16, FW_SEMIBOLD); valueFont = make(29, FW_SEMIBOLD); metricFont = make(16, FW_SEMIBOLD);
    for (auto window : {list, closeButton, killButton, serviceButton, detailsButton, backgroundCheck, allCheck, hideButton, menuButton})
        if (window) SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(normalFont), TRUE);
}
HICON makeIcon() {
    HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
    HBITMAP color = CreateCompatibleBitmap(screen, 32, 32), mask = CreateBitmap(32, 32, 1, 1, nullptr);
    HGDIOBJ old = SelectObject(dc, color);
    fill(dc, {0, 0, 32, 32}, Background);
    fill(dc, {5, 9, 25, 23}, Green);
    fill(dc, {25, 13, 28, 19}, Green);
    fill(dc, {7, 11, 23, 21}, Background);
    fill(dc, {9, 13, 19, 19}, Green);
    SelectObject(dc, mask); PatBlt(dc, 0, 0, 32, 32, BLACKNESS);
    SelectObject(dc, old);
    ICONINFO info{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(color); DeleteObject(mask); DeleteDC(dc); ReleaseDC(nullptr, screen); return icon;
}
bool updateTray(bool add) {
    if (isolatedPreview) return false;
    NOTIFYICONDATAW data{}; data.cbSize = sizeof(data); data.hWnd = mainWindow; data.uID = 1;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP; data.uCallbackMessage = TrayMessage; data.hIcon = appIcon;
    std::wstring text = L"BatteryScope";
    if (!current.history.empty()) {
        const auto& p = current.history.back();
        text += L" · " + valueText(p.percent, L"%", 0);
        text += p.onAc ? L" · 插電中" : L" · " + valueText(current.power.currentWatts, L" W");
    }
    wcsncpy_s(data.szTip, text.c_str(), _TRUNCATE);
    if (!Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &data)) return false;
    if (add) { data.uVersion = NOTIFYICON_VERSION_4; trayVersion4 = Shell_NotifyIconW(NIM_SETVERSION, &data) != FALSE; }
    return true;
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
void stepMotion(uint64_t now) {
    if (recordMotion && motionTicks.size() < 128) motionTicks.push_back(now);
    bool active = panelMotion.active(now);
    if (scrollMotion.duration) {
        applyScroll(scrollMotion.value(now));
        bool moving = scrollMotion.active(now) && !hidden;
        active |= moving;
        if (!moving) {
            scrollMotion.duration = 0;
            if (listRefreshPending && !scrollingThumb) { listRefreshPending = false; refreshList(); }
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
    bool rowsActive = false;
    for (const auto& row : rowMotions) rowsActive |= row.bar.active(now);
    if (!hidden && rowsActive) InvalidateRect(list, nullptr, FALSE);
    active |= !hidden && rowsActive;
    for (auto& item : buttonMotions) {
        if (item.second.duration) {
            InvalidateRect(item.first, nullptr, FALSE);
            active |= !hidden && item.second.active(now);
            if (!item.second.active(now)) item.second.duration = 0;
        }
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
    if (!wasVisible) { positionPanel(); panelMotion = {}; refreshList(); }
    uint64_t now = GetTickCount64(); panelMotion.move(1, now, canAnimate() ? 220 : 0);
    SetLayeredWindowAttributes(mainWindow, 0, BYTE(panelMotion.value(now) * 255), LWA_ALPHA);
    ShowWindow(mainWindow, SW_SHOWNORMAL); stepMotion(now);
    SetForegroundWindow(mainWindow); wake.notify_one();
    if (panelMotion.active(now)) armMotionTimer();
}
void hideMain() {
    if (!panelTargetShown && hidden) return;
    if (!trayAdded && !isolatedPreview) trayAdded = updateTray(true);
    if (!trayAdded && !isolatedPreview) { setNotice(L"系統匣無法使用，面板會保持開啟。"); return; }
    stopScroll();
    panelTargetShown = false; hidden = true; wake.notify_one();
    uint64_t now = GetTickCount64(); panelMotion.move(0, now, canAnimate() ? 150 : 0);
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
    EnableWindow(serviceButton, ready && !row->services.empty());
    EnableWindow(detailsButton, row.has_value());
}
void fitListColumn() {
    RECT r{}; GetClientRect(viewport, &r);
    ListView_SetColumnWidth(list, 0, std::max(1, int(r.right) - px(2)));
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
    scrollMotion.from = scrollPosition; scrollMotion.to = target;
    scrollMotion.started = GetTickCount64();
    scrollMotion.duration = !immediate && canAnimate() && !hidden && std::abs(target - scrollPosition) > .5 ? 180 : 0;
    if (scrollMotion.duration) armMotionTimer(); else applyScroll(target);
}
void wheelScroll(WPARAM wp) {
    UINT lines = 3; SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    RECT r{}; GetClientRect(viewport, &r);
    double distance = lines == WHEEL_PAGESCROLL ? r.bottom : double(lines) * px(RowHeight);
    double delta = -double(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA * distance;
    double pending = scrollMotion.to - scrollPosition;
    double base = scrollMotion.active(GetTickCount64()) && delta * pending > 0 ? scrollMotion.to : scrollPosition;
    scrollTo(base + delta);
}
void refreshList() {
    listRefreshPending = false;
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
    uint64_t now = GetTickCount64();
    std::map<std::pair<uint32_t, uint64_t>, Transition> previous;
    for (const auto& motion : rowMotions) previous[{motion.pid, motion.created}] = motion.bar;
    rowMotions.clear();
    double maximum = next.empty() ? 0 : next.front().averageCpuWatts.value_or(0);
    for (const auto& row : next) {
        Transition bar = previous[{row.pid, row.created}];
        double target = maximum > 0 && row.averageCpuWatts ? std::clamp(*row.averageCpuWatts / maximum, 0.0, 1.0) : 0;
        bool changed = std::abs(bar.to - target) > .001;
        bar.move(target, now, changed && canAnimate() && !hidden ? 260 : 0);
        if (bar.active(now)) armMotionTimer();
        rowMotions.push_back({row.pid, row.created, bar});
    }
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    const size_t oldCount = displayedLabels.size();
    for (size_t i = oldCount; i > next.size(); --i) { ListView_DeleteItem(list, int(i - 1)); ++listStructuralChanges; }
    ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    displayed = std::move(next);
    displayedLabels.resize(displayed.size());
    for (size_t i = 0; i < displayed.size(); ++i) {
        const auto& row = displayed[i];
        std::wstring displayName = row.services.size() == 1 ? row.services.front().display : row.name;
        if (row.services.size() > 1) displayName += L"（" + std::to_wstring(row.services.size()) + L" 項服務）";
        std::wstring accessible = displayName + L", PID " + std::to_wstring(row.pid) + L", " + row.kind +
            L", 估計 CPU " + valueText(row.averageCpuWatts, L" W", 2);
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
    MoveWindow(menuButton, w - sp(92), sp(16), sp(32), sp(30), TRUE);
    MoveWindow(hideButton, w - sp(52), sp(16), sp(32), sp(30), TRUE);
    InvalidateRect(mainWindow, nullptr, FALSE);
}
void drawButton(const DRAWITEMSTRUCT& item) {
    RECT r = item.rcItem; HDC dc = item.hDC;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool checked = (item.CtlID == BackgroundId && backgroundOnly) || (item.CtlID == AllId && showAll);
    const bool danger = item.CtlID == KillId;
    fill(dc, r, Background);
    double hover = buttonMotions[item.hwndItem].value(GetTickCount64());
    if (pressed) InflateRect(&r, -px(1), -px(1));
    rounded(dc, r, pressed ? Border : blend(checked ? Selected : Surface, Border, disabled ? 0 : hover), 10);
    wchar_t title[128]{}; GetWindowTextW(item.hwndItem, title, int(std::size(title)));
    drawText(dc, title, r, disabled ? RGB(87, 102, 115) : danger ? Danger : checked ? Green : Ink,
        normalFont, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    if (item.itemState & ODS_FOCUS) {
        InflateRect(&r, -sp(3), -sp(3)); SetTextColor(dc, Muted); DrawFocusRect(dc, &r);
    }
}
void drawProcess(const DRAWITEMSTRUCT& item) {
    if (item.itemID >= displayed.size()) return;
    const auto& row = displayed[item.itemID]; HDC dc = item.hDC; RECT r = item.rcItem;
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    fill(dc, r, Background);
    RECT tile = r; tile.bottom -= sp(4); rounded(dc, tile, selected ? Selected : Surface, 10);
    const int left = r.left + sp(38), right = r.right - sp(12), y = r.top;
    drawText(dc, std::to_wstring(item.itemID + 1), {r.left + sp(10), y + sp(10), left - sp(6), y + sp(29)}, Muted, smallFont);
    std::wstring name = row.services.size() == 1 ? row.services.front().display : row.name;
    if (row.services.size() > 1) name += L" · " + std::to_wstring(row.services.size()) + L" 項服務";
    drawText(dc, name, {left, y + sp(7), right - sp(90), y + sp(28)}, Ink, normalFont);
    drawText(dc, valueText(row.averageCpuWatts, L" W", 2), {right - sp(90), y + sp(6), right, y + sp(29)},
        Green, metricFont, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    drawText(dc, row.kind + L" · " + std::to_wstring(row.pid), {left, y + sp(29), right - sp(100), y + sp(46)}, Muted, smallFont);
    std::wstring range = row.cpuWattsLow && row.cpuWattsHigh ?
        number(*row.cpuWattsLow, 2) + L"–" + number(*row.cpuWattsHigh, 2) + L" W" :
        L"CPU " + (row.measured ? number(row.cpu) + L"%" : L"—");
    drawText(dc, range, {right - sp(102), y + sp(29), right, y + sp(46)}, Muted, smallFont, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    if (row.averageCpuWatts && !displayed.empty() && displayed.front().averageCpuWatts && *displayed.front().averageCpuWatts > 0) {
        const double ratio = item.itemID < rowMotions.size() ? rowMotions[item.itemID].bar.value(GetTickCount64()) : 0;
        fill(dc, {left, y + sp(50), right, y + sp(52)}, Border);
        fill(dc, {left, y + sp(50), left + int((right - left) * ratio), y + sp(52)}, selected ? Green : RGB(57, 119, 105));
    }
    if (item.itemState & ODS_FOCUS) { InflateRect(&tile, -sp(2), -sp(2)); DrawFocusRect(dc, &tile); }
}
void paint(HDC dc, RECT bounds) {
    fill(dc, bounds, Background);
    const int w = bounds.right, h = bounds.bottom, margin = sp(18), right = w - margin;
    const bool compact = h < px(600), minimal = h < px(480);
    const int top = listTop();
    rounded(dc, {margin, sp(21), margin + sp(8), sp(29)}, Green, 8);
    drawText(dc, L"BatteryScope", {margin + sp(18), sp(12), w - sp(98), sp(42)}, Ink, titleFont);
    drawText(dc, demo ? L"示範資料 · 關閉操作已停用" : L"電池與背景活動", {margin, sp(43), right, sp(63)}, Muted, smallFont);
    PowerSample p; if (!current.history.empty()) p = current.history.back();
    const auto& m = current.power;
    const bool plugged = p.acKnown && (p.onAc || p.charging);
    std::wstring currentW = !p.present ? L"無電池" : plugged ? L"插電中" : valueText(m.currentWatts, L" W");
    std::wstring rateLabel = !p.present ? L"仍可監測程序 CPU" : plugged ? L"充電功率不代表整機耗電" :
        m.estimated ? L"依電量變化估算" : L"電池回報的整機放電";
    if (minimal && m.percentPerHour) rateLabel = number(*m.percentPerHour) + L" 百分點 / 小時";
    rounded(dc, {margin, sp(74), right, sp(180)}, Surface, 18);
    drawText(dc, L"目前耗電", {margin + sp(16), sp(85), right - sp(122), sp(105)}, Muted, smallFont);
    drawText(dc, currentW, {margin + sp(14), sp(105), right - sp(119), sp(151)}, Ink, valueFont);
    drawText(dc, rateLabel, {margin + sp(16), sp(153), right - sp(100), sp(172)}, Muted, smallFont);
    drawText(dc, valueText(p.percent, L"%", 0), {right - sp(108), sp(92), right - sp(16), sp(126)},
        Green, titleFont, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    fill(dc, {right - sp(92), sp(136), right - sp(16), sp(140)}, Border);
    if (p.percent) fill(dc, {right - sp(92), sp(136), right - sp(92) + int(sp(76) * std::clamp(*p.percent, 0.0, 100.0) / 100), sp(140)}, Green);
    drawText(dc, p.charging ? L"充電中" : plugged ? L"已接電源" : L"剩餘電量",
        {right - sp(100), sp(151), right - sp(16), sp(172)}, Muted, smallFont, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    if (!minimal) {
        int middle = w / 2;
        drawText(dc, L"耗電速度", {margin + sp(2), sp(192), middle, sp(209)}, Muted, smallFont);
        drawText(dc, valueText(m.percentPerHour, L" 百分點 / 小時"), {margin + sp(2), sp(211), middle, sp(235)}, Ink, normalFont);
        std::wstring remaining = L"估計續航 —";
        if (m.hoursRemaining && *m.hoursRemaining < 100) {
            int minutes = int(*m.hoursRemaining * 60);
            remaining = L"約 " + std::to_wstring(minutes / 60) + L" 小時 " + std::to_wstring(minutes % 60) + L" 分";
        }
        drawText(dc, L"近 5 分鐘 " + valueText(m.averageWatts, L" W"), {middle + sp(8), sp(192), right, sp(209)}, Muted, smallFont);
        drawText(dc, remaining, {middle + sp(8), sp(211), right, sp(235)}, Ink, normalFont);
    }
    if (!compact) {
        RECT chart{margin, sp(246), right, sp(306)};
        rounded(dc, chart, Surface, 12);
        drawText(dc, L"30 分鐘 · 放電 W", {chart.left + sp(12), chart.top + sp(6), chart.right - sp(12), chart.top + sp(22)}, Muted, smallFont);
        RECT plot{chart.left + sp(12), chart.top + sp(27), chart.right - sp(12), chart.bottom - sp(9)};
        double maximum = 10;
        uint64_t last = current.history.empty() ? GetTickCount64() : current.history.back().ms;
        for (const auto& point : current.history) if (point.watts && *point.watts >= 0 && point.acKnown && !point.onAc && !point.charging)
            maximum = std::max(maximum, std::ceil(*point.watts / 5) * 5);
        fill(dc, {plot.left, plot.bottom, plot.right, plot.bottom + 1}, Border);
        HPEN linePen = CreatePen(PS_SOLID, std::max(1, sp(2)), Green); HGDIOBJ oldPen = SelectObject(dc, linePen);
        bool connected = false, any = false; uint64_t previous = 0;
        for (const auto& point : current.history) {
            if (!point.watts || *point.watts < 0 || !point.present || !point.acKnown || point.onAc || point.charging) { connected = false; continue; }
            int x = plot.right - int((last - point.ms) / 1800000.0 * (plot.right - plot.left));
            int y = plot.bottom - int(*point.watts / maximum * (plot.bottom - plot.top));
            if (connected && point.ms - previous <= 30000) { LineTo(dc, x, y); any = true; }
            else MoveToEx(dc, x, y, nullptr);
            connected = true; previous = point.ms;
        }
        SelectObject(dc, oldPen); DeleteObject(linePen);
        HPEN marker = CreatePen(PS_DOT, 1, RGB(218, 174, 105)); oldPen = SelectObject(dc, marker);
        for (auto time : actionMarks) if (time <= last && last - time <= 1800000) {
            int x = plot.right - int((last - time) / 1800000.0 * (plot.right - plot.left));
            MoveToEx(dc, x, plot.top, nullptr); LineTo(dc, x, plot.bottom);
        }
        SelectObject(dc, oldPen); DeleteObject(marker);
        if (!any) drawText(dc, plugged ? L"插電期間暫停放電曲線" : L"等待放電資料", plot, Muted, smallFont, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        drawText(dc, L"0–" + number(maximum, 0) + L" W", {chart.right - sp(90), chart.top + sp(6), chart.right - sp(12), chart.top + sp(22)},
            Muted, smallFont, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    }
    const bool energyReady = current.energy.ready && !current.energy.timingMismatch;
    drawText(dc, L"CPU 耗電排行", {margin, top - sp(76), right, top - sp(53)}, Ink, normalFont);
    drawText(dc, energyReady ? L"估計 W ↓" : L"活動排序 ↓", {right - sp(105), top - sp(76), right, top - sp(53)}, Green, smallFont, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    drawText(dc, energyReady ? L"30 秒平滑 · " + qualityText(current.energy.quality) + L" " + number(current.energy.observedCpuCoverage * 100, 0) + L"%" : L"能源暫不可用，依 CPU / I/O 活動排序",
        {margin, top - sp(57), right, top - sp(40)}, Muted, smallFont);
    if (displayed.empty()) {
        drawText(dc, backgroundOnly ? L"目前沒有符合篩選的程序" : L"正在讀取程序…",
            {margin, top + sp(12), right, top + sp(44)}, Muted, normalFont, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
    int footer = h - sp(51);
    std::wstring status = GetTickCount64() < noticeUntil || current.processes.empty() ? notice :
        L"已監測 " + std::to_wstring(current.processes.size()) + L" · 操作資訊受限 " + std::to_wstring(current.inaccessible) +
        (current.servicesKnown ? L" · 每 " + std::to_wstring(current.samplingSeconds) + L" 秒更新" : L" · 服務資料不完整");
    drawText(dc, status, {margin, footer, right, footer + sp(19)}, Muted, smallFont);
    drawText(dc, L"個別瓦數僅估計 CPU，非程式總耗電",
        {margin, footer + sp(21), right, footer + sp(39)}, Muted, smallFont);
}
bool startupEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    DWORD size = 0; LSTATUS status = RegQueryValueExW(key, L"BatteryScope", nullptr, nullptr, nullptr, &size);
    RegCloseKey(key); return status == ERROR_SUCCESS;
}
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
    updateButtons(); setNotice(service ? L"正在停止服務…" : force ? L"正在結束程序…" : L"正在傳送正常關閉要求…");
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
    for (const auto& service : row.services) labels.push_back(service.display + L" (" + service.name + L")" + (service.stoppable ? L"" : L" — 不接受停止"));
    std::vector<TASKDIALOG_BUTTON> choices;
    for (size_t i = 0; i < labels.size(); ++i) choices.push_back({300 + int(i), labels[i].c_str()});
    TASKDIALOG_BUTTON button{101, L"停止選取的服務"};
    TASKDIALOGCONFIG config{}; config.cbSize = sizeof(config); config.hwndParent = mainWindow;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    config.dwCommonButtons = TDCBF_CANCEL_BUTTON; config.pszWindowTitle = L"BatteryScope · 停止服務";
    config.pszMainInstruction = L"選擇要停止的服務";
    config.pszContent = L"此操作可能中斷相關功能。共用程序內只停止選取服務，不結束整個程序。\n有執行中的相依服務時會拒絕操作。需要時 Windows 會要求管理員權限。";
    config.cRadioButtons = UINT(choices.size()); config.pRadioButtons = choices.data(); config.nDefaultRadioButton = 300;
    config.cButtons = 1; config.pButtons = &button; config.nDefaultButton = IDCANCEL;
    int chosen = 0, pressed = 0;
    if (SUCCEEDED(TaskDialogIndirect(&config, &pressed, &chosen, nullptr)) && pressed == 101 && chosen >= 300 && size_t(chosen - 300) < row.services.size()) {
        auto service = row.services[size_t(chosen - 300)];
        if (!service.stoppable) { setNotice(L"此服務目前不接受停止要求。"); return; }
        startAction(row, false, service);
    }
}
void showDetails(const ProcessRow& row) {
    std::wstring content = row.name + L"\nPID: " + std::to_wstring(row.pid) + L"\n\n" + row.path +
        L"\n\nCPU（全機比例）: " + number(row.cpu) + L"%\n30 秒平滑 CPU: " + number(row.averageCpu) +
        L"%\nI/O: " + (row.ioKnown ? number(row.ioMB, 2) + L" MB/s" : L"未知") +
        L"\n記憶體: " + (row.memoryKnown ? number(row.memoryMB) + L" MB（" + number(row.memoryAgeSeconds, 0) + L" 秒前）" : L"未知");
    if (row.cpuWatts) {
        content += L"\n\n本次 CPU 估計: " + valueText(row.cpuWatts, L" W", 2) +
            L"\n30 秒平滑 CPU 估計: " + valueText(row.averageCpuWatts, L" W", 2) +
            L"\n30 秒平滑的核心分攤模型範圍: " + valueText(row.cpuWattsLow, L"", 2) + L"–" + valueText(row.cpuWattsHigh, L" W", 2) +
            L"\n監測期間 CPU 估計能量: " + valueText(row.cpuMilliWh, L" mWh", 2);
        content += L"\n本次固定核心資訊覆蓋: " + number(row.placementCoverage*100,0) + L"%";
        content += L"\n全機 CPU 時間覆蓋: " + number(current.energy.observedCpuCoverage*100,0) +
            L"%（可讀程序時間／系統忙碌時間，非準確率）";
        content += L"\n觀測品質: " + qualityText(current.energy.quality) +
            L" · 採樣端點差 " + number(current.energy.sampleSkewMs, 0) + L" ms";
        if (!current.history.empty() && current.history.back().fullWh && row.averageCpuWatts)
            content += L"\nCPU 部分估計耗電速度: " + number(*row.averageCpuWatts / *current.history.back().fullWh * 100, 2) + L" 百分點／小時";
        content += L"\n\n用 EMI 量到的核心能源約束分攤。持續固定在同一核心的執行緒優先按該核心分配；其餘依 CPU 時間估計。範圍反映未知的核心排程位置，並非統計信賴區間；不同指令與 SMT 仍可能產生誤差。";
    }
    content += L"\n\n本次 CPU 核心 " + valueText(current.energy.coreWatts, L" W") +
        L" · CPU 未歸因 " + valueText(current.energy.unassignedCpuWatts, L" W") +
        L"\nCPU 以外／未歸因 " + valueText(current.energy.otherSystemWatts, L" W");
    content += L"\n\n這是 CPU 能源分攤（包含活躍核心的共用開銷），不是程式總功率或關閉後保證能省的瓦數。GPU、螢幕、儲存等沒有硬分配給程序。";
    if (!row.actionable && row.services.empty()) content += L"\n\n此程序受保護、屬於其他使用者，或無法確認安全狀態，因此不提供關閉操作。";
    if (!row.services.empty()) {
        content += L"\n\n所屬服務（共用程序時無法分別歸因）：";
        for (const auto& s : row.services) content += L"\n• " + s.display + L" (" + s.name + L")";
    }
    MessageBoxW(mainWindow, content.c_str(), L"程序詳情", MB_OK | MB_ICONINFORMATION);
}

void sampleLoop() {
    Collector collector;
    EnergyReader energyReader;
    CpuAttributor attributor;
    SamplingPolicy policy;
    Snapshot state;
    uint64_t nextProcess = 0, nextPower = 0;
    bool previousHidden = false, qosInitialized = false;
    while (!quit) {
        auto now = GetTickCount64(); bool background = hidden.load();
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
            energyReader.sampleThreadPlacement(state.processes);
            attributor.apply(state.processes, state.energy, collector.sampleMs(), std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));
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
            changed = true;
        }
        if (changed) {
            state.ms = now;
            { std::lock_guard<std::mutex> lock(mailboxMutex); mailbox = state; }
            PostMessageW(mainWindow, SampleMessage, 0, 0);
        }
        now = GetTickCount64(); uint64_t next = std::min(nextProcess, nextPower);
        std::unique_lock<std::mutex> lock(wakeMutex);
        wake.wait_for(lock, std::chrono::milliseconds(next > now ? next - now : 1));
    }
}

LRESULT CALLBACK listProc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
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
        double target = scrollMotion.active(GetTickCount64()) ? scrollMotion.to : scrollPosition;
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
    if (message == WM_MOUSEMOVE || message == WM_MOUSELEAVE) {
        double target = message == WM_MOUSEMOVE ? 1 : 0;
        auto& motion = buttonMotions[window];
        if (motion.to != target) {
            motion.move(target, GetTickCount64(), canAnimate() && !hidden ? 120 : 0);
            if (target) { TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0}; TrackMouseEvent(&tracking); }
            InvalidateRect(window, nullptr, FALSE);
            if (motion.duration) armMotionTimer();
        }
    }
    if (message == WM_NCDESTROY) { buttonMotions.erase(window); RemoveWindowSubclass(window, buttonProc, 1); }
    return DefSubclassProc(window, message, wp, lp);
}
void openMenu(POINT point) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, TrayShow, L"顯示面板");
    AppendMenuW(menu, MF_STRING | (autoStart ? MF_CHECKED : 0) | (demo ? MF_GRAYED : 0), StartupId, L"登入時啟動");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (pendingAction ? MF_GRAYED : 0), TrayExit, L"結束 BatteryScope");
    SetForegroundWindow(mainWindow);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, mainWindow, nullptr);
    DestroyMenu(menu); PostMessageW(mainWindow, WM_NULL, 0, 0);
}
LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == taskbarCreated && taskbarCreated) { trayAdded = updateTray(true); if (!trayAdded && hidden) showMain(); return 0; }
    switch (message) {
    case WM_CREATE: {
        mainWindow = window;
        dpi = GetDpiForWindow(window);
        readMotionPreference();
        viewport = CreateWindowExW(WS_EX_CONTROLPARENT, L"STATIC", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_VSCROLL,
            0, 0, 0, 0, window, nullptr, nullptr, nullptr);
        SetWindowSubclass(viewport, viewportProc, 1, 0);
        SetWindowTheme(viewport, L"DarkMode_Explorer", nullptr);
        list = CreateWindowExW(0, WC_LISTVIEWW, L"CPU 耗電排行", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOCOLUMNHEADER | LVS_OWNERDRAWFIXED,
            0, 0, 0, 0, viewport, reinterpret_cast<HMENU>(ListId), nullptr, nullptr);
        SetWindowSubclass(list, listProc, 1, 0);
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        SetWindowTheme(list, L"DarkMode_Explorer", nullptr);
        ListView_SetBkColor(list, Background); ListView_SetTextBkColor(list, Background); ListView_SetTextColor(list, Ink);
        const wchar_t* titles[] = {L"程序與 CPU 估計瓦數"};
        for (int i = 0; i < int(std::size(titles)); ++i) {
            LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT; column.pszText = const_cast<wchar_t*>(titles[i]);
            column.cx = px(120); column.fmt = i >= 3 && i <= 6 ? LVCFMT_RIGHT : LVCFMT_LEFT; ListView_InsertColumn(list, i, &column);
        }
        auto button = [&](const wchar_t* text, int id, DWORD style = BS_OWNERDRAW) {
            HWND control = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(INT_PTR(id)), nullptr, nullptr);
            SetWindowSubclass(control, buttonProc, 1, 0); return control;
        };
        closeButton = button(L"正常關閉", CloseId); killButton = button(L"強制結束…", KillId);
        serviceButton = button(L"停止服務…", ServiceId); detailsButton = button(L"查看詳情", DetailsId);
        backgroundCheck = button(L"僅背景：關", BackgroundId);
        allCheck = button(L"顯示：前 10 名", AllId);
        menuButton = button(L"⋯", MenuId); hideButton = button(L"⌄", HideId);
        autoStart = startupEnabled();
        makeFonts(); layout(); updateButtons();
        BOOL dark = TRUE; DWORD corners = 2;
        DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
        DwmSetWindowAttribute(window, 33, &corners, sizeof(corners));
        DwmSetWindowAttribute(window, 34, &Border, sizeof(Border));
        taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        trayAdded = updateTray(true);
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
        dpi = HIWORD(wp); makeFonts(); auto r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(window, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(list, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        layout(); return 0;
    }
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) hideMain(); else layout(); return 0;
    case WM_TIMER:
        if (wp == MotionTimer) { stepMotion(GetTickCount64()); return 0; }
        break;
    case WM_SETTINGCHANGE:
        readMotionPreference();
        if (!canAnimate()) {
            panelMotion.duration = 0;
            if (scrollMotion.duration) { applyScroll(scrollMotion.to); stopScroll(); }
            for (auto& row : rowMotions) row.bar.duration = 0;
            for (auto& button : buttonMotions) button.second.duration = 0;
            stepMotion(GetTickCount64());
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
    case WM_CTLCOLORBTN:
        SetBkColor(reinterpret_cast<HDC>(wp), Background); SetDCBrushColor(reinterpret_cast<HDC>(wp), Background);
        return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR*>(lp)->idFrom == ListId) {
            if (reinterpret_cast<NMHDR*>(lp)->code == LVN_GETEMPTYMARKUP) {
                auto empty = reinterpret_cast<NMLVEMPTYMARKUP*>(lp); empty->dwFlags = EMF_CENTERED;
                wcscpy_s(empty->szMarkup, backgroundOnly ? L"目前沒有符合篩選的程序" : L"正在讀取程序…"); return TRUE;
            }
            if (reinterpret_cast<NMHDR*>(lp)->code == LVN_ITEMCHANGED) updateButtons();
            if (reinterpret_cast<NMHDR*>(lp)->code == NM_DBLCLK) { auto row = selection(); if (row) showDetails(*row); }
        }
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDCANCEL || id == HideId) { hideMain(); return 0; }
        if (id == MenuId) { RECT r{}; GetWindowRect(menuButton, &r); openMenu({r.left, r.bottom}); return 0; }
        if (id == TrayShow) { showMain(); return 0; }
        if (id == TrayExit) { if (!pendingAction) DestroyWindow(window); else setNotice(L"請等待目前操作完成後再結束。"); return 0; }
        if (id == BackgroundId || id == AllId) {
            stopScroll(); scrollPosition = 0; listRefreshPending = false;
            if (id == BackgroundId) backgroundOnly = !backgroundOnly; else showAll = !showAll;
            SetWindowTextW(backgroundCheck, backgroundOnly ? L"僅背景：開" : L"僅背景：關");
            SetWindowTextW(allCheck, showAll ? L"顯示：全部程序" : L"顯示：前 10 名");
            refreshList(); InvalidateRect(window, nullptr, FALSE); return 0;
        }
        if (id == StartupId) {
            if (demo) return 0;
            DWORD result = setStartup(!autoStart); autoStart = startupEnabled();
            if (result) setNotice(errorText(result));
            else setNotice(autoStart ? L"已設定登入時啟動至系統匣。" : L"已取消登入時啟動。");
            return 0;
        }
        auto row = selection(); if (!row) return 0;
        if (id == DetailsId) { showDetails(*row); return 0; }
        if (pendingAction || demo) return 0;
        if (id == CloseId && row->actionable && row->window) startAction(*row, false);
        if (id == KillId && row->actionable) {
            std::wstring text = L"強制結束 " + row->name + L"（PID " + std::to_wstring(row->pid) + L"）？\n\n未儲存資料可能遺失。此操作僅結束所選程序，不包含其他子程序。";
            if (MessageBoxW(window, text.c_str(), L"確認強制結束", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) startAction(*row, true);
        }
        if (id == ServiceId && !row->services.empty()) chooseService(*row);
        return 0;
    }
    case SampleMessage: {
        { std::lock_guard<std::mutex> lock(mailboxMutex); current = mailbox; }
        if (!hidden) {
            if (scrollMotion.active(GetTickCount64()) || scrollingThumb) listRefreshPending = true;
            else refreshList();
            InvalidateRect(window, nullptr, FALSE);
        }
        if (trayAdded) updateTray(false);
        return 0;
    }
    case ActionMessage: {
        pendingAction = false; if (actionWorker.joinable()) actionWorker.join();
        if (!wp) {
            setNotice(serviceAction ? L"服務已停止。" : forceAction ? L"程序已結束。" : L"已傳送關閉要求；程式可能等待儲存確認。");
            actionMarks.push_back(GetTickCount64()); while (actionMarks.size() > 100) actionMarks.pop_front();
        } else {
            std::wstring text = wp == ERROR_NOT_FOUND ? L"程序已結束或身分已變更，未執行操作。" :
                wp == ERROR_DEPENDENT_SERVICES_RUNNING ? L"有其他執行中的服務依賴此服務，未停止。" :
                wp == ERROR_ACCESS_DISABLED_BY_POLICY ? L"此服務屬於受保護的系統功能，未停止。" :
                wp == ERROR_CANCELLED ? L"已取消管理員授權。" : L"操作未完成：" + errorText(DWORD(wp));
            setNotice(text); MessageBoxW(window, text.c_str(), L"BatteryScope", MB_OK | MB_ICONINFORMATION);
        }
        resetSamples = true; wake.notify_one(); updateButtons(); return 0;
    }
    case WM_POWERBROADCAST:
        if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND || wp == PBT_APMPOWERSTATUSCHANGE) { resetSamples = true; wake.notify_one(); }
        return TRUE;
    case TrayMessage:
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
        row.reason = i == 3 ? L"CPU 活動・共用程序" : L"CPU 活動・持續 42 秒";
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
    for (auto child : {viewport, closeButton, killButton, serviceButton, detailsButton, backgroundCheck, allCheck, hideButton, menuButton}) {
        RECT childRect; GetWindowRect(child, &childRect); MapWindowPoints(nullptr, mainWindow, reinterpret_cast<POINT*>(&childRect), 2);
        SaveDC(dc); SetViewportOrgEx(dc, childRect.left, childRect.top, nullptr);
        IntersectClipRect(dc, 0, 0, childRect.right - childRect.left, childRect.bottom - childRect.top);
        SendMessageW(child, WM_PRINT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_NONCLIENT | PRF_CHILDREN | PRF_ERASEBKGND); RestoreDC(dc, -1);
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
    state.processes = collector.collect(state.totalProcesses, state.inaccessible); energyReader.sampleThreadPlacement(state.processes); Sleep(2100);
    state.energy = energyReader.sample();
    state.processes = collector.collect(state.totalProcesses, state.inaccessible);
    alignProcessObservation(state.energy, collector.intervalSeconds(), collector.sampleMs(), collector.systemSnapshot());
    energyReader.sampleThreadPlacement(state.processes);
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
    check("observation_quality_is_not_labeled_accuracy", qualityText(ObservationQuality::partial) == L"部分觀測");
    ListView_SetItemState(list, 1, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    auto structuralBefore = listStructuralChanges;
    auto before = selection(); refreshList(); auto after = selection();
    check("refresh_reuses_native_items", listStructuralChanges == structuralBefore);
    check("selection_survives_refresh", before && after && before->pid == after->pid && before->created == after->created);
    SendMessageW(mainWindow, WM_COMMAND, BackgroundId, 0);
    check("background_filter", backgroundOnly && displayed.size() == current.processes.size() - 1);
    SendMessageW(mainWindow, WM_COMMAND, AllId, 0);
    check("all_processes_toggle", showAll);
    check("demo_destructive_actions_disabled", !IsWindowEnabled(killButton) && !IsWindowEnabled(serviceButton));
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
            while (PeekMessageW(&event, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&event); DispatchMessageW(&event); }
            Sleep(1);
        }
    };
    bool preference = motionEnabled; suppressMotion = false; motionEnabled = true;
    motionTicks.clear(); recordMotion = true;
    showMain();
    check("animated_show_starts_without_blocking", panelMotion.active(GetTickCount64()) && panelTargetShown && IsWindowVisible(mainWindow));
    pump(80);
    BYTE alpha = 255; DWORD flags = 0; GetLayeredWindowAttributes(mainWindow, nullptr, &alpha, &flags);
    check("visible_intermediate_fade_frame", alpha > 0 && alpha < 255);
    hideMain(); pump(30); toggleMain();
    check("rapid_reopen_reverses_exit", panelTargetShown && !hidden && IsWindowVisible(mainWindow) && panelMotion.active(GetTickCount64()));
    pump(320);
    GetLayeredWindowAttributes(mainWindow, nullptr, &alpha, &flags);
    check("animated_open_finishes_opaque", alpha == 255 && IsWindowVisible(mainWindow) && !panelMotion.active(GetTickCount64()));
    current.processes.front().averageCpuWatts = 2.0; refreshList();
    check("power_bars_animate_between_samples", std::any_of(rowMotions.begin(), rowMotions.end(), [](const RowMotion& row) { return row.bar.active(GetTickCount64()); }));
    check("numeric_watts_remain_latest_measurement", !displayed.empty() && displayed.front().averageCpuWatts == 2.0);
    pump(300);
    SendMessageW(detailsButton, WM_MOUSEMOVE, 0, MAKELPARAM(4, 4));
    check("button_hover_transition_started", buttonMotions[detailsButton].active(GetTickCount64()));
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
    check("scroll_has_intermediate_pixel_position", scrollPosition > 0 && scrollPosition < scrollMotion.to && scrollMotion.active(GetTickCount64()));
    RECT itemRect{}; ListView_GetItemRect(list, 0, &itemRect, LVIR_BOUNDS);
    MapWindowPoints(list, viewport, reinterpret_cast<POINT*>(&itemRect), 2);
    check("fractional_scroll_geometry_matches_content", std::abs(itemRect.top + int(std::lround(scrollPosition))) <= 1);
    double prior = scrollPosition; scrollTo(0);
    check("scroll_reverse_does_not_jump", std::abs(scrollPosition - prior) < .001 && scrollMotion.to == 0);
    pump(220);
    check("scroll_reversal_finishes_at_top", scrollPosition == 0 && !scrollMotion.active(GetTickCount64()));
    scrollTo(px(RowHeight) * 3 + px(13), true);
    prior = scrollPosition; refreshList();
    check("refresh_preserves_fractional_scroll", std::abs(scrollPosition - prior) < .001);
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
    check("reduced_motion_scroll_is_immediate", scrollPosition == 0 && !scrollMotion.active(GetTickCount64()));
    motionEnabled = true; scrollTo(px(RowHeight) * 2); pump(40); hideMain();
    check("hide_cancels_pending_scroll", !scrollMotion.active(GetTickCount64()));
    pump(220); showMain(); pump(320);
    scrollTo(px(RowHeight)); pump(240);
    check("scroll_completion_stops_timer", !motionTimerRunning);
    hideMain(); pump(220);
    check("animated_hide_finishes_and_stops_timer", !IsWindowVisible(mainWindow) && hidden && !motionTimerRunning);
    motionEnabled = false; showMain();
    check("reduced_motion_shows_immediately", IsWindowVisible(mainWindow) && !panelMotion.active(GetTickCount64()));
    hideMain();
    check("reduced_motion_hides_immediately", !IsWindowVisible(mainWindow) && !motionTimerRunning);
    out << "animation_steps_observed=" << motionTicks.size() << "\n";
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
    appIcon = makeIcon();
    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.hInstance = instance; wc.lpfnWndProc = windowProc;
    wc.lpszClassName = ClassName; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hIcon = wc.hIconSm = appIcon;
    if (!RegisterClassExW(&wc)) return 2;
    dpi = GetDpiForSystem();
    RECT bounds{0, 0, px(PanelWidth), px(PanelHeight)};
    if (screenshot && args.size() > 2 && args[2] == L"--compact") bounds.bottom = px(520);
    MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor);
    int initialWidth = std::min(bounds.right, monitor.rcWork.right - monitor.rcWork.left - px(20));
    int initialHeight = std::min(bounds.bottom, monitor.rcWork.bottom - monitor.rcWork.top - px(20));
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, ClassName, L"BatteryScope 0.7 · 耗電面板", WS_POPUP | WS_CLIPCHILDREN,
        monitor.rcWork.right - initialWidth - px(10), monitor.rcWork.bottom - initialHeight - px(10), initialWidth, initialHeight, nullptr, nullptr, instance, nullptr);
    if (!window) return 3;
    SetLayeredWindowAttributes(window, 0, 255, LWA_ALPHA);
    if (demo) {
        current = demoSnapshot(); refreshList();
        if (screenshot && args.size() > 2 && args[2] == L"--scrolled") scrollTo(px(17), true);
    }
    else worker = std::thread(sampleLoop);
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
    for (auto f : {normalFont, smallFont, titleFont, valueFont, metricFont}) if (f) DeleteObject(f);
    DestroyIcon(appIcon);
    return shotOK ? 0 : 4;
}
