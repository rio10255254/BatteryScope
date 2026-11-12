#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h> // before windows.h
#include <ws2tcpip.h>
#include "lab.h"
#include "i18n.h"
#include <shlobj.h>
#include <cstring>
#include <filesystem>
#include "version.h"
#include <dwmapi.h>
#include <atomic>
#include <cmath>
#include <thread>

namespace bs::lab {
namespace {
// Fixed work: a dependent xorshift chain, so the same units take longer at a lower clock.
uint64_t spin(uint64_t units) {
    uint64_t x = 0x9E3779B97F4A7C15ull;
    for (uint64_t i = 0; i < units; ++i) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; }
    return x;
}
constexpr uint64_t FrameWork = 1500000;   // ~2-4 ms per scrolling frame
constexpr uint64_t BurstWork = 4000000;   // ~6-10 ms per burst
constexpr uint64_t SustainedChunk = 1000000;
constexpr wchar_t WindowClass[] = L"BatteryScope.LabTest.v1";
double seconds(LARGE_INTEGER a, LARGE_INTEGER b, LARGE_INTEGER frequency) { return double(b.QuadPart - a.QuadPart) / double(frequency.QuadPart); }
std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    std::string out(size_t(WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0, nullptr, nullptr)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), out.data(), int(out.size()), nullptr, nullptr);
    return out;
}
std::string jsonString(const std::string& text) {
    std::string out = "\"";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c < 0x20) { char buffer[8]; snprintf(buffer, sizeof(buffer), "\\u%04x", c); out += buffer; }
        else out += char(c);
    }
    return out + "\"";
}
// A number field from the page's small JSON report; 0 when absent.
double jsonNumber(const std::string& json, const char* key) {
    const std::string token = std::string("\"") + key + "\":";
    const size_t at = json.find(token);
    if (at == std::string::npos) return 0;
    const double value = std::strtod(json.c_str() + at + token.size(), nullptr);
    return std::isfinite(value) && value >= 0 && value < 1e9 ? value : 0;
}
std::wstring appPath(HKEY root, const wchar_t* exe) {
    const std::wstring key = std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\") + exe;
    wchar_t path[MAX_PATH * 2]{}; DWORD size = sizeof(path);
    if (RegGetValueW(root, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, path, &size) != ERROR_SUCCESS) return {};
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES ? path : std::wstring();
}
std::string testPage() {
    HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(103), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL loaded = resource ? LoadResource(nullptr, resource) : nullptr;
    const void* data = loaded ? LockResource(loaded) : nullptr;
    return data ? std::string(static_cast<const char*>(data), SizeofResource(nullptr, resource)) : std::string("<!doctype html><p>missing page</p>");
}
}

struct WorkloadEngine::Impl {
    Controller* controller = nullptr;
    std::atomic<bool> running{false}, abort{false};
    std::atomic<uint64_t> sink{0};
    std::mutex lock;
    WorkloadMetrics metrics;
    std::vector<std::thread> threads;
    HWND window = nullptr;
    // Browser mode: a page served from 127.0.0.1 in a separate browser, all of it in one job.
    std::wstring browser;
    std::atomic<int> port{0};
    HANDLE job = nullptr;
    std::string pageHtml;
    void add(const WorkloadMetrics& m) {
        std::lock_guard<std::mutex> guard(lock);
        metrics.frames += m.frames; metrics.lateFrames += m.lateFrames;
        metrics.bursts += m.bursts; metrics.burstMs += m.burstMs; metrics.work += m.work;
    }
    Workload current() const { return controller ? controller->workload() : Workload::none; }
    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_KEYDOWN && wp == VK_ESCAPE && self) { self->abort = true; return 0; }
        if (message == WM_CLOSE) { if (self) self->abort = true; return 0; }
        if (message == WM_SETCURSOR) { SetCursor(LoadCursorW(nullptr, IDC_ARROW)); return TRUE; }
        return DefWindowProcW(window, message, wp, lp);
    }
    // A mock article: text lines and image blocks on a dark page, three screens tall.
    HBITMAP page(HDC screen, int width, int height) {
        HDC dc = CreateCompatibleDC(screen);
        HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height * 3);
        HGDIOBJ old = SelectObject(dc, bitmap);
        RECT all{0, 0, width, height * 3};
        HBRUSH background = CreateSolidBrush(RGB(24, 26, 31)), text = CreateSolidBrush(RGB(150, 156, 166)),
            heading = CreateSolidBrush(RGB(220, 224, 230)), image = CreateSolidBrush(RGB(52, 92, 120));
        FillRect(dc, &all, background);
        uint32_t seed = 12345;
        auto next = [&] { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
        const int margin = width / 6, column = width - 2 * margin;
        for (int y = 40; y < height * 3 - 40;) {
            const int kind = int(next() % 10);
            if (kind == 0) { RECT r{margin, y, margin + column, y + height / 4}; FillRect(dc, &r, image); y += height / 4 + 30; continue; }
            if (kind == 1) { RECT r{margin, y, margin + column * 2 / 3, y + 26}; FillRect(dc, &r, heading); y += 50; continue; }
            for (int line = 0, lines = 3 + int(next() % 5); line < lines; ++line) {
                const int length = line + 1 == lines ? column * int(30 + next() % 50) / 100 : column * int(88 + next() % 12) / 100;
                RECT r{margin, y, margin + length, y + 10}; FillRect(dc, &r, text); y += 22;
            }
            y += 24;
        }
        DeleteObject(background); DeleteObject(text); DeleteObject(heading); DeleteObject(image);
        SelectObject(dc, old); DeleteDC(dc);
        return bitmap;
    }
    std::wstring caption() const {
        if (!controller) return {};
        const auto st = controller->status();
        std::wstring text = std::wstring(tr(L"BatteryScope 自動測試")) + L"  ·  ";
        if (!st.running) return text + tr(L"已結束，正在匯出結果…");
        if (st.paused) return text + tr(L"請拔掉電源：只在用電池時測試（已暫停）  ·  Esc 中止");
        text += std::to_wstring(st.step + 1) + L"/" + std::to_wstring(st.steps) + L"  " + tr(workloadName(st.workload));
        if (st.arm >= 0) text += L"  ·  " + std::wstring(tr(st.armNames[size_t(st.arm)].c_str()));
        text += tr(L"  ·  剩約 ") + std::to_wstring(int(std::ceil(st.remainingSeconds / 60))) + tr(L" 分  ·  請勿操作，Esc 中止");
        return text;
    }
    void windowThread() {
        SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED); // no sleep or screen off mid-test
        WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = proc; wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = WindowClass; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        const int width = GetSystemMetrics(SM_CXSCREEN), height = GetSystemMetrics(SM_CYSCREEN);
        window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WindowClass, L"BatteryScope", WS_POPUP | WS_VISIBLE,
            0, 0, width, height, nullptr, nullptr, wc.hInstance, nullptr);
        if (!window) { running = false; return; }
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        SetForegroundWindow(window);
        HDC screen = GetDC(window);
        HBITMAP content = page(screen, width, height);
        HDC source = CreateCompatibleDC(screen);
        HGDIOBJ oldBitmap = SelectObject(source, content);
        HFONT font = CreateFontW(-std::max(16, height / 50), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft JhengHei UI");
        LARGE_INTEGER frequency{}, start{}, last{}, now{};
        QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&start); last = start;
        std::wstring text; uint64_t captionAt = 0, refreshAt = 0; double period = 1.0 / 60;
        const int band = std::max(40, height / 22);
        while (running) {
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
            const uint64_t tick = GetTickCount64();
            if (tick - refreshAt >= 1000) {
                DEVMODEW mode{}; mode.dmSize = sizeof(mode);
                if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1) period = 1.0 / mode.dmDisplayFrequency;
                refreshAt = tick;
            }
            if (tick - captionAt >= 1000) { text = caption(); captionAt = tick; }
            const bool scrolling = current() == Workload::scroll;
            QueryPerformanceCounter(&now);
            int offset = 0;
            if (scrolling) {
                sink += spin(FrameWork); // the page's layout and script work for this frame
                offset = int(std::fmod(seconds(start, now, frequency) * 600.0, double(height) * 2));
            }
            BitBlt(screen, 0, band, width, height - band, source, 0, offset, SRCCOPY);
            RECT bar{0, 0, width, band};
            HBRUSH barBrush = CreateSolidBrush(RGB(36, 40, 48)); FillRect(screen, &bar, barBrush); DeleteObject(barBrush);
            HGDIOBJ oldFont = SelectObject(screen, font);
            SetBkMode(screen, TRANSPARENT); SetTextColor(screen, RGB(232, 236, 240));
            RECT textRect{band / 2, 0, width - band / 2, band};
            DrawTextW(screen, text.c_str(), -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(screen, oldFont);
            if (scrolling) {
                DwmFlush(); // wait for the compositor: one frame per display refresh
                QueryPerformanceCounter(&now);
                const double interval = seconds(last, now, frequency);
                WorkloadMetrics m; m.frames = 1; m.lateFrames = interval > period * 1.5 ? 1 : 0;
                add(m);
            } else MsgWaitForMultipleObjects(0, nullptr, FALSE, 250, QS_ALLINPUT);
            last = now;
        }
        SelectObject(source, oldBitmap); DeleteDC(source); DeleteObject(content); DeleteObject(font);
        ReleaseDC(window, screen);
        DestroyWindow(window); window = nullptr;
        UnregisterClassW(WindowClass, GetModuleHandleW(nullptr));
        SetThreadExecutionState(ES_CONTINUOUS);
    }
    void respond(SOCKET client, const char* type, const std::string& body) {
        const std::string head = std::string("HTTP/1.1 200 OK\r\nContent-Type: ") + type + "\r\nCache-Control: no-store\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
        send(client, head.data(), int(head.size()), 0);
        if (!body.empty()) send(client, body.data(), int(body.size()), 0);
    }
    void handle(SOCKET client) {
        DWORD timeout = 2000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        std::string request; char buffer[4096];
        size_t headerEnd = std::string::npos;
        while (request.size() < 65536 && (headerEnd = request.find("\r\n\r\n")) == std::string::npos) {
            const int got = recv(client, buffer, sizeof(buffer), 0);
            if (got <= 0) return;
            request.append(buffer, size_t(got));
        }
        if (headerEnd == std::string::npos) return;
        size_t length = 0;
        for (const char* name : {"Content-Length:", "content-length:"})
            if (size_t at = request.find(name); at != std::string::npos && at < headerEnd) length = size_t(std::strtoul(request.c_str() + at + strlen(name), nullptr, 10));
        while (length < 65536 && request.size() < headerEnd + 4 + length) {
            const int got = recv(client, buffer, sizeof(buffer), 0);
            if (got <= 0) break;
            request.append(buffer, size_t(got));
        }
        const std::string body = request.substr(headerEnd + 4);
        const bool post = request.compare(0, 5, "POST ") == 0;
        const size_t pathStart = request.find(' ') + 1, pathEnd = request.find(' ', pathStart);
        const std::string path = request.substr(pathStart, pathEnd - pathStart);
        if (!post && (path == "/" || path == "/run")) respond(client, "text/html; charset=utf-8", pageHtml);
        else if (!post && path == "/state") {
            DEVMODEW mode{}; mode.dmSize = sizeof(mode);
            const int hz = EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) ? int(mode.dmDisplayFrequency) : 60;
            // The Windows app theme (the dark-mode step switches it), for the page to follow.
            DWORD light = 1, size = sizeof(light);
            RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
            const std::string json = "{\"scenario\":\"" + utf8(workloadKey(current())) + (current() == Workload::none ? "none" : "") +
                "\",\"refresh\":" + std::to_string(hz) + ",\"dark\":" + (light ? "0" : "1") + ",\"caption\":" + jsonString(utf8(caption())) + "}";
            respond(client, "application/json", json);
        } else if (post && path == "/m") {
            WorkloadMetrics m;
            m.frames = jsonNumber(body, "frames") + jsonNumber(body, "videoTotal");
            // The browser's dropped-frame count for the recorded test clip is not used: its frame
            // times are irregular (recorded on a timer), so at 60 Hz frames land in the same refresh
            // and count as dropped although a real 30 fps video would play cleanly.
            m.lateFrames = jsonNumber(body, "late");
            m.bursts = jsonNumber(body, "events"); m.burstMs = jsonNumber(body, "eventMs");
            add(m);
            respond(client, "text/plain", "ok");
        } else if (post && path == "/abort") { abort = true; respond(client, "text/plain", "ok"); }
        else {
            const std::string missing = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send(client, missing.data(), int(missing.size()), 0);
        }
    }
    void serverThread() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) { port = -1; return; }
        SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = 0;
        int size = sizeof(address);
        if (listener == INVALID_SOCKET || bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(listener, 16) != 0 || getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
            if (listener != INVALID_SOCKET) closesocket(listener);
            WSACleanup(); port = -1; return;
        }
        port = ntohs(address.sin_port);
        while (running) {
            fd_set readable; FD_ZERO(&readable); FD_SET(listener, &readable);
            timeval wait{0, 200000};
            if (select(0, &readable, nullptr, nullptr, &wait) > 0) {
                SOCKET client = accept(listener, nullptr, nullptr);
                if (client != INVALID_SOCKET) { handle(client); shutdown(client, SD_BOTH); closesocket(client); }
            }
            // The user closed the browser (Alt+F4): treat it like Esc.
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
            if (job && QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr) &&
                accounting.TotalProcesses > 0 && accounting.ActiveProcesses == 0) abort = true;
        }
        closesocket(listener);
        WSACleanup();
    }
    // Starts the browser in kiosk mode with its own profile, inside a job so every
    // process it starts ends with the test.
    bool launchBrowser() {
        PWSTR base = nullptr;
        constexpr GUID LocalAppData = {0xF1B32785, 0x6FBA, 0x4FCF, {0x9D, 0x55, 0x7B, 0x8E, 0x7F, 0x15, 0x70, 0x91}};
        std::wstring profile;
        if (SUCCEEDED(SHGetKnownFolderPath(LocalAppData, 0, nullptr, &base)) && base) profile = std::wstring(base) + L"\\BatteryScope\\browser-test";
        CoTaskMemFree(base);
        const std::wstring url = L"http://127.0.0.1:" + std::to_wstring(port.load()) + L"/run";
        std::wstring command = L"\"" + browser + L"\" --kiosk " + url + L" --edge-kiosk-type=fullscreen --start-fullscreen --no-first-run"
            L" --no-default-browser-check --disable-sync --autoplay-policy=no-user-gesture-required --disable-features=Translate";
        if (!profile.empty()) command += L" \"--user-data-dir=" + profile + L"\"";
        job = CreateJobObjectW(nullptr, nullptr);
        if (job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        }
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &startup, &process)) return false;
        if (job) AssignProcessToJobObject(job, process.hProcess);
        setProtectedJob(job); // the test browser is never throttled as a background app
        ResumeThread(process.hThread);
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
        return true;
    }
    void burstThread() {
        LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
        while (running) {
            if (current() != Workload::bursts) { Sleep(100); continue; }
            LARGE_INTEGER a{}, b{};
            QueryPerformanceCounter(&a); sink += spin(BurstWork); QueryPerformanceCounter(&b);
            const double ms = seconds(a, b, frequency) * 1000;
            WorkloadMetrics m; m.bursts = 1; m.burstMs = ms; add(m);
            Sleep(DWORD(std::max(1.0, 250 - ms))); // one burst every 250 ms, like typing or clicking
        }
    }
    void sustainedThread() {
        while (running) {
            if (current() != Workload::sustained) { Sleep(100); continue; }
            sink += spin(SustainedChunk);
            WorkloadMetrics m; m.work = double(SustainedChunk) / 1e6; add(m);
        }
    }
};

WorkloadEngine::WorkloadEngine() : impl_(std::make_unique<Impl>()) {}
WorkloadEngine::~WorkloadEngine() { stop(); }
std::wstring WorkloadEngine::browserPath() {
    for (const wchar_t* exe : {L"msedge.exe", L"chrome.exe"})
        for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER})
            if (auto path = appPath(root, exe); !path.empty()) return path;
    return {};
}
void WorkloadEngine::start(Controller* controller) {
    stop();
    auto& d = *impl_;
    d.controller = controller; d.abort = false; d.running = true; d.port = 0;
    { std::lock_guard<std::mutex> guard(d.lock); d.metrics = {}; }
    d.browser = browserPath();
    bool browserStarted = false;
    if (!d.browser.empty()) {
        d.pageHtml = testPage();
        d.threads.emplace_back([this] { impl_->serverThread(); });
        for (int i = 0; i < 100 && d.port == 0; ++i) Sleep(20);
        browserStarted = d.port > 0 && d.launchBrowser();
    }
    if (!browserStarted) { // built-in loads in our own window
        d.threads.emplace_back([this] { impl_->windowThread(); });
        d.threads.emplace_back([this] { impl_->burstThread(); });
    } else SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
    for (int i = 0; i < 2; ++i) d.threads.emplace_back([this] { impl_->sustainedThread(); });
}
void WorkloadEngine::stop() {
    auto& d = *impl_;
    d.running = false;
    if (d.job) { setProtectedJob(nullptr); TerminateJobObject(d.job, 0); CloseHandle(d.job); d.job = nullptr; }
    for (auto& thread : d.threads) if (thread.joinable()) thread.join();
    d.threads.clear();
    SetThreadExecutionState(ES_CONTINUOUS);
}
int WorkloadEngine::port() const { return impl_->port > 0 ? impl_->port.load() : 0; }
bool WorkloadEngine::running() const { return impl_->running; }
bool WorkloadEngine::abortRequested() const { return impl_->abort; }
WorkloadMetrics WorkloadEngine::take() {
    std::lock_guard<std::mutex> guard(impl_->lock);
    WorkloadMetrics taken = impl_->metrics;
    impl_->metrics = {};
    return taken;
}
}
