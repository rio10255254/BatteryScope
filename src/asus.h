#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <thread>
#include <optional>
#include <string>
#include <vector>

// ASUS laptop firmware settings through the ATKACPI driver (\\.\ATKACPI), the
// interface Armoury Crate uses (and G-Helper, and the Linux asus-wmi driver):
// DSTS reads a "device" (a numbered firmware setting or sensor), DEVS writes one.
// Device numbers below are the ones the Linux kernel documents (asus-wmi.h).
namespace bs::asus {

namespace device {
constexpr uint32_t ThermalPolicy = 0x00120075;       // performance mode: 0 balanced, 1 turbo, 2 silent
constexpr uint32_t ThermalPolicyVivo = 0x00110019;   // the same on Vivobook/Zenbook (different numbering)
constexpr uint32_t GpuEco = 0x00090020;              // 1: discrete GPU switched off ("Eco")
constexpr uint32_t GpuMux = 0x00090016;              // 0: discrete GPU drives the screen ("Ultimate"), needs a restart
constexpr uint32_t ChargeLimit = 0x00120057;         // charge stops at this percent
constexpr uint32_t PanelOverdrive = 0x00050019;
constexpr uint32_t CpuFan = 0x00110013, GpuFan = 0x00110014, MidFan = 0x00110031;
constexpr uint32_t CpuTemp = 0x00120094, GpuTemp = 0x00120097;
constexpr uint32_t PptSppt = 0x001200A0;             // PL2, short boost limit
constexpr uint32_t PptSpl = 0x001200A3;              // PL1, sustained limit
constexpr uint32_t PptApuSppt = 0x001200B0, PptPlatformSppt = 0x001200B1;
constexpr uint32_t PptFppt = 0x001200C1;             // fast limit
constexpr uint32_t NvDynamicBoost = 0x001200C0, NvTempTarget = 0x001200C2;
constexpr uint32_t CpuFanCurve = 0x00110024, GpuFanCurve = 0x00110025, MidFanCurve = 0x00110032;
constexpr uint32_t KeyboardBacklight = 0x00050021;   // level 0-3; written with bit 7 set
}

// Values as the firmware reports them. DSTS sets bit 16 ("present") on a
// supported device; the value is the low 16 bits. Unsupported devices answer
// 0xFFFFFFFE or 0 without the present bit.
struct Reading { bool present = false; uint32_t raw = 0; uint32_t value() const { return raw & 0xFFFF; } };
Reading decode(uint32_t raw);

enum class Mode { balanced = 0, turbo = 1, silent = 2 };
const wchar_t* modeKey(Mode mode); // Chinese UI string

class Acpi {
public:
    Acpi();
    ~Acpi();
    Acpi(const Acpi&) = delete;
    Acpi& operator=(const Acpi&) = delete;
    bool available() const { return handle_ != INVALID_HANDLE_VALUE; }
    std::optional<Reading> get(uint32_t device) const;      // nullopt: the call itself failed
    // DEVS; returns the firmware's answer (1 usually means accepted), nullopt when the call failed.
    std::optional<uint32_t> set(uint32_t device, uint32_t value) const;
private:
    std::optional<uint32_t> call(uint32_t method, const std::vector<uint8_t>& args) const;
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

// Everything readable, for the hardware page and --asus-dump.
struct Snapshot {
    bool driver = false;                    // \\.\ATKACPI opened
    std::wstring model;                     // from SMBIOS (registry)
    std::optional<Mode> mode;
    bool vivoNumbering = false;
    std::optional<bool> gpuEco, gpuMux;     // mux true: hybrid (discrete GPU not driving the screen)
    std::optional<unsigned> chargeLimit;
    std::optional<bool> overdrive;
    std::optional<unsigned> cpuFan, gpuFan, midFan;   // as reported (G-Helper shows it times 100 as RPM)
    std::optional<unsigned> cpuTemp, gpuTemp;         // degrees C
    std::vector<std::pair<const wchar_t*, unsigned>> limits; // watts: label, value
    std::vector<std::pair<uint32_t, uint32_t>> raw;           // every device read: id, raw answer
    std::optional<unsigned> keyboard;                         // backlight level 0-3
};
Snapshot read();
std::wstring dump(const Snapshot& snapshot); // plain text for diagnosis

// Writes, each checked by reading back. Return ERROR_SUCCESS, ERROR_NOT_SUPPORTED
// when the device is absent, or ERROR_GEN_FAILURE when the firmware refused it.
DWORD setMode(Mode mode);
DWORD setChargeLimit(unsigned percent);     // 20..100
DWORD setGpuEco(bool eco);
DWORD setOverdrive(bool on);
DWORD setKeyboardBacklight(unsigned level); // 0-3

// ASUS and Armoury Crate services. Stopping and starting them needs administrator
// rights (BatteryScope runs itself elevated for it); their start types are saved
// so starting restores exactly what was there.
struct Service { std::wstring name, display; bool running = false; DWORD startType = 0; };
std::vector<Service> services();
DWORD stopServices(bool keepOff);   // keepOff: also set them to disabled until started again
DWORD startServices();
bool servicesKeptOff();             // stopServices(true) was used and not undone

// The keys ASUS firmware reports itself (Fn+F5, the ROG key, keyboard backlight
// keys...), as WMI events from the ATK driver. Each key's code is posted to the
// window as `message` with the code in wParam.
namespace key {
constexpr unsigned KeyboardUp = 0xC4, KeyboardDown = 0xC5, Mode = 0xAE, Rog = 0x38, MicMute = 0x7C, Touchpad = 0x6B;
}
const wchar_t* keyName(unsigned code); // Chinese UI string, or nullptr for an unknown key
class Hotkeys {
public:
    ~Hotkeys() { stop(); }
    void start(HWND target, UINT message);
    void stop();
    bool running() const { return thread_.joinable(); }
private:
    std::thread thread_;
    std::atomic<bool> stop_{false};
};

// HID interfaces of ASUS devices (keyboard, lighting), for diagnosis: "VID_0B05&PID_19B6 usage FF31:0076".
std::vector<std::wstring> hidDevices();
}
