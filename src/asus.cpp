#include "asus.h"
#include "stability.h"
#include <wbemcli.h>
#include <oleauto.h>
#include <setupapi.h>
#include <tlhelp32.h>
#include <mmdeviceapi.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwctype>
#include <iterator>
#include <mutex>

namespace bs::asus {
namespace {
constexpr DWORD ControlCode = 0x0022240C;   // the ATKACPI driver's method call
constexpr uint32_t Dsts = 0x53545344;       // "DSTS": read a device
constexpr uint32_t Devs = 0x53564544;       // "DEVS": write a device
constexpr uint32_t Init = 0x54494E49;       // "INIT"
std::vector<uint8_t> args(uint32_t device, uint32_t value) {
    std::vector<uint8_t> a(8);
    std::memcpy(a.data(), &device, 4); std::memcpy(a.data() + 4, &value, 4);
    return a;
}
std::wstring lower(std::wstring text) { for (auto& c : text) c = wchar_t(std::towlower(c)); return text; }
template<class T> void bind(HMODULE module, const char* name, T& target) {
    FARPROC symbol = module ? GetProcAddress(module, name) : nullptr;
    static_assert(sizeof(symbol) == sizeof(target));
    std::memcpy(&target, &symbol, sizeof(target));
}
HMODULE systemLibrary(const wchar_t* name) { return LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32); }

// What BatteryScope remembers about settings the firmware does not report back.
constexpr wchar_t StoreKey[] = L"Software\\BatteryScope\\Asus";
std::optional<DWORD> load(const wchar_t* name) {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, StoreKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) return value;
    return std::nullopt;
}
void store(const wchar_t* name, DWORD value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, StoreKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
}

// COM on whatever thread calls: joins the thread's apartment when it already has one.
struct Com {
    HRESULT result;
    Com() : result(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~Com() { if (SUCCEEDED(result)) CoUninitialize(); }
    bool ready() const { return SUCCEEDED(result) || result == RPC_E_CHANGED_MODE; }
};
template<class T> struct Ref {
    T* p = nullptr;
    Ref() = default;
    Ref(const Ref&) = delete;
    Ref& operator=(const Ref&) = delete;
    ~Ref() { if (p) p->Release(); }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
    T** put() { if (p) { p->Release(); p = nullptr; } return &p; }
    void** putVoid() { return reinterpret_cast<void**>(put()); }
};
struct Bstr {
    BSTR b;
    explicit Bstr(const wchar_t* text) : b(SysAllocString(text)) {}
    ~Bstr() { SysFreeString(b); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
};
HRESULT connectWmi(Ref<IWbemServices>& wmi, const wchar_t* space = L"ROOT\\WMI") {
    Ref<IWbemLocator> locator;
    HRESULT result = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator, locator.putVoid());
    if (FAILED(result)) return result;
    Bstr root(space);
    result = locator->ConnectServer(root.b, nullptr, nullptr, nullptr, 0, nullptr, nullptr, wmi.put());
    if (FAILED(result)) return result;
    return CoSetProxyBlanket(wmi.p, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
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
Mode nextMode(Mode mode) { return mode == Mode::silent ? Mode::balanced : mode == Mode::balanced ? Mode::turbo : Mode::silent; }

Acpi::Acpi() {
    handle_ = CreateFileW(L"\\\\.\\ATKACPI", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}
Acpi::~Acpi() { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
std::optional<uint32_t> Acpi::call(uint32_t method, const std::vector<uint8_t>& a, std::vector<uint8_t>* raw) const {
    if (handle_ == INVALID_HANDLE_VALUE) return std::nullopt;
    std::vector<uint8_t> in(8 + a.size());
    const uint32_t length = uint32_t(a.size());
    std::memcpy(in.data(), &method, 4); std::memcpy(in.data() + 4, &length, 4);
    if (!a.empty()) std::memcpy(in.data() + 8, a.data(), a.size());
    uint8_t out[16]{}; DWORD returned = 0;
    if (!DeviceIoControl(handle_, ControlCode, in.data(), DWORD(in.size()), out, sizeof(out), &returned, nullptr)) return std::nullopt;
    uint32_t result = 0; std::memcpy(&result, out, 4);
    if (raw) raw->assign(out, out + sizeof(out));
    return result;
}
std::optional<Reading> Acpi::get(uint32_t device) const {
    auto result = call(Dsts, args(device, 0));
    if (!result) return std::nullopt;
    return decode(*result);
}
std::optional<uint32_t> Acpi::set(uint32_t device, uint32_t value) const {
    wchar_t text[48]; swprintf(text, 48, L"acpi 0x%08X=%u", device, value);
    if (stability::refuseWrite(text)) return std::nullopt; // going to sleep, screen off or shutting down
    stability::note(text); // before the write, so a crash right after it is on record
    return call(Devs, args(device, value));
}
std::optional<uint32_t> Acpi::set(uint32_t device, const std::vector<uint8_t>& values) const {
    std::wstring text = L"acpi 0x";
    { wchar_t id[16]; swprintf(id, 16, L"%08X=", device); text += id; }
    for (uint8_t b : values) { wchar_t hex[4]; swprintf(hex, 4, L"%02X", b); text += hex; }
    if (stability::refuseWrite(text)) return std::nullopt;
    stability::note(text);
    std::vector<uint8_t> a(4 + values.size());
    std::memcpy(a.data(), &device, 4);
    if (!values.empty()) std::memcpy(a.data() + 4, values.data(), values.size());
    return call(Devs, a);
}
std::optional<std::vector<uint8_t>> Acpi::getBuffer(uint32_t device, uint32_t argument) const {
    std::vector<uint8_t> raw;
    if (!call(Dsts, args(device, argument), &raw)) return std::nullopt;
    return raw;
}
bool Acpi::init() const { return call(Init, std::vector<uint8_t>(8, 0)).has_value(); }

Remembered remembered() {
    Remembered r;
    if (auto v = load(L"Mode"); v && *v <= 2) r.mode = Mode(*v);
    if (auto v = load(L"ChargeLimit"); v && *v >= 20 && *v <= 100) r.chargeLimit = *v;
    if (auto v = load(L"Keyboard"); v && *v <= 3) r.keyboard = *v;
    if (auto v = load(L"FnLock")) r.fnLock = *v != 0;
    return r;
}

// ----- HID -----
namespace {
// hid.dll, loaded at run time (its headers and import library differ between SDKs).
struct HidCaps { USHORT usage, usagePage, inputLength, outputLength, featureLength, reserved[17], counts[10]; };
struct HidAttributes { ULONG size; USHORT vendor, product, version; };
constexpr size_t ReportCapsSize = 72;       // HIDP_BUTTON_CAPS and HIDP_VALUE_CAPS; the report id is byte 2 of each
enum HidReportType { HidInput = 0, HidFeature = 2 };
struct HidApi {
    BOOLEAN (WINAPI* setFeature)(HANDLE, PVOID, ULONG) = nullptr;
    BOOLEAN (WINAPI* attributes)(HANDLE, HidAttributes*) = nullptr;
    BOOLEAN (WINAPI* preparsed)(HANDLE, void**) = nullptr;
    BOOLEAN (WINAPI* freePreparsed)(void*) = nullptr;
    LONG (WINAPI* caps)(void*, HidCaps*) = nullptr;
    LONG (WINAPI* buttonCaps)(int, void*, USHORT*, void*) = nullptr;
    LONG (WINAPI* valueCaps)(int, void*, USHORT*, void*) = nullptr;
};
constexpr LONG HidSuccess = 0x00110000;
const HidApi* hid() {
    static const HidApi api = [] {
        HidApi a; HMODULE module = systemLibrary(L"hid.dll");
        bind(module, "HidD_SetFeature", a.setFeature); bind(module, "HidD_GetAttributes", a.attributes);
        bind(module, "HidD_GetPreparsedData", a.preparsed); bind(module, "HidD_FreePreparsedData", a.freePreparsed);
        bind(module, "HidP_GetCaps", a.caps); bind(module, "HidP_GetButtonCaps", a.buttonCaps); bind(module, "HidP_GetValueCaps", a.valueCaps);
        return a;
    }();
    return api.setFeature && api.attributes && api.preparsed && api.freePreparsed && api.caps ? &api : nullptr;
}
constexpr uint16_t AsusVendor = 0x0B05;
constexpr uint8_t InputReport = 0x5A, AuraReport = 0x5D, SlashReport = 0x5E;
// Keyboards that take the 0x5A feature reports (G-Helper's ALL_PIDS; 0x19B6 is the 2024 Zephyrus keyboard).
constexpr uint16_t Keyboards[] = {0x1A30, 0x1854, 0x1869, 0x1866, 0x19B6, 0x1822, 0x1837, 0x184A, 0x183D, 0x8502, 0x1807, 0x17E0, 0x1ABE,
    0x1B4C, 0x1B6E, 0x1B2C, 0x8854, 0x1CE7, 0x1CE6, 0x1BF2, 0x1BF3, 0x1CD7, 0x1CD8, 0x4B42, 0x18C6};
struct HidInterface {
    std::wstring path;
    uint16_t product = 0, usagePage = 0, usage = 0, inputLength = 0, featureLength = 0;
    std::vector<uint8_t> inputIds, featureIds;  // report ids the interface declares
    bool feature(uint8_t id) const { return std::find(featureIds.begin(), featureIds.end(), id) != featureIds.end(); }
    bool input(uint8_t id) const { return std::find(inputIds.begin(), inputIds.end(), id) != inputIds.end(); }
};
std::vector<std::wstring> hidPaths() {
    std::vector<std::wstring> paths;
    constexpr GUID HidClass = {0x4D1E55B2, 0xF16F, 0x11CF, {0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};
    HDEVINFO set = SetupDiGetClassDevsW(&HidClass, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return paths;
    SP_DEVICE_INTERFACE_DATA item{}; item.cbSize = sizeof(item);
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &HidClass, i, &item); ++i) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &item, nullptr, 0, &needed, nullptr);
        std::vector<BYTE> buffer(needed + sizeof(wchar_t));
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &item, detail, needed, nullptr, nullptr)) continue;
        if (lower(detail->DevicePath).find(L"vid_0b05") != std::wstring::npos) paths.push_back(detail->DevicePath);
    }
    SetupDiDestroyDeviceInfoList(set);
    return paths;
}
// The report ids of one kind an interface declares (from its button and value capabilities).
std::vector<uint8_t> reportIds(const HidApi& api, void* data, int type, USHORT buttons, USHORT values) {
    std::vector<uint8_t> ids;
    auto collect = [&](LONG (WINAPI* get)(int, void*, USHORT*, void*), USHORT count) {
        if (!get || !count) return;
        std::vector<uint8_t> raw(size_t(count) * ReportCapsSize);
        USHORT length = count;
        if (get(type, raw.data(), &length, data) != HidSuccess) return;
        for (USHORT i = 0; i < length; ++i) {
            const uint8_t id = raw[size_t(i) * ReportCapsSize + 2];
            if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
        }
    };
    collect(api.buttonCaps, buttons); collect(api.valueCaps, values);
    return ids;
}
// ASUS HID interfaces, scanned at most once a minute.
std::vector<HidInterface> hidInterfaces(bool rescan = false) {
    static std::mutex lock;
    static std::vector<HidInterface> cached;
    static uint64_t scannedAt = 0;
    std::lock_guard<std::mutex> guard(lock);
    const uint64_t now = GetTickCount64();
    if (!rescan && scannedAt && now - scannedAt < 60000) return cached;
    cached.clear(); scannedAt = now;
    const HidApi* api = hid();
    if (!api) return cached;
    for (const auto& path : hidPaths()) {
        HANDLE device = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (device == INVALID_HANDLE_VALUE) continue;
        HidInterface item; item.path = path;
        HidAttributes attributes{}; attributes.size = sizeof(attributes);
        if (api->attributes(device, &attributes) && attributes.vendor == AsusVendor) {
            item.product = attributes.product;
            void* data = nullptr;
            if (api->preparsed(device, &data)) {
                HidCaps caps{};
                if (api->caps(data, &caps) == HidSuccess) {
                    item.usagePage = caps.usagePage; item.usage = caps.usage;
                    item.inputLength = caps.inputLength; item.featureLength = caps.featureLength;
                    // counts: link nodes, input buttons, input values, input indices, output (3), feature buttons, feature values, ...
                    item.inputIds = reportIds(*api, data, HidInput, caps.counts[1], caps.counts[2]);
                    item.featureIds = reportIds(*api, data, HidFeature, caps.counts[7], caps.counts[8]);
                }
                api->freePreparsed(data);
            }
        }
        CloseHandle(device);
        if (item.product) cached.push_back(item);
    }
    return cached;
}
bool listed(uint16_t product, const uint16_t* begin, const uint16_t* end) { return std::find(begin, end, product) != end; }
// Interfaces of these products with this feature report and at least this long a feature report.
std::vector<HidInterface> withFeature(const uint16_t* begin, const uint16_t* end, uint8_t reportId, size_t minLength, bool rescan = false) {
    std::vector<HidInterface> found;
    for (const auto& item : hidInterfaces(rescan))
        if (listed(item.product, begin, end) && item.featureLength >= std::max<size_t>(minLength, 1) && item.feature(reportId)) found.push_back(item);
    return found;
}
// Sends the packets, padded to the feature report length, to every interface of these
// products that declares the report (G-Helper's WriteInput). ERROR_SUCCESS when at
// least one interface took them all.
DWORD sendFeature(const uint16_t* begin, const uint16_t* end, uint8_t reportId, size_t minLength, const std::vector<std::vector<uint8_t>>& packets) {
    const HidApi* api = hid();
    if (!api || packets.empty()) return ERROR_NOT_SUPPORTED;
    auto targets = withFeature(begin, end, reportId, minLength);
    if (targets.empty()) targets = withFeature(begin, end, reportId, minLength, true); // after a dock, sleep or driver change
    if (targets.empty()) return ERROR_NOT_SUPPORTED;
    DWORD result = ERROR_GEN_FAILURE;
    for (const auto& item : targets) {
        HANDLE device = CreateFileW(item.path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (device == INVALID_HANDLE_VALUE) continue;
        bool all = true;
        for (const auto& packet : packets) {
            if (packet.size() > item.featureLength) { all = false; break; }
            std::vector<uint8_t> buffer(item.featureLength, 0);
            std::memcpy(buffer.data(), packet.data(), packet.size());
            all = api->setFeature(device, buffer.data(), ULONG(buffer.size())) != FALSE && all;
        }
        CloseHandle(device);
        if (all) result = ERROR_SUCCESS;
    }
    return result;
}
DWORD sendKeyboard(const std::vector<std::vector<uint8_t>>& packets) { return sendFeature(std::begin(Keyboards), std::end(Keyboards), InputReport, 0, packets); }
bool keyboardPresent() { return !withFeature(std::begin(Keyboards), std::end(Keyboards), InputReport, 0).empty(); }
bool rogModel() {
    wchar_t text[128]{}; DWORD bytes = sizeof(text);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"SystemProductName", RRF_RT_REG_SZ, nullptr, text, &bytes) != ERROR_SUCCESS) return false;
    return lower(text).find(L"rog") != std::wstring::npos;
}
}

namespace packet {
std::vector<uint8_t> init(uint8_t reportId) {
    std::vector<uint8_t> p{reportId};
    for (const char* c = "ASUS Tech.Inc."; *c; ++c) p.push_back(uint8_t(*c));
    return p;
}
std::vector<uint8_t> keyboardBrightness(uint8_t reportId, unsigned level) { return {reportId, 0xBA, 0xC5, 0xC4, uint8_t(std::min(3u, level))}; }
std::vector<uint8_t> fnLock(bool on) { return {InputReport, 0xD0, 0x4E, uint8_t(on ? 0x00 : 0x01)}; }
std::vector<uint8_t> touchpad() { return {InputReport, 0xF4, 0x6B}; }
}

Snapshot read() {
    Snapshot s;
    wchar_t text[128]{}; DWORD bytes = sizeof(text);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"SystemProductName", RRF_RT_REG_SZ, nullptr, text, &bytes) == ERROR_SUCCESS)
        s.model = text;
    Acpi acpi;
    s.driver = acpi.available();
    if (!s.driver) return s;
    const Remembered memory = remembered();
    auto get = [&](uint32_t id) -> std::optional<Reading> {
        auto r = acpi.get(id);
        if (r) s.raw.push_back({id, r->raw});
        if (r && r->present) return r;
        return std::nullopt;
    };
    if (auto r = get(device::ThermalPolicy)) {
        // Most models answer this with a meaningless value (the G14 2024 says 3): what was set here counts.
        s.modeDevice = true;
        s.mode = memory.mode ? memory.mode : r->value() <= 2 ? std::optional<Mode>(Mode(r->value())) : std::nullopt;
    } else if (get(device::ThermalPolicyVivo)) s.vivoNumbering = true; // numbered differently: shown raw only
    if (auto r = get(device::GpuEco)) s.gpuEco = r->value() == 1;
    if (auto r = get(device::GpuMux)) s.gpuMux = r->value() == 1;
    if (auto r = get(device::ChargeLimit)) {
        s.chargeDevice = true;
        s.chargeLimit = memory.chargeLimit ? memory.chargeLimit : r->value() >= 20 && r->value() <= 100 ? std::optional<unsigned>(r->value()) : std::nullopt;
    }
    if (auto r = get(device::PanelOverdrive)) s.overdrive = r->value() == 1;
    if (auto r = get(device::CpuFan)) s.cpuFan = r->value();
    if (auto r = get(device::GpuFan)) s.gpuFan = r->value();
    if (auto r = get(device::MidFan)) s.midFan = r->value();
    if (auto r = get(device::CpuTemp)) if (r->value() > 0 && r->value() < 120) s.cpuTemp = r->value();
    if (auto r = get(device::GpuTemp)) if (r->value() > 0 && r->value() < 120) s.gpuTemp = r->value();
    const auto acpiKeyboard = get(device::KeyboardBacklight);
    s.keyboardDevice = acpiKeyboard.has_value() || keyboardPresent();
    if (s.keyboardDevice) s.keyboard = memory.keyboard ? memory.keyboard : acpiKeyboard ? std::optional<unsigned>(std::min(3u, acpiKeyboard->value() & 0x7F)) : std::nullopt;
    s.fnLockHardware = get(device::FnLock).has_value();
    s.fnLock = memory.fnLock.value_or(false);
    get(device::MicMuteLed);
    s.slash = slashPresent();
    const std::pair<uint32_t, const wchar_t*> limits[] = {
        {device::PptSpl, L"SPL（持續）"}, {device::PptSppt, L"SPPT（短時）"}, {device::PptFppt, L"FPPT（瞬間）"},
        {device::PptApuSppt, L"APU SPPT"}, {device::PptPlatformSppt, L"平台 SPPT"},
        {device::NvDynamicBoost, L"獨顯 Dynamic Boost"}, {device::NvTempTarget, L"獨顯溫度目標"}};
    // Limits reading 0 are write-only on this model (G-Helper sets them blind): nothing to show.
    for (const auto& [id, label] : limits) if (auto r = get(id); r && r->value() > 0) s.limits.push_back({label, r->value()});
    return s;
}
std::wstring dump(const Snapshot& s) {
    std::wstring out = L"model=" + s.model + L"\nATKACPI=" + (s.driver ? L"yes" : L"no");
    wchar_t line[128];
    for (const auto& [id, raw] : s.raw) { swprintf(line, 128, L"\n0x%08X = 0x%08X", id, raw); out += line; }
    const Remembered memory = remembered();
    if (memory.mode) out += L"\nremembered mode=" + std::to_wstring(int(*memory.mode));
    if (memory.chargeLimit) out += L"\nremembered charge=" + std::to_wstring(*memory.chargeLimit);
    if (memory.keyboard) out += L"\nremembered keyboard=" + std::to_wstring(*memory.keyboard);
    if (memory.fnLock) out += L"\nremembered fnlock=" + std::to_wstring(int(*memory.fnLock));
    // The firmware's fan curves as it hands them out (8 temperatures, then 8 duties), per fan and mode.
    {
        Acpi acpi;
        const std::pair<uint32_t, const wchar_t*> fans[] = {{0x00110024u, L"cpu"}, {0x00110025u, L"gpu"}};
        for (const auto& [device, name] : fans)
            for (uint32_t fanMode = 0; fanMode < 3; ++fanMode) {
                const auto raw = acpi.available() ? acpi.getBuffer(device, fanMode) : std::nullopt;
                out += std::wstring(L"\nfan curve ") + name + L" mode " + std::to_wstring(fanMode) + L" =";
                if (!raw) { out += L" (no answer)"; continue; }
                for (uint8_t b : *raw) { wchar_t hex[8]; swprintf(hex, 8, L" %02X", b); out += hex; }
            }
    }
    auto ids = [](const std::vector<uint8_t>& list) {
        std::wstring text;
        for (uint8_t id : list) { wchar_t hex[8]; swprintf(hex, 8, L"%02X", id); text += (text.empty() ? L"" : L",") + std::wstring(hex); }
        return text.empty() ? std::wstring(L"-") : text;
    };
    for (const auto& item : hidInterfaces(true)) {
        swprintf(line, 128, L"\nhid pid=%04X usage=%04X:%04X input=%u feature=%u ", item.product, item.usagePage, item.usage, item.inputLength, item.featureLength);
        out += line + std::wstring(L"in[") + ids(item.inputIds) + L"] feat[" + ids(item.featureIds) + L"] " + lower(item.path);
    }
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
// A write the firmware cannot report back: taken on its answer (1: accepted).
DWORD writeBlind(uint32_t device, uint32_t value) {
    Acpi acpi;
    if (!acpi.available()) return ERROR_NOT_SUPPORTED;
    auto before = acpi.get(device);
    if (!before || !before->present) return ERROR_NOT_SUPPORTED;
    const auto answer = acpi.set(device, value);
    if (!answer) return ERROR_GEN_FAILURE;
    return *answer == 0 || *answer == 0xFFFFFFFE || *answer == 0xFFFFFFFF ? ERROR_GEN_FAILURE : ERROR_SUCCESS;
}
}
namespace { void applyAfterMode(Mode mode); }
DWORD setMode(Mode mode) {
    const DWORD status = writeBlind(device::ThermalPolicy, DWORD(mode));
    if (status == ERROR_SUCCESS) { store(L"Mode", DWORD(mode)); applyAfterMode(mode); }
    return status;
}
DWORD setChargeLimit(unsigned percent) {
    if (percent < 20 || percent > 100) return ERROR_INVALID_PARAMETER;
    const DWORD status = writeBlind(device::ChargeLimit, percent);
    if (status == ERROR_SUCCESS) store(L"ChargeLimit", percent);
    return status;
}
DWORD setGpuEco(bool eco) { return writeChecked(device::GpuEco, eco ? 1 : 0, eco ? 1 : 0, 1500); }
DWORD setOverdrive(bool on) { return writeChecked(device::PanelOverdrive, on ? 1 : 0, on ? 1 : 0); }
DWORD setKeyboardBacklight(unsigned level, bool remember) {
    if (level > 3) return ERROR_INVALID_PARAMETER;
    // ROG keyboards take the level as a 0x5A feature report; TUF and Vivobook models
    // through the ACPI device, used here when no keyboard interface took it.
    DWORD status = sendKeyboard({packet::keyboardBrightness(InputReport, level)});
    if (status != ERROR_SUCCESS) {
        const DWORD acpi = writeBlind(device::KeyboardBacklight, 0x80 | level);
        if (acpi != ERROR_NOT_SUPPORTED) status = acpi;
    }
    if (status == ERROR_SUCCESS && remember) store(L"Keyboard", level);
    return status;
}
bool fnLockHardware() {
    Acpi acpi;
    const auto r = acpi.available() ? acpi.get(device::FnLock) : std::nullopt;
    return r && r->present;
}
DWORD setFnLock(bool on) {
    // As G-Helper: the firmware's Fn lock where it has one (with the keyboard told too);
    // elsewhere BatteryScope remaps F1-F11 itself, so only the choice is kept here.
    if (fnLockHardware()) {
        Acpi acpi;
        const bool written = acpi.set(device::FnLock, on ? 1 : 0).has_value();
        const bool told = sendKeyboard({packet::fnLock(on)}) == ERROR_SUCCESS;
        if (!written && !told) return ERROR_GEN_FAILURE;
    }
    store(L"FnLock", on ? 1 : 0);
    return ERROR_SUCCESS;
}
DWORD setMicMuteLight(bool on) {
    Acpi acpi;
    const auto r = acpi.available() ? acpi.get(device::MicMuteLed) : std::nullopt;
    if (!r || !r->present) return ERROR_NOT_SUPPORTED;
    return acpi.set(device::MicMuteLed, on ? 1 : 0) ? ERROR_SUCCESS : ERROR_GEN_FAILURE;
}
DWORD firmwareKey(FirmwareKey key) {
    Acpi acpi;
    if (!acpi.available()) return ERROR_NOT_SUPPORTED;
    return acpi.set(device::UniversalControl, uint32_t(key)) ? ERROR_SUCCESS : ERROR_GEN_FAILURE;
}
// ----- Fan curves and power limits -----
namespace {
constexpr uint32_t CpuFanCurveDevice = 0x00110024, GpuFanCurveDevice = 0x00110025;
constexpr uint32_t PowerSpl = 0x001200A3, PowerSppt = 0x001200A0, PowerFppt = 0x001200C1;
std::atomic<unsigned> tuningGeneration{0};
bool present(uint32_t device) {
    Acpi acpi;
    const auto r = acpi.available() ? acpi.get(device) : std::nullopt;
    return r && r->present;
}
DWORD writeFans(Mode mode, FanPreset preset) {
    if (preset == FanPreset::factory) return ERROR_SUCCESS;
    const Mode source = preset == FanPreset::quiet ? Mode::silent : preset == FanPreset::strong ? Mode::turbo : Mode::balanced;
    (void)mode;
    const auto cpu = factoryFanCurve(false, source), gpu = factoryFanCurve(true, source);
    if (!validFanCurve(cpu)) return ERROR_NOT_SUPPORTED;
    Acpi acpi;
    const auto a = acpi.set(CpuFanCurveDevice, cpu);
    const auto b = validFanCurve(gpu) ? acpi.set(GpuFanCurveDevice, gpu) : std::optional<uint32_t>(1u);
    return a == 1u && b == 1u ? ERROR_SUCCESS : ERROR_GEN_FAILURE;
}
DWORD writePower(PowerPreset preset) {
    const PowerLimits limits = powerLimits(preset);
    if (!limits.spl) return ERROR_SUCCESS;
    Acpi acpi;
    if (!present(PowerSpl)) return ERROR_NOT_SUPPORTED;
    bool ok = acpi.set(PowerSpl, limits.spl) == 1u;
    if (present(PowerSppt)) ok = acpi.set(PowerSppt, limits.sppt) == 1u && ok;
    if (present(PowerFppt)) ok = acpi.set(PowerFppt, limits.fppt) == 1u && ok;
    return ok ? ERROR_SUCCESS : ERROR_GEN_FAILURE;
}
// After a mode change: fans, then power limits, as G-Helper times it. A newer change cancels an older one.
void applyAfterMode(Mode mode) {
    const Tuning t = tuning();
    if (t.fans == FanPreset::factory && t.power == PowerPreset::factory) return;
    const unsigned generation = ++tuningGeneration;
    std::thread([mode, t, generation] {
        Sleep(100);
        if (generation != tuningGeneration) return;
        writeFans(mode, t.fans);
        Sleep(1000);
        if (generation != tuningGeneration) return;
        writePower(t.power);
    }).detach();
}
}
PowerLimits powerLimits(PowerPreset preset) {
    switch (preset) {
    case PowerPreset::saver: return {15, 20, 25};
    case PowerPreset::light: return {25, 30, 35};
    case PowerPreset::standard: return {45, 54, 65};
    default: return {};
    }
}
const wchar_t* fanPresetKey(FanPreset preset) {
    switch (preset) {
    case FanPreset::quiet: return L"安靜";
    case FanPreset::standard: return L"標準";
    case FanPreset::strong: return L"強力";
    default: return L"原廠";
    }
}
Tuning tuning() {
    Tuning t;
    if (auto v = load(L"FanPreset"); v && *v <= DWORD(FanPreset::strong)) t.fans = FanPreset(*v);
    if (auto v = load(L"PowerPreset"); v && *v <= DWORD(PowerPreset::standard)) t.power = PowerPreset(*v);
    return t;
}
bool fanCurvesSupported() { return validFanCurve(factoryFanCurve(false, Mode::balanced)); }
bool powerLimitsSupported() { return present(PowerSpl); }
std::vector<uint8_t> factoryFanCurve(bool gpu, Mode mode) {
    // The firmware numbers fan modes with turbo and silent swapped.
    const uint32_t fanMode = mode == Mode::turbo ? 2 : mode == Mode::silent ? 1 : 0;
    Acpi acpi;
    if (!acpi.available()) return {};
    auto raw = acpi.getBuffer(gpu ? GpuFanCurveDevice : CpuFanCurveDevice, fanMode);
    if (!raw || raw->size() < 16) return {};
    raw->resize(16);
    return validFanCurve(*raw) ? *raw : std::vector<uint8_t>{};
}
bool validFanCurve(const std::vector<uint8_t>& c) {
    if (c.size() != 16 || std::all_of(c.begin(), c.end(), [](uint8_t v) { return v == 0; })) return false;
    for (int i = 0; i < 8; ++i) if (c[size_t(i)] > 110 || c[size_t(i) + 8] > 100) return false; // temperatures in °C, duties in %
    return true;
}
DWORD setTuning(Tuning t) {
    const Tuning before = tuning();
    store(L"FanPreset", DWORD(t.fans)); store(L"PowerPreset", DWORD(t.power));
    ++tuningGeneration; // any pending re-apply is now stale
    const Mode mode = remembered().mode.value_or(Mode::balanced);
    // Back to factory: the G14 2024 keeps custom limits through a re-set of the same mode, so
    // switch to another mode first (G-Helper's workaround), then back; that resets the fans too.
    if ((before.power != PowerPreset::factory && t.power == PowerPreset::factory) || (before.fans != FanPreset::factory && t.fans == FanPreset::factory)) {
        const DWORD other = mode == Mode::turbo ? DWORD(Mode::balanced) : DWORD(Mode::turbo);
        if (writeBlind(device::ThermalPolicy, other) != ERROR_SUCCESS) return ERROR_NOT_SUPPORTED;
        Sleep(1500);
        writeBlind(device::ThermalPolicy, DWORD(mode));
        Sleep(100);
    }
    DWORD status = writeFans(mode, t.fans);
    if (status == ERROR_GEN_FAILURE) writeBlind(device::ThermalPolicy, DWORD(mode)); // refused: back to the mode's own curve
    if (t.power != PowerPreset::factory) {
        Sleep(t.fans != FanPreset::factory ? 1000 : 0);
        const DWORD power = writePower(t.power);
        if (power != ERROR_SUCCESS && status == ERROR_SUCCESS) status = power;
    }
    return status;
}
void reapply(bool mode, bool charge) {
    const Remembered memory = remembered();
    if (mode && memory.mode) setMode(*memory.mode);
    if (charge && memory.chargeLimit) setChargeLimit(*memory.chargeLimit);
    if (memory.keyboard) setKeyboardBacklight(*memory.keyboard);
    if (memory.fnLock) setFnLock(*memory.fnLock);
}

// ----- Slash lighting -----
const std::vector<SlashMode>& slashModes() {
    static const std::vector<SlashMode> modes{SlashMode::bounce, SlashMode::slash, SlashMode::loading, SlashMode::bitStream,
        SlashMode::transmission, SlashMode::flow, SlashMode::flux, SlashMode::phantom, SlashMode::spectrum, SlashMode::hazard,
        SlashMode::interfacing, SlashMode::ramp, SlashMode::gameOver, SlashMode::start, SlashMode::buzzer, SlashMode::staticLight};
    return modes;
}
const wchar_t* slashModeName(SlashMode mode) {
    switch (mode) {
    case SlashMode::staticLight: return L"Static";
    case SlashMode::bounce: return L"Bounce";
    case SlashMode::slash: return L"Slash";
    case SlashMode::loading: return L"Loading";
    case SlashMode::bitStream: return L"BitStream";
    case SlashMode::transmission: return L"Transmission";
    case SlashMode::flow: return L"Flow";
    case SlashMode::flux: return L"Flux";
    case SlashMode::phantom: return L"Phantom";
    case SlashMode::spectrum: return L"Spectrum";
    case SlashMode::hazard: return L"Hazard";
    case SlashMode::interfacing: return L"Interfacing";
    case SlashMode::ramp: return L"Ramp";
    case SlashMode::gameOver: return L"Game Over";
    case SlashMode::start: return L"Start";
    case SlashMode::buzzer: return L"Buzzer";
    }
    return L"?";
}
namespace packet {
// G-Helper's order: wake up, on or off, then (when on) set up, mode, options and save.
std::vector<std::vector<uint8_t>> slash(uint8_t r, const Slash& s) {
    const uint8_t brightness = uint8_t(std::min(3u, std::max(1u, s.brightness)) * 85.333);
    const uint8_t interval = uint8_t(std::min(5u, s.interval));
    std::vector<std::vector<uint8_t>> p;
    p.push_back(init(r));
    p.push_back({r, 0xC2});
    p.push_back({r, 0xD1, 0x01, 0x00, 0x01});
    p.push_back({r, 0xD8, 0x02, 0x00, 0x01, uint8_t(s.on ? 0x00 : 0x80)});
    p.push_back({r, 0xD8, 0x01, 0x00, 0x01, uint8_t(s.offOnBattery ? 0x80 : 0x00)}); // lighting on battery
    if (!s.on) return p;
    p.push_back({r, 0xD7, 0x00, 0x00, 0x01, 0xAC});
    p.push_back({r, 0xD2, 0x02, 0x01, 0x08, 0xAB});
    p.push_back({r, 0xD3, 0x04, 0x00, 0x0C, 0x01, uint8_t(s.mode), 0x02, 0x42, 0x03, 0x13, 0x04, 0x11, 0x05, 0x12, 0x06, 0x13});
    p.push_back({r, 0xD3, 0x03, 0x01, 0x08, 0xAB, 0xFF, 0x01, 0x01, 0x06, brightness, 0xFF, interval});
    p.push_back({r, 0xD4, 0x00, 0x00, 0x01, 0xAB});
    return p;
}
}
Slash slashSettings() {
    Slash s;
    if (auto v = load(L"SlashOn")) { s.on = *v != 0; s.remembered = true; }
    if (auto v = load(L"SlashMode")) {
        const auto& modes = slashModes();
        if (std::any_of(modes.begin(), modes.end(), [&](SlashMode m) { return DWORD(m) == *v; })) s.mode = SlashMode(*v);
    }
    if (auto v = load(L"SlashBrightness"); v && *v >= 1 && *v <= 3) s.brightness = *v;
    if (auto v = load(L"SlashInterval"); v && *v <= 5) s.interval = *v;
    if (auto v = load(L"SlashBattery")) s.offOnBattery = *v != 0;
    return s;
}
namespace {
// G-Helper's detection: the keyboard's 0x5D lighting report on newer models, else the
// separate lighting controller (0x193B) with report 0x5E, both at least 128 bytes long.
constexpr uint16_t SlashKeyboard[] = {0x19B6}, SlashController[] = {0x193B};
}
bool slashPresent() {
    return !withFeature(std::begin(SlashKeyboard), std::end(SlashKeyboard), AuraReport, 128).empty()
        || !withFeature(std::begin(SlashController), std::end(SlashController), SlashReport, 128).empty();
}
DWORD setSlash(const Slash& s) {
    DWORD status = !withFeature(std::begin(SlashKeyboard), std::end(SlashKeyboard), AuraReport, 128).empty()
        ? sendFeature(std::begin(SlashKeyboard), std::end(SlashKeyboard), AuraReport, 128, packet::slash(AuraReport, s))
        : sendFeature(std::begin(SlashController), std::end(SlashController), SlashReport, 128, packet::slash(SlashReport, s));
    if (status != ERROR_SUCCESS) return status;
    store(L"SlashOn", s.on ? 1 : 0); store(L"SlashMode", DWORD(s.mode)); store(L"SlashBrightness", s.brightness);
    store(L"SlashInterval", s.interval); store(L"SlashBattery", s.offOnBattery ? 1 : 0);
    return ERROR_SUCCESS;
}

// ----- What the Fn keys do -----
namespace {
// IAudioEndpointVolume (endpointvolume.h), declared here because some SDKs only forward-declare it.
constexpr GUID EndpointVolumeId = {0x5CDF2C82, 0x841E, 0x4546, {0x97, 0x22, 0x0C, 0xF7, 0x40, 0x78, 0x22, 0x9A}};
struct EndpointVolume : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE RegisterControlChangeNotify(IUnknown*) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnregisterControlChangeNotify(IUnknown*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChannelCount(UINT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMasterVolumeLevel(float, LPCGUID) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMasterVolumeLevelScalar(float, LPCGUID) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMasterVolumeLevel(float*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMasterVolumeLevelScalar(float*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetChannelVolumeLevel(UINT, float, LPCGUID) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetChannelVolumeLevelScalar(UINT, float, LPCGUID) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChannelVolumeLevel(UINT, float*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChannelVolumeLevelScalar(UINT, float*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMute(BOOL, LPCGUID) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMute(BOOL*) = 0;
};
// The first row of a WMI query, or null.
HRESULT firstRow(IWbemServices* wmi, const wchar_t* text, Ref<IWbemClassObject>& row) {
    Ref<IEnumWbemClassObject> rows;
    Bstr language(L"WQL"), query(text);
    HRESULT result = wmi->ExecQuery(language.b, query.b, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, rows.put());
    if (FAILED(result)) return result;
    CoSetProxyBlanket(rows.p, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    ULONG count = 0;
    result = rows->Next(3000, 1, row.put(), &count);
    return FAILED(result) ? result : count ? S_OK : E_FAIL;
}
}
namespace os {
std::optional<int> screenBrightness() {
    Com com;
    if (!com.ready()) return std::nullopt;
    Ref<IWbemServices> wmi; Ref<IWbemClassObject> row;
    if (FAILED(connectWmi(wmi)) || FAILED(firstRow(wmi.p, L"SELECT CurrentBrightness FROM WmiMonitorBrightness WHERE Active = TRUE", row))) return std::nullopt;
    VARIANT value; VariantInit(&value);
    std::optional<int> level;
    if (SUCCEEDED(row->Get(L"CurrentBrightness", 0, &value, nullptr, nullptr))) {
        VARIANT number; VariantInit(&number);
        if (SUCCEEDED(VariantChangeType(&number, &value, 0, VT_I4))) level = number.lVal;
        VariantClear(&number);
    }
    VariantClear(&value);
    return level;
}
DWORD stepScreenBrightness(int percent) {
    const auto now = screenBrightness();
    if (!now) return ERROR_NOT_SUPPORTED;
    return setScreenBrightness(*now + percent);
}
DWORD setScreenBrightness(int level) {
    const int target = std::clamp(level, 0, 100);
    Com com;
    if (!com.ready()) return ERROR_NOT_SUPPORTED;
    Ref<IWbemServices> wmi; Ref<IWbemClassObject> row;
    if (FAILED(connectWmi(wmi)) || FAILED(firstRow(wmi.p, L"SELECT * FROM WmiMonitorBrightnessMethods WHERE Active = TRUE", row))) return ERROR_NOT_SUPPORTED;
    VARIANT path; VariantInit(&path);
    if (FAILED(row->Get(L"__PATH", 0, &path, nullptr, nullptr)) || path.vt != VT_BSTR) { VariantClear(&path); return ERROR_NOT_SUPPORTED; }
    Ref<IWbemClassObject> type, signature, in;
    Bstr className(L"WmiMonitorBrightnessMethods"), method(L"WmiSetBrightness");
    HRESULT result = wmi->GetObject(className.b, 0, nullptr, type.put(), nullptr);
    if (SUCCEEDED(result)) result = type->GetMethod(method.b, 0, signature.put(), nullptr);
    if (SUCCEEDED(result)) result = signature->SpawnInstance(0, in.put());
    if (SUCCEEDED(result)) {
        VARIANT timeout; VariantInit(&timeout); timeout.vt = VT_I4; timeout.lVal = 1;
        VARIANT level; VariantInit(&level); level.vt = VT_UI1; level.bVal = BYTE(target);
        in->Put(L"Timeout", 0, &timeout, 0);
        in->Put(L"Brightness", 0, &level, 0);
        result = wmi->ExecMethod(path.bstrVal, method.b, 0, nullptr, in.p, nullptr, nullptr);
    }
    VariantClear(&path);
    return SUCCEEDED(result) ? ERROR_SUCCESS : DWORD(result);
}
std::optional<bool> toggleMicrophone() {
    Com com;
    if (!com.ready()) return std::nullopt;
    Ref<IMMDeviceEnumerator> devices;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), devices.putVoid()))) return std::nullopt;
    Ref<IMMDevice> main;
    if (FAILED(devices->GetDefaultAudioEndpoint(eCapture, eCommunications, main.put()))
        && FAILED(devices->GetDefaultAudioEndpoint(eCapture, eConsole, main.put()))) return std::nullopt;
    Ref<EndpointVolume> volume; BOOL muted = FALSE;
    if (FAILED(main->Activate(EndpointVolumeId, CLSCTX_ALL, nullptr, volume.putVoid())) || FAILED(volume->GetMute(&muted))) return std::nullopt;
    const BOOL next = !muted;
    // Every microphone, so nothing keeps listening through another input.
    Ref<IMMDeviceCollection> all; UINT count = 0;
    if (SUCCEEDED(devices->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, all.put())) && SUCCEEDED(all->GetCount(&count)))
        for (UINT i = 0; i < count; ++i) {
            Ref<IMMDevice> device; Ref<EndpointVolume> each;
            if (SUCCEEDED(all->Item(i, device.put())) && SUCCEEDED(device->Activate(EndpointVolumeId, CLSCTX_ALL, nullptr, each.putVoid()))) each->SetMute(next, nullptr);
        }
    volume->SetMute(next, nullptr);
    setMicMuteLight(next != FALSE);
    return next != FALSE;
}
void tap(std::initializer_list<WORD> down) {
    std::vector<INPUT> inputs;
    for (WORD vk : down) { INPUT i{}; i.type = INPUT_KEYBOARD; i.ki.wVk = vk; inputs.push_back(i); }
    for (auto it = std::rbegin(down); it != std::rend(down); ++it) { INPUT i{}; i.type = INPUT_KEYBOARD; i.ki.wVk = *it; i.ki.dwFlags = KEYEVENTF_KEYUP; inputs.push_back(i); }
    SendInput(UINT(inputs.size()), inputs.data(), sizeof(INPUT));
}
void toggleTouchpad() {
    if (rogModel() && sendKeyboard({packet::touchpad()}) == ERROR_SUCCESS) return;
    tap({VK_LWIN, VK_LCONTROL, VK_F24});
}
std::optional<bool> touchpadOn() {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\PrecisionTouchPad\\Status", L"Enabled", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return std::nullopt;
    return value != 0;
}
void emojiPanel() { tap({VK_LWIN, VK_OEM_PERIOD}); }
void screenOff(HWND window) { DefWindowProcW(window, WM_SYSCOMMAND, SC_MONITORPOWER, 2); }
bool serviceRunning(const wchar_t* name) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    bool running = false;
    if (SC_HANDLE service = OpenServiceW(scm, name, SERVICE_QUERY_STATUS)) {
        SERVICE_STATUS status{};
        running = QueryServiceStatus(service, &status) && status.dwCurrentState != SERVICE_STOPPED;
        CloseServiceHandle(service);
    }
    CloseServiceHandle(scm);
    return running;
}
bool processRunning(const wchar_t* exe) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    const std::wstring wanted = lower(exe);
    bool found = false;
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot, &entry); more && !found; more = Process32NextW(snapshot, &entry)) found = lower(entry.szExeFile) == wanted;
    CloseHandle(snapshot);
    return found;
}
}

// ----- Services -----
namespace {
constexpr wchar_t SavedKey[] = L"Software\\BatteryScope\\AsusServices";
bool asusService(const std::wstring& name, const std::wstring& display) {
    const std::wstring n = lower(name), d = lower(display);
    for (const wchar_t* word : {L"asus", L"armoury", L"rog live", L"glidex", L"lightingservice", L"ashidservice"})
        if (n.find(word) != std::wstring::npos || d.find(word) != std::wstring::npos) return true;
    return false;
}
// The services G-Helper stops, which ASUS installs to start automatically.
bool managedService(const std::wstring& name) {
    const std::wstring n = lower(name);
    for (const wchar_t* known : {L"armourycratecontrolinterface", L"armourycrateproartservice", L"ashidservice", L"asusoptimization", L"asusappservice",
        L"asuslinknear", L"asuslinkremote", L"asussoftwaremanager", L"asusliveupdateagent", L"asusswitch", L"asussystemanalysis",
        L"asussystemdiagnosis", L"asusxgmobileservice", L"asuscertservice", L"armourycrateseservice", L"armourycrateservice", L"lightingservice"})
        if (n == known) return true;
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
DWORD startOne(SC_HANDLE service) {
    if (StartServiceW(service, 0, nullptr)) return ERROR_SUCCESS;
    const DWORD error = GetLastError();
    return error == ERROR_SERVICE_ALREADY_RUNNING ? ERROR_SUCCESS : error;
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
        s.managed = managedService(s.name);
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
            if (!s.managed) continue; // the touchpad and system control services stay
            Sc service(OpenServiceW(scm.h, s.name.c_str(), SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG | SERVICE_CHANGE_CONFIG));
            if (!service.h) { if (pass) first = first ? first : GetLastError(); continue; }
            if (keepOff && saved) {
                DWORD old = 0, size = sizeof(old);
                if (RegQueryValueExW(saved, s.name.c_str(), nullptr, nullptr, reinterpret_cast<BYTE*>(&old), &size) != ERROR_SUCCESS) {
                    // A service someone else already disabled (G-Helper does) comes back as ASUS installs it.
                    DWORD type = startTypeOf(service.h);
                    if (type == SERVICE_DISABLED) type = managedService(s.name) ? SERVICE_AUTO_START : SERVICE_DEMAND_START;
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
        if (haveSaved && RegQueryValueExW(saved, s.name.c_str(), nullptr, nullptr, reinterpret_cast<BYTE*>(&type), &size) == ERROR_SUCCESS) {
            if (type == SERVICE_DISABLED) type = managedService(s.name) ? SERVICE_AUTO_START : SERVICE_DEMAND_START; // saved while already disabled
            ChangeServiceConfigW(service.h, SERVICE_NO_CHANGE, type, SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        }
        if (type == SERVICE_AUTO_START || type == SERVICE_BOOT_START || type == SERVICE_SYSTEM_START)
            if (const DWORD error = startOne(service.h); error && !first) first = error;
    }
    if (haveSaved) { RegCloseKey(saved); RegDeleteKeyW(HKEY_CURRENT_USER, SavedKey); }
    return first;
}
DWORD restoreServiceDefaults() {
    Sc scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm.h) return GetLastError();
    DWORD first = ERROR_SUCCESS;
    for (int pass = 0; pass < 2; ++pass) // a service waits for another to start first
        for (const auto& s : services()) {
            Sc service(OpenServiceW(scm.h, s.name.c_str(), SERVICE_START | SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG));
            if (!service.h) { if (pass && !first) first = GetLastError(); continue; }
            DWORD type = startTypeOf(service.h);
            if (s.managed || type == SERVICE_DISABLED) {
                type = SERVICE_AUTO_START;
                if (!ChangeServiceConfigW(service.h, SERVICE_NO_CHANGE, type, SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) && pass && !first) first = GetLastError();
            }
            if (type == SERVICE_AUTO_START) if (const DWORD error = startOne(service.h); error && pass && !first) first = error;
        }
    RegDeleteKeyW(HKEY_CURRENT_USER, SavedKey);
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
bool servicesDisabled() {
    for (const auto& s : services()) if (s.startType == SERVICE_DISABLED) return true;
    return false;
}


// ----- Hotkeys -----
bool key::optimization(unsigned code) {
    switch (code) {
    case BrightnessDown: case BrightnessUp: case Touchpad: case Sleep: case ScreenOff: case ScreenOffOld: case Emoji: case Airplane: case Camera: case FnLock: return true;
    default: return false;
    }
}
const wchar_t* keyName(unsigned code) {
    switch (code) {
    case key::BrightnessDown: return L"螢幕亮度調暗（Fn+F7）";
    case key::BrightnessUp: return L"螢幕亮度調亮（Fn+F8）";
    case key::KeyboardUp: return L"鍵盤背光調亮";
    case key::KeyboardDown: return L"鍵盤背光調暗";
    case key::KeyboardCycle: return L"鍵盤背光切換";
    case key::Mode: case key::ModeOld: return L"效能模式鍵（Fn+F5）";
    case key::Rog: return L"ROG 鍵";
    case key::MicMute: return L"麥克風靜音鍵";
    case key::Touchpad: return L"觸控板鍵（Fn+F10）";
    case key::Sleep: return L"睡眠鍵（Fn+F11）";
    case key::Airplane: return L"飛航模式鍵（Fn+F12）";
    case key::Camera: return L"相機鍵";
    case key::ScreenOff: case key::ScreenOffOld: return L"關閉螢幕鍵";
    case key::Emoji: return L"表情符號鍵";
    case key::FnLock: return L"Fn 鎖定（Fn+Esc）";
    case key::FnC: return L"Fn+C";
    case key::Aura: case key::AuraOld: return L"燈效鍵（Fn+F4）";
    case key::Calculator: return L"計算機鍵";
    default: return nullptr;
    }
}
void Hotkeys::start(HWND target, UINT message) {
    if (wmi_.joinable()) return;
    stop_ = false; status_ = S_FALSE; keyboard_ = false;
    wake_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    { Acpi acpi; if (acpi.available()) acpi.init(); } // as G-Helper does before listening
    wmi_ = std::thread([this, target, message] { listenWmi(target, message); });
    hid_ = std::thread([this, target, message] { listenKeyboard(target, message); });
}
void Hotkeys::listenWmi(HWND target, UINT message) {
    Com com;
    if (!com.ready()) { status_ = com.result; return; }
    Ref<IWbemServices> wmi; Ref<IEnumWbemClassObject> events;
    HRESULT result = connectWmi(wmi);
    if (SUCCEEDED(result)) {
        Bstr language(L"WQL"), query(L"SELECT * FROM AsusAtkWmiEvent");
        result = wmi->ExecNotificationQuery(language.b, query.b, WBEM_FLAG_RETURN_IMMEDIATELY | WBEM_FLAG_FORWARD_ONLY, nullptr, events.put());
        if (SUCCEEDED(result)) CoSetProxyBlanket(events.p, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    }
    status_ = SUCCEEDED(result) ? S_OK : result;
    while (events && !stop_) {
        Ref<IWbemClassObject> event; ULONG returned = 0;
        result = events->Next(500, 1, event.put(), &returned); // wakes twice a second to check for stop
        if (result == WBEM_S_TIMEDOUT) continue;
        if (FAILED(result) || !returned) { status_ = FAILED(result) ? result : E_FAIL; break; }
        VARIANT id; VariantInit(&id);
        if (SUCCEEDED(event->Get(L"EventID", 0, &id, nullptr, nullptr))) {
            VARIANT number; VariantInit(&number);
            if (SUCCEEDED(VariantChangeType(&number, &id, 0, VT_UI4))) PostMessageW(target, message, WPARAM(number.ulVal), 0);
            VariantClear(&number);
        }
        VariantClear(&id);
    }
}
// Input reports on the keyboard's vendor interface: [0x5A, key code]. Only ASUS
// Optimization reads them otherwise, which is why the keys go dead once it is stopped.
void Hotkeys::listenKeyboard(HWND target, UINT message) {
    while (!stop_) {
        std::optional<HidInterface> chosen;
        for (const auto& item : withFeature(std::begin(Keyboards), std::end(Keyboards), InputReport, 0, true)) {
            if (item.inputLength < 2) continue;
            if (!chosen || (item.input(InputReport) && !chosen->input(InputReport))) chosen = item;
        }
        HANDLE device = chosen ? CreateFileW(chosen->path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr)
            : INVALID_HANDLE_VALUE;
        if (device == INVALID_HANDLE_VALUE) { WaitForSingleObject(wake_, 10000); continue; } // no keyboard yet: look again
        // The keyboard reports these keys once greeted, as ASUS Optimization (and G-Helper) do.
        if (!os::processRunning(L"asusoptimization.exe")) sendKeyboard({packet::init(InputReport)});
        keyboard_ = true;
        OVERLAPPED overlapped{}; overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::vector<uint8_t> buffer(chosen->inputLength);
        while (!stop_ && overlapped.hEvent) {
            DWORD got = 0;
            if (!ReadFile(device, buffer.data(), DWORD(buffer.size()), &got, &overlapped)) {
                if (GetLastError() != ERROR_IO_PENDING) break;
                HANDLE waits[2] = {overlapped.hEvent, wake_};
                if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) { CancelIo(device); GetOverlappedResult(device, &overlapped, &got, TRUE); break; }
                if (!GetOverlappedResult(device, &overlapped, &got, FALSE)) break;
            }
            if (got > 1 && buffer[0] == InputReport && buffer[1] > 0 && buffer[1] != 236) PostMessageW(target, message, WPARAM(buffer[1]), 1);
        }
        keyboard_ = false;
        if (overlapped.hEvent) CloseHandle(overlapped.hEvent);
        CloseHandle(device);
        if (!stop_) WaitForSingleObject(wake_, 1000); // the keyboard went away (sleep, a driver update): open it again
    }
}
void Hotkeys::stop() {
    stop_ = true;
    if (wake_) SetEvent(wake_);
    if (wmi_.joinable()) wmi_.join();
    if (hid_.joinable()) hid_.join();
    if (wake_) { CloseHandle(wake_); wake_ = nullptr; }
}

std::vector<std::wstring> hidDevices() {
    std::vector<std::wstring> paths;
    for (const auto& path : hidPaths()) paths.push_back(lower(path));
    return paths;
}
}
