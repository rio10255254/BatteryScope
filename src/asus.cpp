#include "asus.h"
#include <wbemcli.h>
#include <oleauto.h>
#include <setupapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cstring>
#include <cwctype>

namespace bs::asus {
namespace {
constexpr DWORD ControlCode = 0x0022240C;   // the ATKACPI driver's method call
constexpr uint32_t Dsts = 0x53545344;       // "DSTS": read a device
constexpr uint32_t Devs = 0x53564544;       // "DEVS": write a device
std::vector<uint8_t> args(uint32_t device, uint32_t value) {
    std::vector<uint8_t> a(8);
    std::memcpy(a.data(), &device, 4); std::memcpy(a.data() + 4, &value, 4);
    return a;
}
}

Reading decode(uint32_t raw) {
    Reading r; r.raw = raw;
    r.present = raw != 0xFFFFFFFE && raw != 0xFFFFFFFF && (raw & 0x00010000) != 0;
    return r;
}
const wchar_t* modeKey(Mode mode) {
    switch (mode) {
    case Mode::silent: return L"靜音";
    case Mode::turbo: return L"Turbo";
    default: return L"平衡";
    }
}

Acpi::Acpi() {
    handle_ = CreateFileW(L"\\\\.\\ATKACPI", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}
Acpi::~Acpi() { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
std::optional<uint32_t> Acpi::call(uint32_t method, const std::vector<uint8_t>& a) const {
    if (handle_ == INVALID_HANDLE_VALUE) return std::nullopt;
    std::vector<uint8_t> in(8 + a.size());
    const uint32_t length = uint32_t(a.size());
    std::memcpy(in.data(), &method, 4); std::memcpy(in.data() + 4, &length, 4);
    if (!a.empty()) std::memcpy(in.data() + 8, a.data(), a.size());
    uint8_t out[16]{}; DWORD returned = 0;
    if (!DeviceIoControl(handle_, ControlCode, in.data(), DWORD(in.size()), out, sizeof(out), &returned, nullptr)) return std::nullopt;
    uint32_t result = 0; std::memcpy(&result, out, 4);
    return result;
}
std::optional<Reading> Acpi::get(uint32_t device) const {
    auto result = call(Dsts, args(device, 0));
    if (!result) return std::nullopt;
    return decode(*result);
}
std::optional<uint32_t> Acpi::set(uint32_t device, uint32_t value) const { return call(Devs, args(device, value)); }

Snapshot read() {
    Snapshot s;
    wchar_t text[128]{}; DWORD bytes = sizeof(text);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"SystemProductName", RRF_RT_REG_SZ, nullptr, text, &bytes) == ERROR_SUCCESS)
        s.model = text;
    Acpi acpi;
    s.driver = acpi.available();
    if (!s.driver) return s;
    auto get = [&](uint32_t id) -> std::optional<Reading> {
        auto r = acpi.get(id);
        if (r) s.raw.push_back({id, r->raw});
        if (r && r->present) return r;
        return std::nullopt;
    };
    if (auto r = get(device::ThermalPolicy)) { if (r->value() <= 2) s.mode = Mode(r->value()); }
    else if (get(device::ThermalPolicyVivo)) s.vivoNumbering = true; // numbered differently: shown raw only
    if (auto r = get(device::GpuEco)) s.gpuEco = r->value() == 1;
    if (auto r = get(device::GpuMux)) s.gpuMux = r->value() == 1;
    if (auto r = get(device::ChargeLimit)) if (r->value() >= 20 && r->value() <= 100) s.chargeLimit = r->value();
    if (auto r = get(device::PanelOverdrive)) s.overdrive = r->value() == 1;
    if (auto r = get(device::CpuFan)) s.cpuFan = r->value();
    if (auto r = get(device::GpuFan)) s.gpuFan = r->value();
    if (auto r = get(device::MidFan)) s.midFan = r->value();
    if (auto r = get(device::CpuTemp)) if (r->value() > 0 && r->value() < 120) s.cpuTemp = r->value();
    if (auto r = get(device::GpuTemp)) if (r->value() > 0 && r->value() < 120) s.gpuTemp = r->value();
    if (auto r = get(device::KeyboardBacklight)) s.keyboard = std::min(3u, r->value() & 0x7F);
    const std::pair<uint32_t, const wchar_t*> limits[] = {
        {device::PptSpl, L"SPL（持續）"}, {device::PptSppt, L"SPPT（短時）"}, {device::PptFppt, L"FPPT（瞬間）"},
        {device::PptApuSppt, L"APU SPPT"}, {device::PptPlatformSppt, L"平台 SPPT"},
        {device::NvDynamicBoost, L"獨顯 Dynamic Boost"}, {device::NvTempTarget, L"獨顯溫度目標"}};
    for (const auto& [id, label] : limits) if (auto r = get(id)) s.limits.push_back({label, r->value()});
    return s;
}
std::wstring dump(const Snapshot& s) {
    std::wstring out = L"model=" + s.model + L"\nATKACPI=" + (s.driver ? L"yes" : L"no");
    wchar_t line[64];
    for (const auto& [id, raw] : s.raw) { swprintf(line, 64, L"\n0x%08X = 0x%08X", id, raw); out += line; }
    for (const auto& path : hidDevices()) out += L"\nhid " + path;
    return out + L"\n";
}

namespace {
DWORD writeChecked(uint32_t device, uint32_t value, uint32_t expect, DWORD settleMs = 200) {
    Acpi acpi;
    if (!acpi.available()) return ERROR_NOT_SUPPORTED;
    auto before = acpi.get(device);
    if (!before || !before->present) return ERROR_NOT_SUPPORTED;
    if (!acpi.set(device, value)) return ERROR_GEN_FAILURE;
    Sleep(settleMs);
    auto after = acpi.get(device);
    return after && after->present && after->value() == expect ? ERROR_SUCCESS : ERROR_GEN_FAILURE;
}
}
DWORD setMode(Mode mode) { return writeChecked(device::ThermalPolicy, DWORD(mode), DWORD(mode)); }
DWORD setChargeLimit(unsigned percent) {
    if (percent < 20 || percent > 100) return ERROR_INVALID_PARAMETER;
    return writeChecked(device::ChargeLimit, percent, percent);
}
DWORD setGpuEco(bool eco) { return writeChecked(device::GpuEco, eco ? 1 : 0, eco ? 1 : 0, 1500); }
DWORD setOverdrive(bool on) { return writeChecked(device::PanelOverdrive, on ? 1 : 0, on ? 1 : 0); }
DWORD setKeyboardBacklight(unsigned level) {
    if (level > 3) return ERROR_INVALID_PARAMETER;
    Acpi acpi;
    if (!acpi.available()) return ERROR_NOT_SUPPORTED;
    auto before = acpi.get(device::KeyboardBacklight);
    if (!before || !before->present) return ERROR_NOT_SUPPORTED;
    if (!acpi.set(device::KeyboardBacklight, 0x80 | level)) return ERROR_GEN_FAILURE;
    Sleep(100);
    auto after = acpi.get(device::KeyboardBacklight);
    return after && after->present && (after->value() & 0x7F) == level ? ERROR_SUCCESS : ERROR_GEN_FAILURE;
}

// ----- Services -----
namespace {
constexpr wchar_t SavedKey[] = L"Software\\BatteryScope\\AsusServices";
std::wstring lower(std::wstring text) { for (auto& c : text) c = wchar_t(std::towlower(c)); return text; }
bool asusService(const std::wstring& name, const std::wstring& display) {
    const std::wstring n = lower(name), d = lower(display);
    for (const wchar_t* word : {L"asus", L"armoury", L"rog live", L"glidex"})
        if (n.find(word) != std::wstring::npos || d.find(word) != std::wstring::npos) return true;
    return false;
}
struct Sc { SC_HANDLE h = nullptr; explicit Sc(SC_HANDLE v) : h(v) {} ~Sc() { if (h) CloseServiceHandle(h); } };
DWORD startTypeOf(SC_HANDLE service) {
    DWORD needed = 0;
    QueryServiceConfigW(service, nullptr, 0, &needed);
    std::vector<BYTE> buffer(needed ? needed : 1);
    auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
    return needed && QueryServiceConfigW(service, config, needed, &needed) ? config->dwStartType : SERVICE_DEMAND_START;
}
DWORD stopOne(SC_HANDLE service) {
    SERVICE_STATUS_PROCESS status{}; DWORD returned = 0;
    if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof(status), &returned)) return GetLastError();
    if (status.dwCurrentState == SERVICE_STOPPED) return ERROR_SUCCESS;
    SERVICE_STATUS basic{};
    if (!ControlService(service, SERVICE_CONTROL_STOP, &basic)) return GetLastError();
    for (int i = 0; i < 60; ++i) {
        if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof(status), &returned)) return GetLastError();
        if (status.dwCurrentState == SERVICE_STOPPED) return ERROR_SUCCESS;
        Sleep(250);
    }
    return ERROR_TIMEOUT;
}
}
std::vector<Service> services() {
    std::vector<Service> found;
    Sc scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE | SC_MANAGER_CONNECT));
    if (!scm.h) return found;
    DWORD needed = 0, count = 0, resume = 0;
    EnumServicesStatusExW(scm.h, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, nullptr, 0, &needed, &count, &resume, nullptr);
    std::vector<BYTE> buffer(needed + 4096);
    resume = 0;
    if (!EnumServicesStatusExW(scm.h, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, buffer.data(), DWORD(buffer.size()), &needed, &count, &resume, nullptr)) return found;
    const auto* rows = reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());
    for (DWORD i = 0; i < count; ++i) {
        if (!asusService(rows[i].lpServiceName, rows[i].lpDisplayName)) continue;
        Service s; s.name = rows[i].lpServiceName; s.display = rows[i].lpDisplayName;
        s.running = rows[i].ServiceStatusProcess.dwCurrentState != SERVICE_STOPPED;
        Sc service(OpenServiceW(scm.h, s.name.c_str(), SERVICE_QUERY_CONFIG));
        s.startType = service.h ? startTypeOf(service.h) : SERVICE_DEMAND_START;
        found.push_back(s);
    }
    return found;
}
DWORD stopServices(bool keepOff) {
    Sc scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm.h) return GetLastError();
    HKEY saved = nullptr;
    RegCreateKeyExW(HKEY_CURRENT_USER, SavedKey, 0, nullptr, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &saved, nullptr);
    DWORD first = ERROR_SUCCESS;
    // Two passes: a service another ASUS service depends on stops once that one has.
    for (int pass = 0; pass < 2; ++pass)
        for (const auto& s : services()) {
            Sc service(OpenServiceW(scm.h, s.name.c_str(), SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG | SERVICE_CHANGE_CONFIG));
            if (!service.h) { if (pass) first = first ? first : GetLastError(); continue; }
            if (keepOff && saved) {
                DWORD old = 0, size = sizeof(old);
                if (RegQueryValueExW(saved, s.name.c_str(), nullptr, nullptr, reinterpret_cast<BYTE*>(&old), &size) != ERROR_SUCCESS) {
                    const DWORD type = startTypeOf(service.h);
                    RegSetValueExW(saved, s.name.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&type), sizeof(type));
                }
                ChangeServiceConfigW(service.h, SERVICE_NO_CHANGE, SERVICE_DISABLED, SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
            }
            const DWORD status = stopOne(service.h);
            if (pass && status && !first) first = status;
        }
    if (saved) RegCloseKey(saved);
    // Armoury Crate's own programs in the user session.
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
        for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
            const std::wstring name = lower(entry.szExeFile);
            if (name.rfind(L"armourycrate", 0) != 0 && name != L"armourysocketserver.exe" && name != L"armouryswagent.exe") continue;
            if (HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, entry.th32ProcessID)) { TerminateProcess(process, 0); CloseHandle(process); }
        }
        CloseHandle(snapshot);
    }
    return first;
}
DWORD startServices() {
    Sc scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm.h) return GetLastError();
    HKEY saved = nullptr;
    const bool haveSaved = RegOpenKeyExW(HKEY_CURRENT_USER, SavedKey, 0, KEY_QUERY_VALUE, &saved) == ERROR_SUCCESS;
    DWORD first = ERROR_SUCCESS;
    for (const auto& s : services()) {
        Sc service(OpenServiceW(scm.h, s.name.c_str(), SERVICE_START | SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS));
        if (!service.h) { first = first ? first : GetLastError(); continue; }
        DWORD type = s.startType, size = sizeof(type);
        if (haveSaved && RegQueryValueExW(saved, s.name.c_str(), nullptr, nullptr, reinterpret_cast<BYTE*>(&type), &size) == ERROR_SUCCESS)
            ChangeServiceConfigW(service.h, SERVICE_NO_CHANGE, type, SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        if ((type == SERVICE_AUTO_START || type == SERVICE_BOOT_START || type == SERVICE_SYSTEM_START) && !StartServiceW(service.h, 0, nullptr)) {
            const DWORD error = GetLastError();
            if (error != ERROR_SERVICE_ALREADY_RUNNING && !first) first = error;
        }
    }
    if (haveSaved) { RegCloseKey(saved); RegDeleteKeyW(HKEY_CURRENT_USER, SavedKey); }
    return first;
}
bool servicesKeptOff() {
    HKEY saved = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, SavedKey, 0, KEY_QUERY_VALUE, &saved) != ERROR_SUCCESS) return false;
    DWORD values = 0;
    RegQueryInfoKeyW(saved, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(saved);
    return values > 0;
}

// ----- Hotkeys -----
const wchar_t* keyName(unsigned code) {
    switch (code) {
    case key::KeyboardUp: return L"鍵盤背光調亮";
    case key::KeyboardDown: return L"鍵盤背光調暗";
    case key::Mode: return L"效能模式鍵（Fn+F5）";
    case key::Rog: return L"ROG 鍵";
    case key::MicMute: return L"麥克風靜音鍵";
    case key::Touchpad: return L"觸控板鍵";
    default: return nullptr;
    }
}
void Hotkeys::start(HWND target, UINT message) {
    if (thread_.joinable()) return;
    stop_ = false;
    thread_ = std::thread([this, target, message] {
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return;
        IWbemLocator* locator = nullptr; IWbemServices* wmi = nullptr; IEnumWbemClassObject* events = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator, reinterpret_cast<void**>(&locator)))) {
            BSTR root = SysAllocString(L"ROOT\\WMI");
            if (SUCCEEDED(locator->ConnectServer(root, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &wmi))) {
                CoSetProxyBlanket(wmi, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
                BSTR language = SysAllocString(L"WQL"), query = SysAllocString(L"SELECT * FROM AsusAtkWmiEvent");
                wmi->ExecNotificationQuery(language, query, WBEM_FLAG_RETURN_IMMEDIATELY | WBEM_FLAG_FORWARD_ONLY, nullptr, &events);
                SysFreeString(language); SysFreeString(query);
            }
            SysFreeString(root);
        }
        while (events && !stop_) {
            IWbemClassObject* event = nullptr; ULONG returned = 0;
            const HRESULT result = events->Next(500, 1, &event, &returned); // wakes twice a second to check for stop
            if (result == WBEM_S_TIMEDOUT) continue;
            if (FAILED(result) || !returned) break;
            VARIANT id; VariantInit(&id);
            if (SUCCEEDED(event->Get(L"EventID", 0, &id, nullptr, nullptr))) {
                VARIANT number; VariantInit(&number);
                if (SUCCEEDED(VariantChangeType(&number, &id, 0, VT_UI4))) PostMessageW(target, message, WPARAM(number.ulVal), 0);
                VariantClear(&number);
            }
            VariantClear(&id);
            event->Release();
        }
        if (events) events->Release();
        if (wmi) wmi->Release();
        if (locator) locator->Release();
        CoUninitialize();
    });
}
void Hotkeys::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

std::vector<std::wstring> hidDevices() {
    std::vector<std::wstring> paths;
    constexpr GUID HidInterface = {0x4D1E55B2, 0xF16F, 0x11CF, {0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};
    HDEVINFO set = SetupDiGetClassDevsW(&HidInterface, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return paths;
    SP_DEVICE_INTERFACE_DATA item{}; item.cbSize = sizeof(item);
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &HidInterface, i, &item); ++i) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &item, nullptr, 0, &needed, nullptr);
        std::vector<BYTE> buffer(needed + sizeof(wchar_t));
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &item, detail, needed, nullptr, nullptr)) continue;
        const std::wstring path = lower(detail->DevicePath);
        if (path.find(L"vid_0b05") != std::wstring::npos) paths.push_back(path);
    }
    SetupDiDestroyDeviceInfoList(set);
    return paths;
}
}
