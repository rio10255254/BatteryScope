#include "lab.h"
#include "platform.h"
#include <cfgmgr32.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <mmdeviceapi.h>
#include <setupapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <cstring>
#include <cwctype>
#include <functional>
#include <map>
#include <set>
#include <filesystem>

namespace bs::lab {
namespace {
// Processor power settings of the active plan (winnt.h GUIDs, defined here so
// no import library or initguid translation unit is needed).
constexpr GUID ProcessorGroup = {0x54533251, 0x82be, 0x4824, {0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00}};
constexpr GUID BoostMode = {0xbe337238, 0x0d82, 0x4146, {0xa9, 0x60, 0x4f, 0x37, 0x49, 0xd4, 0x70, 0xc7}};
constexpr GUID EnergyPreference = {0x36687f9e, 0xe3a5, 0x4dbf, {0xb1, 0xdc, 0x15, 0xeb, 0x38, 0x1c, 0x68, 0x63}};
constexpr GUID LocalAppDataFolder = {0xF1B32785, 0x6FBA, 0x4FCF, {0x9D, 0x55, 0x7B, 0x8E, 0x7F, 0x15, 0x70, 0x91}};
constexpr GUID DisplayClass = {0x4d36e968, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
constexpr wchar_t LabKey[] = L"Software\\BatteryScope\\Lab";
// IAudioMeterInformation (endpointvolume.h), declared here because some SDKs only forward-declare it.
constexpr GUID PeakMeterId = {0xC02216F6, 0x8C67, 0x4B5B, {0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64}};
struct PeakMeter : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetPeakValue(float* peak) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMeteringChannelCount(UINT* count) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChannelsPeakValues(UINT32 count, float* peaks) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryHardwareSupport(DWORD* mask) = 0;
};

// The plan APIs are loaded at run time: their declarations differ between SDKs.
struct PlanApi {
    DWORD (WINAPI* getActive)(HKEY, GUID**) = nullptr;
    DWORD (WINAPI* setActive)(HKEY, const GUID*) = nullptr;
    DWORD (WINAPI* readDc)(HKEY, const GUID*, const GUID*, const GUID*, LPDWORD) = nullptr;
    DWORD (WINAPI* writeDc)(HKEY, const GUID*, const GUID*, const GUID*, DWORD) = nullptr;
};
template<class T> void bind(HMODULE module, const char* name, T& target) {
    FARPROC symbol = module ? GetProcAddress(module, name) : nullptr;
    static_assert(sizeof(symbol) == sizeof(target));
    std::memcpy(&target, &symbol, sizeof(target));
}
const PlanApi& planApi() {
    static PlanApi api = [] {
        PlanApi result; HMODULE module = LoadLibraryW(L"powrprof.dll");
        bind(module, "PowerGetActiveScheme", result.getActive);
        bind(module, "PowerSetActiveScheme", result.setActive);
        bind(module, "PowerReadDCValueIndex", result.readDc);
        bind(module, "PowerWriteDCValueIndex", result.writeDc);
        return result;
    }();
    return api;
}
DWORD readProcessor(const GUID& setting, DWORD& value) {
    const auto& api = planApi();
    if (!api.getActive || !api.readDc) return ERROR_PROC_NOT_FOUND;
    GUID* scheme = nullptr;
    DWORD status = api.getActive(nullptr, &scheme);
    if (status) return status;
    status = api.readDc(nullptr, scheme, &ProcessorGroup, &setting, &value);
    LocalFree(scheme);
    return status;
}
DWORD writeProcessor(const GUID& setting, DWORD value) {
    const auto& api = planApi();
    if (!api.getActive || !api.writeDc || !api.setActive) return ERROR_PROC_NOT_FOUND;
    GUID* scheme = nullptr;
    DWORD status = api.getActive(nullptr, &scheme);
    if (status) return status;
    status = api.writeDc(nullptr, scheme, &ProcessorGroup, &setting, value);
    if (!status) status = api.setActive(nullptr, scheme); // applies the changed value now
    LocalFree(scheme);
    return status;
}
int currentRefresh() {
    DEVMODEW mode{}; mode.dmSize = sizeof(mode);
    return EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) ? int(mode.dmDisplayFrequency) : 0;
}
// Refresh rates offered at the current resolution and color depth.
std::vector<int> refreshRates() {
    DEVMODEW current{}; current.dmSize = sizeof(current);
    std::vector<int> rates;
    if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &current)) return rates;
    DEVMODEW mode{}; mode.dmSize = sizeof(mode);
    for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &mode); ++i)
        if (mode.dmPelsWidth == current.dmPelsWidth && mode.dmPelsHeight == current.dmPelsHeight && mode.dmBitsPerPel == current.dmBitsPerPel
            && std::find(rates.begin(), rates.end(), int(mode.dmDisplayFrequency)) == rates.end())
            rates.push_back(int(mode.dmDisplayFrequency));
    std::sort(rates.begin(), rates.end());
    return rates;
}
DWORD setRefresh(int hz) {
    DEVMODEW mode{}; mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode)) return GetLastError();
    if (int(mode.dmDisplayFrequency) == hz) return ERROR_SUCCESS;
    mode.dmDisplayFrequency = DWORD(hz); mode.dmFields = DM_DISPLAYFREQUENCY;
    // Flags 0: a dynamic change that Windows does not save, so a restart restores the user's rate.
    const LONG result = ChangeDisplaySettingsExW(nullptr, &mode, nullptr, 0, nullptr);
    return result == DISP_CHANGE_SUCCESSFUL ? ERROR_SUCCESS : ERROR_INVALID_PARAMETER;
}
bool lowerEquals(std::wstring a, const wchar_t* b) {
    std::transform(a.begin(), a.end(), a.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
    return a == b;
}
std::wstring luidText(const LUID& luid) {
    wchar_t text[48];
    swprintf(text, 48, L"luid_0x%08lX_0x%08lX", static_cast<unsigned long>(luid.HighPart), static_cast<unsigned long>(luid.LowPart));
    return text;
}
// Microphone or camera in use by any app: Windows records a start time with no stop time.
bool deviceInUse(const wchar_t* capability) {
    const std::wstring root = std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\") + capability;
    std::function<bool(const std::wstring&, int)> scan = [&](const std::wstring& path, int depth) -> bool {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
        bool active = false;
        ULONGLONG start = 0, stop = 0; DWORD size = sizeof(start);
        if (RegQueryValueExW(key, L"LastUsedTimeStart", nullptr, nullptr, reinterpret_cast<BYTE*>(&start), &size) == ERROR_SUCCESS) {
            size = sizeof(stop);
            if (RegQueryValueExW(key, L"LastUsedTimeStop", nullptr, nullptr, reinterpret_cast<BYTE*>(&stop), &size) == ERROR_SUCCESS)
                active = start != 0 && stop == 0;
        }
        wchar_t name[512];
        for (DWORD i = 0; !active && depth < 2; ++i) {
            DWORD length = DWORD(std::size(name));
            if (RegEnumKeyExW(key, i, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            active = scan(path + L"\\" + name, depth + 1);
        }
        RegCloseKey(key);
        return active;
    };
    return scan(root, 0);
}
}

const wchar_t* knobKey(Knob knob) {
    switch (knob) {
    case Knob::cpuBoost: return L"cpu-boost";
    case Knob::energyPreference: return L"epp";
    case Knob::refreshRate: return L"refresh";
    case Knob::powerMode: return L"power-mode";
    default: return L"";
    }
}
DWORD readKnob(Knob knob, DWORD& value) {
    switch (knob) {
    case Knob::cpuBoost: return readProcessor(BoostMode, value);
    case Knob::energyPreference: return readProcessor(EnergyPreference, value);
    case Knob::refreshRate: { const int hz = currentRefresh(); value = DWORD(hz); return hz ? ERROR_SUCCESS : ERROR_GEN_FAILURE; }
    case Knob::powerMode: {
        const auto modes = queryPowerModes();
        if (!modes.dcKnown) return modes.error ? modes.error : ERROR_GEN_FAILURE;
        value = DWORD(modes.dc); return ERROR_SUCCESS;
    }
    default: return ERROR_INVALID_PARAMETER;
    }
}
DWORD applyKnob(Knob knob, DWORD value) {
    switch (knob) {
    case Knob::cpuBoost: return writeProcessor(BoostMode, value);
    case Knob::energyPreference: return writeProcessor(EnergyPreference, value);
    case Knob::refreshRate: return setRefresh(int(value));
    case Knob::powerMode: { GUID replaced{}; return overridePowerMode(false, PowerMode(value), replaced); }
    default: return ERROR_INVALID_PARAMETER;
    }
}
KnobPlan planKnob(Knob knob) {
    KnobPlan plan;
    DWORD original = 0;
    if ((plan.error = readKnob(knob, original)) != ERROR_SUCCESS) return plan;
    plan.original = original;
    switch (knob) {
    case Knob::cpuBoost:
        // 0 = disabled; keep the user's own enabled flavor (aggressive, efficient...) for the "on" arm.
        plan.values = {original ? original : 2u, 0u};
        plan.names = {L"Boost 開", L"Boost 關"};
        break;
    case Knob::energyPreference:
        plan.values = {original, original >= 75 ? 40u : 85u};
        plan.names = {L"能源偏好 " + std::to_wstring(plan.values[0]), L"能源偏好 " + std::to_wstring(plan.values[1])};
        break;
    case Knob::refreshRate: {
        const auto rates = refreshRates();
        if (rates.empty() || rates.back() <= 60 || std::find(rates.begin(), rates.end(), 60) == rates.end()) { plan.error = ERROR_NOT_SUPPORTED; return plan; }
        plan.values = {DWORD(rates.back()), 60u};
        plan.names = {std::to_wstring(rates.back()) + L" Hz", L"60 Hz"};
        break;
    }
    case Knob::powerMode:
        if (PowerMode(original) == PowerMode::other) { plan.error = ERROR_NOT_SUPPORTED; return plan; } // an OEM mode we could not restore exactly
        // The mode in use against efficiency (or, if efficiency is in use, against balanced).
        plan.values = {original, PowerMode(original) == PowerMode::efficiency ? DWORD(PowerMode::balanced) : DWORD(PowerMode::efficiency)};
        for (size_t i = 0; i < 2; ++i)
            plan.names[i] = PowerMode(plan.values[i]) == PowerMode::efficiency ? L"效率" : PowerMode(plan.values[i]) == PowerMode::balanced ? L"平衡" : L"效能";
        break;
    default: plan.error = ERROR_INVALID_PARAMETER; return plan;
    }
    // Writing the value it already has proves the setting can be changed by this user.
    plan.error = applyKnob(knob, original);
    plan.available = plan.error == ERROR_SUCCESS;
    return plan;
}

void rememberOriginal(Knob knob, DWORD value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, LabKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    const DWORD id = DWORD(knob);
    RegSetValueExW(key, L"Knob", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&id), sizeof(id));
    RegSetValueExW(key, L"Original", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
}
void forgetOriginal() { RegDeleteKeyValueW(HKEY_CURRENT_USER, LabKey, L"Knob"); RegDeleteKeyValueW(HKEY_CURRENT_USER, LabKey, L"Original"); }
bool restoreInterrupted() {
    DWORD knob = 0, value = 0, size = sizeof(knob);
    if (RegGetValueW(HKEY_CURRENT_USER, LabKey, L"Knob", RRF_RT_REG_DWORD, nullptr, &knob, &size) != ERROR_SUCCESS) return false;
    size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, LabKey, L"Original", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS
        && knob < DWORD(Knob::count) && applyKnob(Knob(knob), value) != ERROR_SUCCESS) return false; // keep the record and retry next time
    forgetOriginal();
    return true;
}
std::wstring labFolder() {
    PWSTR base = nullptr;
    std::wstring folder;
    if (SUCCEEDED(SHGetKnownFolderPath(LocalAppDataFolder, KF_FLAG_CREATE, nullptr, &base)) && base)
        folder = (std::filesystem::path(base) / L"BatteryScope" / L"lab").wstring();
    CoTaskMemFree(base);
    std::error_code error;
    if (!folder.empty()) std::filesystem::create_directories(folder, error);
    return folder;
}

struct Probe::Impl {
    std::wstring discreteLuid, discreteName;
    std::vector<std::wstring> integratedLuids;
    UINT discreteVendor = 0;
    uint64_t adaptersAt = 0;
    PeakMeter* meter = nullptr;
    uint64_t meterAt = 0;
    bool comReady = false;
    DWM_TIMING_INFO previous{};
    bool timingKnown = false;
    std::filesystem::file_time_type ghelperConfigTime{};
    ~Impl() { if (meter) meter->Release(); if (comReady) CoUninitialize(); }
    void findAdapters(uint64_t now) {
        if (adaptersAt && now - adaptersAt < 60000) return;
        adaptersAt = now; discreteLuid.clear(); discreteName.clear(); integratedLuids.clear(); discreteVendor = 0;
        IDXGIFactory1* factory = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return;
        struct Found { DXGI_ADAPTER_DESC1 desc; };
        std::vector<Found> hardware;
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) hardware.push_back({desc});
            adapter->Release();
        }
        factory->Release();
        if (hardware.size() < 2) { for (auto& h : hardware) integratedLuids.push_back(luidText(h.desc.AdapterLuid)); return; }
        // The discrete GPU: NVIDIA if present, otherwise the one with the most dedicated memory.
        size_t pick = 0;
        for (size_t i = 0; i < hardware.size(); ++i) {
            const bool nvidia = hardware[i].desc.VendorId == 0x10DE;
            if (nvidia || (hardware[pick].desc.VendorId != 0x10DE && hardware[i].desc.DedicatedVideoMemory > hardware[pick].desc.DedicatedVideoMemory)) pick = i;
        }
        for (size_t i = 0; i < hardware.size(); ++i) {
            if (i == pick) { discreteLuid = luidText(hardware[i].desc.AdapterLuid); discreteName = hardware[i].desc.Description; discreteVendor = hardware[i].desc.VendorId; }
            else integratedLuids.push_back(luidText(hardware[i].desc.AdapterLuid));
        }
    }
    // D0 = 0 ... D3 = 3; 9 when the discrete GPU is not present (switched off, e.g. Eco mode).
    int discreteState() {
        if (!discreteVendor) return -1;
        HDEVINFO set = SetupDiGetClassDevsW(&DisplayClass, nullptr, nullptr, 0);
        if (set == INVALID_HANDLE_VALUE) return -1;
        int state = 9;
        wchar_t vendor[16]; swprintf(vendor, 16, L"VEN_%04X", discreteVendor);
        for (DWORD i = 0;; ++i) {
            SP_DEVINFO_DATA device{}; device.cbSize = sizeof(device);
            if (!SetupDiEnumDeviceInfo(set, i, &device)) break;
            wchar_t ids[1024]{};
            if (!SetupDiGetDeviceRegistryPropertyW(set, &device, SPDRP_HARDWAREID, nullptr, reinterpret_cast<BYTE*>(ids), sizeof(ids) - sizeof(wchar_t), nullptr)) continue;
            if (!wcsstr(ids, vendor)) continue;
            ULONG status = 0, problem = 0;
            if (CM_Get_DevNode_Status(&status, &problem, device.DevInst, 0) != CR_SUCCESS || !(status & DN_STARTED)) continue; // disabled or removed
            CM_POWER_DATA power{}; power.PD_Size = sizeof(power);
            if (SetupDiGetDeviceRegistryPropertyW(set, &device, SPDRP_DEVICE_POWER_DATA, nullptr, reinterpret_cast<BYTE*>(&power), sizeof(power), nullptr)
                && power.PD_MostRecentPowerState >= PowerDeviceD0 && power.PD_MostRecentPowerState <= PowerDeviceD3)
                state = int(power.PD_MostRecentPowerState) - int(PowerDeviceD0);
            else state = -1;
            break;
        }
        SetupDiDestroyDeviceInfoList(set);
        return state;
    }
    bool audioPlaying(uint64_t now) {
        if (!comReady) comReady = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        if (!meter && (!meterAt || now - meterAt > 30000)) {
            meterAt = now;
            IMMDeviceEnumerator* devices = nullptr;
            if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&devices)))) {
                IMMDevice* device = nullptr;
                if (SUCCEEDED(devices->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
                    device->Activate(PeakMeterId, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&meter));
                    device->Release();
                }
                devices->Release();
            }
        }
        float peak = 0;
        if (meter && FAILED(meter->GetPeakValue(&peak))) { meter->Release(); meter = nullptr; return false; } // device changed: find it again
        return peak > 0.001f;
    }
    void composition(Sample& s) {
        DWM_TIMING_INFO timing{}; timing.cbSize = sizeof(timing);
        if (FAILED(DwmGetCompositionTimingInfo(nullptr, &timing))) { timingKnown = false; return; }
        if (timingKnown && timing.cFrame >= previous.cFrame) {
            s.framesComposed = double(timing.cFrame - previous.cFrame);
            s.refreshes = timing.cRefresh >= previous.cRefresh ? double(timing.cRefresh - previous.cRefresh) : 0;
            const auto lost = [](DWM_FRAME_COUNT now, DWM_FRAME_COUNT before) { return now >= before ? double(now - before) : 0.0; };
            s.framesDropped = lost(timing.cFramesDropped, previous.cFramesDropped) + lost(timing.cFramesMissed, previous.cFramesMissed);
        }
        previous = timing; timingKnown = true;
    }
    // Keeps a copy of G-Helper's settings whenever they change, next to the trace.
    void ghelperConfig(const std::wstring& folder) {
        PWSTR roaming = nullptr;
        constexpr GUID RoamingFolder = {0x3EB685DB, 0x65F9, 0x4CF6, {0xA0, 0x3A, 0xE3, 0xEF, 0x65, 0x72, 0x9F, 0x3D}};
        if (FAILED(SHGetKnownFolderPath(RoamingFolder, 0, nullptr, &roaming)) || !roaming) { CoTaskMemFree(roaming); return; }
        const auto config = std::filesystem::path(roaming) / L"GHelper" / L"config.json";
        CoTaskMemFree(roaming);
        std::error_code error;
        const auto time = std::filesystem::last_write_time(config, error);
        if (error || time == ghelperConfigTime || folder.empty()) return;
        ghelperConfigTime = time;
        SYSTEMTIME now{}; GetLocalTime(&now);
        wchar_t name[64]; swprintf(name, 64, L"ghelper-config-%04u%02u%02u-%02u%02u%02u.json", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
        std::filesystem::copy_file(config, std::filesystem::path(folder) / name, std::filesystem::copy_options::overwrite_existing, error);
    }
};

Probe::Probe() : impl_(std::make_unique<Impl>()) {}
Probe::~Probe() = default;
void Probe::read(Sample& s, const std::vector<std::pair<std::wstring, double>>& gpu, const std::vector<ProcessRow>& processes) {
    auto& p = *impl_;
    const uint64_t now = GetTickCount64();
    LASTINPUTINFO input{}; input.cbSize = sizeof(input);
    s.signals.idleSeconds = GetLastInputInfo(&input) ? (GetTickCount() - input.dwTime) / 1000.0 : 0;
    s.signals.audio = p.audioPlaying(now);
    s.signals.call = deviceInUse(L"microphone") || deviceInUse(L"webcam");
    QUERY_USER_NOTIFICATION_STATE notification{};
    s.signals.fullscreenGame = SUCCEEDED(SHQueryUserNotificationState(&notification)) && notification == QUNS_RUNNING_D3D_FULL_SCREEN;
    s.signals.systemCpu = s.systemCpu;
    s.context = classify(s.signals);
    DWORD foregroundPid = 0;
    if (HWND window = GetForegroundWindow()) GetWindowThreadProcessId(window, &foregroundPid);
    std::vector<const ProcessRow*> busy;
    for (const auto& row : processes) {
        if (row.pid == foregroundPid) s.foreground = row.name;
        if (lowerEquals(row.name, L"ghelper.exe")) s.ghelper = true;
        if (row.measured && row.cpu >= .5) busy.push_back(&row);
    }
    std::sort(busy.begin(), busy.end(), [](const ProcessRow* a, const ProcessRow* b) { return a->cpu > b->cpu; });
    for (size_t i = 0; i < busy.size() && i < 5; ++i) {
        wchar_t cpu[16]; swprintf(cpu, 16, L" %.1f%%", busy[i]->cpu);
        s.topProcesses += (i ? L";" : L"") + busy[i]->name + cpu;
    }
    s.refreshHz = currentRefresh();
    p.composition(s);
    DWORD value = 0;
    if (readKnob(Knob::cpuBoost, value) == ERROR_SUCCESS) s.boostDc = int(value);
    if (readKnob(Knob::energyPreference, value) == ERROR_SUCCESS) s.eppDc = int(value);
    // GPU engines: "pid_<pid>_luid_0x<high>_0x<low>_phys_0_eng_<n>_engtype_<type>".
    p.findAdapters(now);
    std::map<std::pair<std::wstring, std::wstring>, double> perEngine;                       // (luid, type) -> sum over processes
    std::map<std::pair<uint32_t, std::wstring>, std::map<std::wstring, double>> perProcess; // (pid, luid) -> type -> sum
    for (const auto& [name, percent] : gpu) {
        if (!std::isfinite(percent) || percent <= 0) continue;
        const size_t luid = name.find(L"luid_"), type = name.find(L"_engtype_"), phys = name.find(L"_phys_");
        if (luid == std::wstring::npos || type == std::wstring::npos || phys == std::wstring::npos || phys < luid) continue;
        const std::wstring adapter = name.substr(luid, phys - luid), engine = name.substr(type + 9);
        uint32_t pid = 0;
        if (name.compare(0, 4, L"pid_") == 0) pid = uint32_t(wcstoul(name.c_str() + 4, nullptr, 10));
        perEngine[{adapter, engine}] += percent;
        if (pid) perProcess[{pid, adapter}][engine] += percent;
    }
    auto busiest = [&](const std::wstring& adapter) -> Value {
        Value best;
        for (const auto& [key, sum] : perEngine) if (key.first == adapter) best = std::max(best.value_or(0), std::min(100.0, sum));
        return best;
    };
    if (!p.discreteLuid.empty()) s.dgpuPercent = busiest(p.discreteLuid).value_or(0);
    for (const auto& luid : p.integratedLuids) if (auto v = busiest(luid)) s.igpuPercent = std::max(s.igpuPercent.value_or(0), *v);
    std::set<std::wstring> apps;
    for (const auto& [key, engines] : perProcess) {
        if (key.second != p.discreteLuid) continue;
        double top = 0; for (const auto& e : engines) top = std::max(top, e.second);
        if (top < 1) continue;
        for (const auto& row : processes) if (row.pid == key.first) { apps.insert(row.name); break; }
    }
    for (const auto& app : apps) s.dgpuApps += (s.dgpuApps.empty() ? L"" : L";") + app;
    s.dgpuState = p.discreteState();
    if (s.ghelper) p.ghelperConfig(labFolder());
}
}

namespace bs::lab {
namespace {
std::string utf8Text(const std::wstring& text) {
    if (text.empty()) return {};
    std::string out(size_t(WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0, nullptr, nullptr)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), out.data(), int(out.size()), nullptr, nullptr);
    return out;
}
std::wstring todayStamp(wchar_t separator) {
    SYSTEMTIME t{}; GetLocalTime(&t);
    wchar_t text[32];
    if (separator) swprintf(text, 32, L"%04u-%02u-%02u %02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    else swprintf(text, 32, L"%04u%02u%02u", t.wYear, t.wMonth, t.wDay);
    return text;
}
void appendFile(const std::filesystem::path& path, const std::string& header, const std::string& rows) {
    std::error_code error;
    const bool fresh = !std::filesystem::exists(path, error);
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    const std::string text = (fresh ? header : std::string()) + rows;
    DWORD written = 0;
    WriteFile(file, text.data(), DWORD(text.size()), &written, nullptr);
    CloseHandle(file);
}
// Keeps the newest 14 daily traces.
void pruneTraces(const std::wstring& folder) {
    std::vector<std::filesystem::path> traces;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error))
        if (entry.path().filename().wstring().rfind(L"trace-", 0) == 0) traces.push_back(entry.path());
    std::sort(traces.begin(), traces.end());
    for (size_t i = 0; i + 14 < traces.size(); ++i) std::filesystem::remove(traces[i], error);
}
}

Controller::Controller(std::wstring folder, std::shared_ptr<Actuator> actuator) : folder_(std::move(folder)), actuator_(std::move(actuator)) {
    if (folder_.empty()) return;
    HANDLE file = CreateFileW((std::filesystem::path(folder_) / L"blocks.csv").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    std::string text; char buffer[65536]; DWORD read = 0;
    while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read && text.size() < (16u << 20)) text.append(buffer, read);
    CloseHandle(file);
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        Block block;
        if (Block::parse(text.substr(at, end - at), block)) blocks_.push_back(block);
        at = end + 1;
    }
}
void Controller::setRecording(bool on) {
    std::lock_guard<std::mutex> guard(lock_);
    if (recording_ && !on) flush();
    recording_ = on;
    if (on && !folder_.empty()) pruneTraces(folder_);
}
std::wstring experimentKey(Knob knob, Workload workload) {
    std::wstring key = knobKey(knob);
    if (workload != Workload::none) key += std::wstring(L"@") + workloadKey(workload);
    return key;
}
std::vector<Step> automaticPlan(bool browser) {
    // Each setting under the activities it can change; 3 pairs of 45-second blocks (35 counted) per step.
    using W = Workload;
    std::vector<std::pair<Knob, Workload>> order;
    if (browser) order = {
        {Knob::powerMode, W::idle}, {Knob::powerMode, W::browse}, {Knob::powerMode, W::video}, {Knob::powerMode, W::office},
        {Knob::refreshRate, W::idle}, {Knob::refreshRate, W::browse}, {Knob::refreshRate, W::video},
        {Knob::cpuBoost, W::browse}, {Knob::cpuBoost, W::office}, {Knob::cpuBoost, W::sustained},
        {Knob::energyPreference, W::browse}, {Knob::energyPreference, W::video}, {Knob::energyPreference, W::sustained}};
    else order = {
        {Knob::powerMode, W::idle}, {Knob::powerMode, W::bursts}, {Knob::powerMode, W::scroll},
        {Knob::refreshRate, W::idle}, {Knob::refreshRate, W::scroll},
        {Knob::cpuBoost, W::bursts}, {Knob::cpuBoost, W::scroll}, {Knob::cpuBoost, W::sustained},
        {Knob::energyPreference, W::bursts}, {Knob::energyPreference, W::sustained}};
    std::vector<Step> steps;
    for (const auto& [knob, workload] : order) {
        Step step; step.knob = knob; step.workload = workload;
        step.pairs = 3; step.blockSeconds = 45; step.warmupSeconds = 10; step.minimumSeconds = 25;
        // The first video step records the 12-second test clip; every step lets its load start first.
        step.prepareSeconds = workload == W::video ? 20 : 6;
        steps.push_back(step);
    }
    return steps;
}
void Controller::log(const std::wstring& text) {
    pendingLog_ += utf8Text(todayStamp(L' ') + L"  " + text) + "\r\n";
}
bool Controller::beginStep() {
    while (stepIndex_ < steps_.size()) {
        const Step& step = steps_[stepIndex_];
        KnobPlan plan = actuator_->plan(step.knob);
        if (plan.available) {
            actuator_->remember(step.knob, plan.original);
            knob_ = step.knob; plan_ = plan; applied_ = false; paused_ = false;
            FILETIME now{}; GetSystemTimeAsFileTime(&now);
            run_ = ((uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime) / 10000000ull + stepIndex_;
            schedule_ = Schedule(run_ * 2654435761ull);
            block_ = 0; pairs_.clear(); prepareStart_ = 0; prepared_ = false;
            log(L"step " + std::to_wstring(stepIndex_ + 1) + L"/" + std::to_wstring(steps_.size()) + L" " + experimentKey(step.knob, step.workload) +
                L": original " + std::to_wstring(plan.original) + L", arms " + std::to_wstring(plan.values[0]) + L" / " + std::to_wstring(plan.values[1]));
            return true;
        }
        lastError_ = plan.error; ++skipped_;
        log(L"step " + std::to_wstring(stepIndex_ + 1) + L" " + experimentKey(step.knob, step.workload) + L" skipped: error " + std::to_wstring(plan.error));
        ++stepIndex_;
    }
    return false;
}
DWORD Controller::start(Knob knob) {
    Step step; step.knob = knob; step.pairs = TargetPairs; step.blockSeconds = BlockSeconds;
    std::lock_guard<std::mutex> guard(lock_);
    if (running_) return ERROR_BUSY;
    KnobPlan plan = actuator_->plan(knob);
    if (!plan.available) { lastError_ = plan.error; return plan.error; }
    steps_ = {step}; stepIndex_ = 0; planMode_ = false; skipped_ = 0;
    externalChanges_ = 0; lastError_ = ERROR_SUCCESS; finished_ = false;
    if (!beginStep()) return lastError_;
    running_ = true;
    return ERROR_SUCCESS;
}
DWORD Controller::startPlan(const std::vector<Step>& steps) {
    std::lock_guard<std::mutex> guard(lock_);
    if (running_) return ERROR_BUSY;
    if (steps.empty()) return ERROR_INVALID_PARAMETER;
    steps_ = steps; stepIndex_ = 0; planMode_ = true; skipped_ = 0;
    externalChanges_ = 0; lastError_ = ERROR_SUCCESS; finished_ = false;
    log(L"automatic test started, " + std::to_wstring(steps.size()) + L" steps");
    if (!beginStep()) { log(L"no setting can be changed; nothing to test"); return lastError_ ? lastError_ : ERROR_NOT_SUPPORTED; }
    running_ = true;
    return ERROR_SUCCESS;
}
Workload Controller::workload() const {
    std::lock_guard<std::mutex> guard(lock_);
    return running_ && stepIndex_ < steps_.size() ? steps_[stepIndex_].workload : Workload::none;
}
void Controller::finishStep() {
    // Always put the original back: a block may have just ended with its setting still in place.
    actuator_->apply(knob_, plan_.original);
    actuator_->forget();
    applied_ = false;
    log(L"step " + std::to_wstring(stepIndex_ + 1) + L" done, setting restored to " + std::to_wstring(plan_.original));
}
void Controller::finishRun(bool completed) {
    if (!running_) return;
    finishStep();
    running_ = false; paused_ = false; finished_ = completed;
    log(completed ? L"finished" : L"stopped");
    flush();
}
void Controller::stop() { std::lock_guard<std::mutex> guard(lock_); finishRun(false); }
bool Controller::active() const { std::lock_guard<std::mutex> guard(lock_); return recording_ || running_; }
void Controller::shutdown() { std::lock_guard<std::mutex> guard(lock_); finishRun(false); flush(); }
void Controller::flushNow() { std::lock_guard<std::mutex> guard(lock_); flush(); }
void Controller::flush() {
    if (!pendingLog_.empty()) {
        if (!folder_.empty()) appendFile(std::filesystem::path(folder_) / L"lab.log", std::string(), pendingLog_);
        pendingLog_.clear();
    }
    if (pendingRows_.empty()) return;
    if (!folder_.empty()) appendFile(std::filesystem::path(folder_) / (L"trace-" + traceDate_ + L".csv"), csvHeader(), pendingRows_);
    pendingRows_.clear(); pendingCount_ = 0;
}
void Controller::onSample(Sample& s, double seconds) {
    std::lock_guard<std::mutex> guard(lock_);
    if (!recording_ && !running_) return;
    ghelper_ = s.ghelper;
    if (running_) {
        const Step& step = steps_[stepIndex_];
        s.experiment = experimentKey(step.knob, step.workload);
        s.block = block_;
        if (!s.onBattery) {
            // Settings on battery do not apply while plugged in: undo, and redo this block later.
            if (applied_) { actuator_->apply(knob_, plan_.original); applied_ = false; log(L"plugged in: paused, setting restored"); }
            paused_ = true;
        } else if (!prepared_ && (prepareStart_ ? s.ms - prepareStart_ : 0) < step.prepareSeconds * 1000) {
            if (!prepareStart_) prepareStart_ = s.ms; // the load is starting; nothing is measured yet
        } else {
            prepared_ = true;
            const int arm = schedule_.armFor(block_);
            const DWORD expected = plan_.values[size_t(arm)];
            if (!applied_) {
                lastError_ = actuator_->apply(knob_, expected);
                if (paused_) log(L"on battery: resumed");
                applied_ = true; paused_ = false;
                recorder_.begin(block_ / 2, arm, s.ms, step.warmupSeconds, step.minimumSeconds);
            } else {
                DWORD actual = 0;
                if (actuator_->read(knob_, actual) == ERROR_SUCCESS && actual != expected) {
                    // Another program (for example G-Helper) changed it: this block no longer measures the arm.
                    ++externalChanges_; recorder_.invalidate(); actuator_->apply(knob_, expected);
                    log(L"setting changed from outside to " + std::to_wstring(actual) + L"; block " + std::to_wstring(block_) + L" will be repeated");
                }
            }
            s.arm = arm;
            s.counted = recorder_.add(s, seconds);
            if (s.ms >= recorder_.startMs() && (s.ms - recorder_.startMs()) / 1000.0 >= step.blockSeconds) {
                Block block;
                if (recorder_.finish(block)) {
                    block.experiment = experimentKey(step.knob, step.workload); block.run = run_; block.armName = plan_.names[size_t(arm)];
                    block.started = todayStamp(L' ');
                    blocks_.push_back(block);
                    if (!folder_.empty()) appendFile(std::filesystem::path(folder_) / L"blocks.csv", Block::csvHeader(), block.csv());
                    pairs_[block_ / 2][size_t(arm)] = true;
                    log(L"block " + std::to_wstring(block_) + L" " + plan_.names[size_t(arm)] + L": " + std::to_wstring(block.battery) + L" W");
                    ++block_;
                } else if (!recorder_.invalid()) { log(L"block " + std::to_wstring(block_) + L" too short, skipped"); ++block_; }
                // An invalidated block is repeated with the same arm, keeping pairs balanced.
                applied_ = false;
                int done = 0; for (const auto& pair : pairs_) done += pair.second[0] && pair.second[1];
                if (done >= step.pairs) {
                    finishStep();
                    ++stepIndex_;
                    if (!beginStep()) finishRun(true);
                }
            }
        }
    }
    if (recording_ || running_) {
        breakdown_.add(s, seconds);
        if (s.onBattery) recordedSeconds_ += seconds;
        const std::wstring date = todayStamp(0);
        if (date != traceDate_) { flush(); traceDate_ = date; }
        pendingRows_ += csvRow(s);
        if (++pendingCount_ >= 15) flush();
    }
}
Controller::Status Controller::status() const {
    std::lock_guard<std::mutex> guard(lock_);
    Status st;
    st.recording = recording_; st.running = running_; st.paused = paused_; st.finished = finished_; st.ghelper = ghelper_;
    st.recordedSeconds = recordedSeconds_; st.knob = knob_; st.armNames = plan_.names;
    st.plan = planMode_; st.step = int(stepIndex_); st.steps = int(steps_.size()); st.skipped = skipped_; st.run = run_;
    st.workload = running_ && stepIndex_ < steps_.size() ? steps_[stepIndex_].workload : Workload::none;
    if (running_) {
        // Battery time left: the rest of this step's blocks plus every later step.
        int done = 0; for (const auto& pair : pairs_) done += pair.second[0] && pair.second[1];
        const Step& step = steps_[stepIndex_];
        double remaining = std::max(0, step.pairs * 2 - block_) * step.blockSeconds + (prepared_ ? 0 : step.prepareSeconds);
        if (applied_) remaining -= std::min(step.blockSeconds, (GetTickCount64() - recorder_.startMs()) / 1000.0);
        for (size_t i = stepIndex_ + 1; i < steps_.size(); ++i) remaining += steps_[i].pairs * 2 * steps_[i].blockSeconds + steps_[i].prepareSeconds;
        st.remainingSeconds = std::max(0.0, remaining);
        (void)done;
    }
    st.arm = running_ && applied_ ? schedule_.armFor(block_) : -1;
    st.block = block_; st.externalChanges = externalChanges_; st.lastError = lastError_;
    for (const auto& pair : pairs_) st.pairsDone += pair.second[0] && pair.second[1];
    st.blockElapsed = running_ && applied_ ? (GetTickCount64() - recorder_.startMs()) / 1000.0 : 0;
    st.breakdown = breakdown_;
    st.results = analyze(blocks_);
    return st;
}
}
