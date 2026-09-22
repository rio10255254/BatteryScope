#include "platform.h"
#include <winternl.h>
#include <initguid.h>
#include <devguid.h>
#include <poclass.h>
#include <setupapi.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <set>
#include <cwctype>
#include <cstring>
#include <limits>

namespace bs {
uint64_t fileTime(FILETIME t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; }
std::wstring errorText(DWORD code) {
    wchar_t* buffer = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring s = buffer ? buffer : L"未知錯誤";
    if (buffer) LocalFree(buffer);
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    return s + L" (" + std::to_wstring(code) + L")";
}
std::wstring executablePath() {
    std::wstring s(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, s.data(), DWORD(s.size()));
    s.resize(n); return s;
}
PowerSample readPower() {
    PowerSample p;
    p.ms = GetTickCount64();
    SYSTEM_POWER_STATUS system{};
    if (GetSystemPowerStatus(&system)) {
        p.acKnown = system.ACLineStatus != 255;
        p.onAc = system.ACLineStatus == 1;
        p.present = system.BatteryFlag != 255 && !(system.BatteryFlag & 128);
        p.charging = system.BatteryFlag != 255 && (system.BatteryFlag & 8);
        if (p.present && system.BatteryLifePercent <= 100) p.percent = system.BatteryLifePercent;
    }
    HDEVINFO devices = SetupDiGetClassDevsW(&GUID_DEVCLASS_BATTERY, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) return p;
    double remaining = 0, full = 0, watts = 0;
    unsigned count = 0;
    bool capacityOK = true, rateOK = true;
    for (DWORD i = 0;; ++i) {
        SP_DEVICE_INTERFACE_DATA iface{}; iface.cbSize = sizeof(iface);
        if (!SetupDiEnumDeviceInterfaces(devices, nullptr, &GUID_DEVCLASS_BATTERY, i, &iface)) break;
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(devices, &iface, nullptr, 0, &needed, nullptr);
        if (!needed) { capacityOK = rateOK = false; continue; }
        std::vector<BYTE> bytes(needed);
        auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(bytes.data());
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailW(devices, &iface, detail, needed, nullptr, nullptr)) {
            capacityOK = rateOK = false; continue;
        }
        Handle battery(CreateFileW(detail->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!battery) { capacityOK = rateOK = false; continue; }
        ULONG timeout = 0, tag = 0;
        DWORD returned = 0;
        if (!DeviceIoControl(battery, IOCTL_BATTERY_QUERY_TAG, &timeout, sizeof(timeout), &tag, sizeof(tag), &returned, nullptr) || !tag) {
            capacityOK = rateOK = false; continue;
        }
        BATTERY_QUERY_INFORMATION query{}; query.BatteryTag = tag; query.InformationLevel = BatteryInformation;
        BATTERY_INFORMATION info{};
        if (!DeviceIoControl(battery, IOCTL_BATTERY_QUERY_INFORMATION, &query, sizeof(query), &info, sizeof(info), &returned, nullptr)) {
            capacityOK = rateOK = false; continue;
        }
        if (!(info.Capabilities & BATTERY_SYSTEM_BATTERY)) continue;
        ++count;
        BATTERY_WAIT_STATUS wait{}; wait.BatteryTag = tag;
        BATTERY_STATUS status{};
        if (!DeviceIoControl(battery, IOCTL_BATTERY_QUERY_STATUS, &wait, sizeof(wait), &status, sizeof(status), &returned, nullptr)) {
            capacityOK = rateOK = false; continue;
        }
        const bool relative = info.Capabilities & BATTERY_CAPACITY_RELATIVE;
        if (relative || status.Capacity == BATTERY_UNKNOWN_CAPACITY || info.FullChargedCapacity == BATTERY_UNKNOWN_CAPACITY || !info.FullChargedCapacity)
            capacityOK = false;
        else { remaining += status.Capacity / 1000.0; full += info.FullChargedCapacity / 1000.0; }
        if (relative || ULONG(status.Rate) == BATTERY_UNKNOWN_RATE) rateOK = false;
        else watts += -double(status.Rate) / 1000.0;
        if (status.PowerState & BATTERY_CHARGING) p.charging = true;
    }
    SetupDiDestroyDeviceInfoList(devices);
    if (count) {
        p.present = true;
        if (capacityOK && full > 0) {
            p.remainingWh = remaining; p.fullWh = full;
            p.percent = std::clamp(remaining / full * 100, 0.0, 100.0);
        }
        if (rateOK) p.watts = watts;
    }
    return p;
}

struct ScHandle {
    SC_HANDLE value = nullptr;
    explicit ScHandle(SC_HANDLE h) : value(h) {}
    ~ScHandle() { if (value) CloseServiceHandle(value); }
    operator SC_HANDLE() const { return value; }
};
std::vector<ServiceInfo> readServices(bool& ok) {
    ok = false;
    std::vector<ServiceInfo> result;
    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE));
    if (!scm.value) return result;
    DWORD resume = 0;
    std::vector<BYTE> buffer(256 * 1024);
    for (;;) {
        DWORD needed = 0, count = 0;
        BOOL success = EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_ACTIVE,
            buffer.data(), DWORD(buffer.size()), &needed, &count, &resume, nullptr);
        DWORD error = success ? ERROR_SUCCESS : GetLastError();
        auto rows = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());
        for (DWORD i = 0; i < count; ++i) {
            const auto& r = rows[i];
            result.push_back({r.lpServiceName, r.lpDisplayName, r.ServiceStatusProcess.dwProcessId,
                bool(r.ServiceStatusProcess.dwControlsAccepted & SERVICE_ACCEPT_STOP),
                bool(r.ServiceStatusProcess.dwServiceType & SERVICE_WIN32_SHARE_PROCESS)});
        }
        if (success) { ok = true; break; }
        if (error != ERROR_MORE_DATA || count == 0) break;
    }
    return result;
}
static std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return wchar_t(towlower(c)); }); return s;
}
static bool sameUser(HANDLE process) {
    Handle ours, theirs;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &ours.value) || !OpenProcessToken(process, TOKEN_QUERY, &theirs.value)) return false;
    DWORD a = 0, b = 0;
    GetTokenInformation(ours, TokenUser, nullptr, 0, &a);
    GetTokenInformation(theirs, TokenUser, nullptr, 0, &b);
    if (!a || !b) return false;
    std::vector<BYTE> x(a), y(b);
    return GetTokenInformation(ours, TokenUser, x.data(), a, &a) && GetTokenInformation(theirs, TokenUser, y.data(), b, &b)
        && EqualSid(reinterpret_cast<TOKEN_USER*>(x.data())->User.Sid, reinterpret_cast<TOKEN_USER*>(y.data())->User.Sid);
}
bool safeProcess(HANDLE process, DWORD pid, std::wstring* reason) {
    auto deny = [&](const wchar_t* s) { if (reason) *reason = s; return false; };
    if (pid <= 4 || pid == GetCurrentProcessId()) return deny(L"系統程序或監測工具本身");
    DWORD session = 0, ourSession = 0;
    if (!ProcessIdToSessionId(pid, &session) || !ProcessIdToSessionId(GetCurrentProcessId(), &ourSession) || !session || session != ourSession)
        return deny(L"系統服務或其他使用者的程序");
    BOOL critical = TRUE;
    if (!IsProcessCritical(process, &critical) || critical) return deny(L"關鍵程序或無法確認安全狀態");
    if (!sameUser(process)) return deny(L"無法確認程序擁有者");
    wchar_t path[32768]; DWORD length = DWORD(std::size(path));
    if (!QueryFullProcessImageNameW(process, 0, path, &length)) return deny(L"無法確認執行檔");
    std::wstring name = lower(path);
    name = name.substr(name.find_last_of(L"\\/") + 1);
    static const std::set<std::wstring> protectedNames = {
        L"explorer.exe", L"dwm.exe", L"winlogon.exe", L"csrss.exe", L"lsass.exe", L"smss.exe",
        L"services.exe", L"svchost.exe", L"sihost.exe", L"fontdrvhost.exe", L"wininit.exe", L"secure system"
    };
    if (protectedNames.count(name)) return deny(L"受保護的 Windows 程序");
    return true;
}
static bool matchesCreation(HANDLE process, uint64_t expected) {
    FILETIME c{}, e{}, k{}, u{};
    return expected && GetProcessTimes(process, &c, &e, &k, &u) && fileTime(c) == expected;
}
struct CloseRequest { DWORD pid; unsigned count = 0; };
static BOOL CALLBACK requestClose(HWND window, LPARAM parameter) {
    auto& request = *reinterpret_cast<CloseRequest*>(parameter);
    DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
    if (pid == request.pid && IsWindowVisible(window) && !GetWindow(window, GW_OWNER)) {
        if (PostMessageW(window, WM_CLOSE, 0, 0)) ++request.count;
    }
    return TRUE;
}
DWORD closeProcess(const ProcessRow& expected, bool force) {
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | (force ? PROCESS_TERMINATE : 0), FALSE, expected.pid));
    if (!process) return GetLastError();
    if (!matchesCreation(process, expected.created)) return ERROR_NOT_FOUND;
    if (WaitForSingleObject(process, 0) != WAIT_TIMEOUT) return ERROR_NOT_FOUND;
    if (!safeProcess(process, expected.pid)) return ERROR_ACCESS_DENIED;
    bool servicesOK = false;
    auto services = readServices(servicesOK);
    if (!servicesOK) return ERROR_ACCESS_DENIED;
    for (const auto& service : services) if (service.pid == expected.pid) return ERROR_ACCESS_DENIED;
    if (force) {
        if (!TerminateProcess(process, 1)) return GetLastError();
        return WaitForSingleObject(process, 5000) == WAIT_OBJECT_0 ? ERROR_SUCCESS : ERROR_TIMEOUT;
    }
    CloseRequest request{expected.pid};
    EnumWindows(requestClose, reinterpret_cast<LPARAM>(&request));
    return request.count ? ERROR_SUCCESS : ERROR_NOT_SUPPORTED;
}
static bool protectedService(const std::wstring& name) {
    static const std::set<std::wstring> deny = {
        L"rpcss", L"rpceptmapper", L"dcomlaunch", L"eventlog", L"plugplay", L"power", L"profsvc", L"samss",
        L"lsm", L"schedule", L"winmgmt", L"usermanager", L"bfe", L"mpssvc", L"windefend", L"securityhealthservice",
        L"wscsvc", L"sense", L"cryptsvc", L"keyiso", L"gpsvc", L"brokerinfrastructure", L"nsi", L"dhcp", L"dnscache"
    };
    return deny.count(lower(name)) != 0;
}
DWORD stopService(const std::wstring& name, DWORD expectedPid, uint64_t expectedCreated) {
    if (name.empty() || !expectedPid || !expectedCreated) return ERROR_INVALID_PARAMETER;
    if (protectedService(name)) return ERROR_ACCESS_DISABLED_BY_POLICY;
    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm.value) return GetLastError();
    ScHandle service(OpenServiceW(scm, name.c_str(), SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_ENUMERATE_DEPENDENTS));
    if (!service.value) return GetLastError();
    SERVICE_STATUS_PROCESS status{}; DWORD returned = 0;
    if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof(status), &returned)) return GetLastError();
    if (status.dwCurrentState == SERVICE_STOPPED) return ERROR_SUCCESS;
    if (status.dwProcessId != expectedPid || !(status.dwServiceType & SERVICE_WIN32) || !(status.dwControlsAccepted & SERVICE_ACCEPT_STOP)) return ERROR_SERVICE_CANNOT_ACCEPT_CTRL;
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, expectedPid));
    if (!process || !matchesCreation(process, expectedCreated)) return ERROR_NOT_FOUND;
    BOOL critical = TRUE;
    if (!IsProcessCritical(process, &critical) || critical) return ERROR_ACCESS_DENIED;
    DWORD needed = 0, dependentCount = 0;
    BOOL noDependents = EnumDependentServicesW(service, SERVICE_ACTIVE, nullptr, 0, &needed, &dependentCount);
    if (!noDependents) {
        DWORD error = GetLastError();
        return error == ERROR_MORE_DATA ? ERROR_DEPENDENT_SERVICES_RUNNING : error;
    }
    if (dependentCount) return ERROR_DEPENDENT_SERVICES_RUNNING;
    SERVICE_STATUS basic{};
    if (!ControlService(service, SERVICE_CONTROL_STOP, &basic)) return GetLastError();
    const auto deadline = GetTickCount64() + 15000;
    while (GetTickCount64() < deadline) {
        if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof(status), &returned)) return GetLastError();
        if (status.dwCurrentState == SERVICE_STOPPED) return ERROR_SUCCESS;
        Sleep(250);
    }
    return ERROR_TIMEOUT;
}

static BOOL CALLBACK visibleWindow(HWND window, LPARAM parameter) {
    if (IsWindowVisible(window) && !GetWindow(window, GW_OWNER) && GetWindowTextLengthW(window) > 0) {
        DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
        reinterpret_cast<std::set<DWORD>*>(parameter)->insert(pid);
    }
    return TRUE;
}
bool setSamplerEcoQoS(bool enabled) {
    THREAD_POWER_THROTTLING_STATE state{};
    state.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = enabled ? THREAD_POWER_THROTTLING_EXECUTION_SPEED : 0;
    state.StateMask = state.ControlMask;
    return SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &state, sizeof(state)) != FALSE;
}

namespace {
struct RawProcess {
    DWORD pid = 0;
    uint64_t created = 0, cpu = 0, io = 0;
    std::wstring name;
    double memoryMB = 0;
    bool ioKnown = false, memoryKnown = false;
};

bool systemProcessSnapshot(std::vector<RawProcess>& result, uint64_t& sampleMs, double& durationMs) {
    using Query = NTSTATUS (NTAPI*)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);
    static Query query = [] {
        FARPROC symbol = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation");
        Query function = nullptr;
        static_assert(sizeof(function) == sizeof(symbol));
        std::memcpy(&function, &symbol, sizeof(function));
        return function;
    }();
    if (!query) return false;
    std::vector<BYTE> bytes(1024 * 1024);
    ULONG returned = 0;
    NTSTATUS status = 0;
    LARGE_INTEGER counterBegin{}, counterEnd{}, frequency{};
    QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&counterBegin);
    const uint64_t begin = GetTickCount64();
    for (unsigned attempt = 0; attempt < 5; ++attempt) {
        status = query(SystemProcessInformation, bytes.data(), ULONG(bytes.size()), &returned);
        if (status >= 0) break;
        constexpr NTSTATUS lengthMismatch = NTSTATUS(0xC0000004L);
        if (status != lengthMismatch || bytes.size() >= 64 * 1024 * 1024) return false;
        const size_t wanted = std::max(bytes.size() * 2, size_t(returned) + 64 * 1024);
        bytes.resize(std::min<size_t>(wanted, 64 * 1024 * 1024));
    }
    const uint64_t end = GetTickCount64();
    QueryPerformanceCounter(&counterEnd);
    if (status < 0) return false;
    sampleMs = begin + (end - begin) / 2;
    durationMs = frequency.QuadPart > 0 ? (counterEnd.QuadPart - counterBegin.QuadPart) * 1000.0 / frequency.QuadPart : double(end - begin);
    const uintptr_t first = reinterpret_cast<uintptr_t>(bytes.data());
    const uintptr_t last = first + bytes.size();
    size_t offset = 0;
    for (;;) {
        if (offset + sizeof(SYSTEM_PROCESS_INFORMATION) > bytes.size()) return false;
        auto process = reinterpret_cast<const SYSTEM_PROCESS_INFORMATION*>(bytes.data() + offset);
        const ULONG_PTR rawPid = reinterpret_cast<ULONG_PTR>(process->UniqueProcessId);
        if (rawPid && rawPid <= std::numeric_limits<DWORD>::max()) {
            RawProcess row;
            row.pid = DWORD(rawPid);
            row.created = uint64_t(process->CreateTime.QuadPart);
            row.cpu = uint64_t(process->KernelTime.QuadPart) + uint64_t(process->UserTime.QuadPart);
            row.io = process->IoCounters.ReadTransferCount + process->IoCounters.WriteTransferCount;
            row.ioKnown = true;
            row.memoryKnown = true;
            row.memoryMB = process->VirtualMemoryCounters.WorkingSetSize / 1048576.0;
            if (process->ImageName.Buffer && process->ImageName.Length && !(process->ImageName.Length % sizeof(wchar_t))) {
                const uintptr_t name = reinterpret_cast<uintptr_t>(process->ImageName.Buffer);
                if (name >= first && name + process->ImageName.Length >= name && name + process->ImageName.Length <= last)
                    row.name.assign(process->ImageName.Buffer, process->ImageName.Length / sizeof(wchar_t));
            }
            if (row.name.empty()) row.name = row.pid == 4 ? L"System" : L"PID " + std::to_wstring(row.pid);
            result.push_back(std::move(row));
        }
        if (!process->NextEntryOffset) break;
        if (process->NextEntryOffset < sizeof(SYSTEM_PROCESS_INFORMATION)
            || offset + process->NextEntryOffset <= offset || offset + process->NextEntryOffset >= bytes.size()) return false;
        offset += process->NextEntryOffset;
    }
    return !result.empty();
}

bool toolhelpProcessSnapshot(std::vector<RawProcess>& result, size_t& total, size_t& inaccessible,
    uint64_t& sampleMs, double& durationMs, CollectionWork& work) {
    const uint64_t begin = GetTickCount64();
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    if (!snapshot || !Process32FirstW(snapshot, &entry)) return false;
    do {
        ++total;
        if (!entry.th32ProcessID) continue;
        ++work.processOpens;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
        FILETIME c{}, e{}, k{}, u{};
        if (!process || !GetProcessTimes(process, &c, &e, &k, &u)) { ++inaccessible; continue; }
        RawProcess row;
        row.pid = entry.th32ProcessID; row.name = entry.szExeFile; row.created = fileTime(c);
        row.cpu = fileTime(k) + fileTime(u);
        IO_COUNTERS io{};
        row.ioKnown = GetProcessIoCounters(process, &io) != FALSE;
        row.io = io.ReadTransferCount + io.WriteTransferCount;
        PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory); ++work.memory;
        row.memoryKnown = GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
        row.memoryMB = memory.WorkingSetSize / 1048576.0;
        result.push_back(std::move(row));
    } while (Process32NextW(snapshot, &entry));
    const uint64_t end = GetTickCount64();
    sampleMs = begin + (end - begin) / 2;
    durationMs = double(end - begin);
    return !result.empty();
}
}

std::vector<ProcessRow> Collector::collect(size_t& total, size_t& inaccessible, bool background) {
    (void)background;
    work_ = {};
    total = inaccessible = 0;
    std::vector<RawProcess> raw;
    systemSnapshot_ = systemProcessSnapshot(raw, sampleMs_, snapshotDurationMs_);
    if (systemSnapshot_) total = raw.size();
    else if (!toolhelpProcessSnapshot(raw, total, inaccessible, sampleMs_, snapshotDurationMs_, work_)) return {};
    intervalSeconds_ = previousSampleMs_ && sampleMs_ > previousSampleMs_ && sampleMs_ - previousSampleMs_ <= 30000
        ? (sampleMs_ - previousSampleMs_) / 1000.0 : 0;
    previousSampleMs_ = sampleMs_;
    const uint64_t now = GetTickCount64();
    if (!servicesAt_ || now - servicesAt_ >= 10000) {
        services_ = readServices(servicesKnown_); servicesAt_ = now;
    }
    std::map<DWORD, std::vector<ServiceInfo>> serviceMap;
    for (const auto& s : services_) if (s.pid) serviceMap[s.pid].push_back(s);
    std::set<DWORD> visible;
    EnumWindows(visibleWindow, reinterpret_cast<LPARAM>(&visible));
    std::vector<ProcessRow> rows;
    std::map<DWORD, Previous> next;
    std::map<DWORD, Metadata> nextMetadata;
    const unsigned processors = std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    for (const auto& source : raw) {
        ProcessRow row; row.pid = source.pid; row.name = source.name;
        row.services = serviceMap[row.pid]; row.window = visible.count(row.pid) != 0;
        row.kind = row.pid <= 4 ? L"Windows 核心" : !row.services.empty() ? L"服務" : row.window ? L"應用程式" : L"背景程序";
        row.created = source.created;
        const uint64_t cpu = source.cpu, ioBytes = source.io;
        const bool rawIoKnown = source.ioKnown;
        row.ioKnown = rawIoKnown;
        Metadata meta;
        auto cached = metadata_.find(row.pid);
        if (cacheEnabled_ && cached != metadata_.end() && cached->second.created == row.created) meta = cached->second;
        meta.created = row.created;
        meta.memoryKnown = source.memoryKnown; meta.memoryMB = source.memoryMB; meta.memoryAt = sampleMs_;
        const bool protectedIdentity = row.pid <= 4 || row.created == 0;
        const bool needPath = !protectedIdentity && (!meta.pathAt || (meta.path.empty() && now - meta.pathAt >= 30000));
        const bool needSafety = !protectedIdentity && row.services.empty() && servicesKnown_
            && (!meta.safetyAt || now - meta.safetyAt >= 30000);
        if (needPath || needSafety) {
            ++work_.processOpens;
            Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, row.pid));
            meta.readable = bool(process);
            if (needPath) {
                wchar_t path[32768]; DWORD length = DWORD(std::size(path)); ++work_.paths;
                if (process && QueryFullProcessImageNameW(process, 0, path, &length)) meta.path.assign(path, length);
                meta.pathAt = now;
            }
            // Only a UI hint is cached. closeProcess revalidates identity, safety and services live.
            if (needSafety) {
                ++work_.safety; meta.safe = process && safeProcess(process, row.pid); meta.safetyAt = now;
            }
        }
        row.path = meta.path; row.memoryKnown = meta.memoryKnown; row.memoryMB = meta.memoryMB;
        row.memoryAgeSeconds = (now - meta.memoryAt) / 1000.0;
        row.actionable = !protectedIdentity && row.services.empty() && servicesKnown_ && meta.safe;
        if (!meta.readable) ++inaccessible;
        nextMetadata[row.pid] = std::move(meta);
        auto old = previous_.find(row.pid);
        if (old != previous_.end() && old->second.created == row.created) {
            auto& p = old->second;
            const double dt = (now - p.ms) / 1000.0;
            if (dt > 0 && dt <= 30) {
                row.measured = true;
                row.cpu = cpuPercent(p.cpu, cpu, dt, processors);
                row.ioKnown = row.ioKnown && p.ioKnown && ioBytes >= p.io;
                if (row.ioKnown) row.ioMB = (ioBytes - p.io) / dt / 1048576.0;
                row.averageCpu = smooth(p.averageCpu, row.cpu, dt);
                row.averageIo = smooth(p.averageIo, row.ioMB, dt);
                row.busySeconds = row.cpu >= 1 || (row.ioKnown && row.ioMB >= 1) ? p.busySeconds + dt : 0;
                // An activity heuristic, NOT watts or a calibrated energy attribution.
                row.score = row.averageCpu + std::min(20.0, row.averageIo * 2);
            }
        }
        next[row.pid] = {row.created, cpu, ioBytes, now, row.averageCpu, row.averageIo, row.busySeconds, rawIoKnown};
        if (!row.measured) row.reason = L"採集中";
        else if (row.averageCpu < .1 && row.averageIo < .1) row.reason = L"活動低";
        else {
            row.reason = row.averageCpu >= row.averageIo * 2 ? L"CPU 活動" : L"I/O 活動";
            if (row.busySeconds >= 30) row.reason += L"・持續 " + std::to_wstring(int(row.busySeconds)) + L" 秒";
        }
        if (row.services.size() > 1) row.reason += L"・共用程序";
        rows.push_back(std::move(row));
    }
    previous_ = std::move(next);
    metadata_ = std::move(nextMetadata);
    std::sort(rows.begin(), rows.end(), [](const ProcessRow& a, const ProcessRow& b) {
        return a.score == b.score ? a.pid < b.pid : a.score > b.score;
    });
    return rows;
}
}
