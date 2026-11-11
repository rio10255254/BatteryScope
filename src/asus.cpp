#include "asus.h"
#include <cstring>

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
}
