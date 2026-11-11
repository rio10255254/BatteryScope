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
// The keyboard backlight, Fn lock and the Slash lighting are driven over the
// keyboard's and the lighting's HID interfaces instead, as G-Helper does.
namespace bs::asus {

namespace device {
constexpr uint32_t ThermalPolicy = 0x00120075;       // performance mode: 0 balanced, 1 turbo, 2 silent (write-only on many models)
constexpr uint32_t ThermalPolicyVivo = 0x00110019;   // the same on Vivobook/Zenbook (different numbering)
constexpr uint32_t GpuEco = 0x00090020;              // 1: discrete GPU switched off ("Eco")
constexpr uint32_t GpuMux = 0x00090016;              // 0: discrete GPU drives the screen ("Ultimate"), needs a restart
constexpr uint32_t ChargeLimit = 0x00120057;         // charge stops at this percent (write-only on many models)
constexpr uint32_t PanelOverdrive = 0x00050019;
constexpr uint32_t CpuFan = 0x00110013, GpuFan = 0x00110014, MidFan = 0x00110031;
constexpr uint32_t CpuTemp = 0x00120094, GpuTemp = 0x00120097;
constexpr uint32_t PptSppt = 0x001200A0;             // PL2, short boost limit
constexpr uint32_t PptSpl = 0x001200A3;              // PL1, sustained limit
constexpr uint32_t PptApuSppt = 0x001200B0, PptPlatformSppt = 0x001200B1;
constexpr uint32_t PptFppt = 0x001200C1;             // fast limit
constexpr uint32_t NvDynamicBoost = 0x001200C0, NvTempTarget = 0x001200C2;
constexpr uint32_t CpuFanCurve = 0x00110024, GpuFanCurve = 0x00110025, MidFanCurve = 0x00110032;
constexpr uint32_t KeyboardBacklight = 0x00050021;   // level 0-3; written with bit 7 set (TUF models; ROG uses HID)
constexpr uint32_t FnLock = 0x00100023;              // hardware Fn lock, where the firmware has it (not on ROG)
constexpr uint32_t MicMuteLed = 0x00040017;          // the microphone-mute key's light
constexpr uint32_t UniversalControl = 0x00100021;    // asks the firmware to send Windows a key (see FirmwareKey)
}
// Keys the firmware sends to Windows itself through UniversalControl, so Windows does
// what it does for its own keys (its sleep, its brightness, its airplane mode).
enum class FirmwareKey : uint32_t { brightnessDown = 0x10, brightnessUp = 0x20, sleep = 0x6C, airplane = 0x88 };

// Values as the firmware reports them. DSTS sets bit 16 ("present") on a
// supported device; the value is the low 16 bits. Unsupported devices answer
// 0xFFFFFFFE or 0 without the present bit.
struct Reading { bool present = false; uint32_t raw = 0; uint32_t value() const { return raw & 0xFFFF; } };
Reading decode(uint32_t raw);

enum class Mode { balanced = 0, turbo = 1, silent = 2 };
const wchar_t* modeKey(Mode mode); // Chinese UI string
Mode nextMode(Mode mode);          // the Fn+F5 order: silent → balanced → turbo → silent

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
    bool init() const;   // INIT: what G-Helper calls before listening for keys
private:
    std::optional<uint32_t> call(uint32_t method, const std::vector<uint8_t>& args) const;
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

// The firmware of many models (the G14 2024 among them) accepts the performance
// mode, the charge limit and the keyboard settings but never reports them back.
// Like G-Helper and the Linux driver, BatteryScope remembers what it last set
// (HKCU\Software\BatteryScope\Asus) and sets it again after a restart or sleep.
struct Remembered {
    std::optional<Mode> mode;
    std::optional<unsigned> chargeLimit, keyboard;
    std::optional<bool> fnLock;
};
Remembered remembered();

// Everything readable, for the hardware page and --asus-dump.
struct Snapshot {
    bool driver = false;                    // \\.\ATKACPI opened
    std::wstring model;                     // from SMBIOS (registry)
    bool modeDevice = false;                // the firmware has the performance mode setting
    std::optional<Mode> mode;               // remembered, or reported where the firmware does
    bool vivoNumbering = false;
    std::optional<bool> gpuEco, gpuMux;     // mux true: hybrid (discrete GPU not driving the screen)
    bool chargeDevice = false;              // the firmware has the charge limit setting
    std::optional<unsigned> chargeLimit;    // remembered, or reported where the firmware does
    std::optional<bool> overdrive;
    std::optional<unsigned> cpuFan, gpuFan, midFan;   // as reported (G-Helper shows it times 100 as RPM)
    std::optional<unsigned> cpuTemp, gpuTemp;         // degrees C
    std::vector<std::pair<const wchar_t*, unsigned>> limits; // watts: label, value (limits reading 0 are left out)
    std::vector<std::pair<uint32_t, uint32_t>> raw;           // every device read: id, raw answer
    bool keyboardDevice = false;            // a backlight to drive (ACPI device or the keyboard's HID interface)
    std::optional<unsigned> keyboard;       // backlight level 0-3
    bool fnLockHardware = false;            // the firmware has a Fn lock; otherwise BatteryScope remaps F1-F11 itself
    std::optional<bool> fnLock;
    bool slash = false;                     // Slash lighting found
};
Snapshot read();
std::wstring dump(const Snapshot& snapshot); // plain text for diagnosis

// Writes. Return ERROR_SUCCESS, ERROR_NOT_SUPPORTED when the device is absent, or
// ERROR_GEN_FAILURE when the firmware refused it. Mode and charge limit are
// accepted on the firmware's word (they cannot be read back) and remembered.
DWORD setMode(Mode mode);
DWORD setChargeLimit(unsigned percent);     // 20..100
DWORD setGpuEco(bool eco);                  // checked by reading back
DWORD setOverdrive(bool on);                // checked by reading back
DWORD setKeyboardBacklight(unsigned level, bool remember = true); // 0-3, over HID (ACPI on TUF); remembered unless told not to
bool fnLockHardware();
DWORD setFnLock(bool on);                   // the firmware's Fn lock where there is one; remembered either way
DWORD setMicMuteLight(bool on);             // where the firmware has the light
DWORD firmwareKey(FirmwareKey key);
// Puts back what BatteryScope remembers (mode, charge limit, keyboard, Fn lock):
// after a restart, sleep, or ASUS software having changed it.
void reapply(bool mode, bool charge);

// HID packets, as G-Helper sends them. Each starts with its report id; the
// sender pads it to the interface's feature report length.
namespace packet {
std::vector<uint8_t> init(uint8_t reportId);                  // "ASUS Tech.Inc."
std::vector<uint8_t> keyboardBrightness(uint8_t reportId, unsigned level);
std::vector<uint8_t> fnLock(bool on);
std::vector<uint8_t> touchpad();
}

// Slash lighting (the LED bar on the lid of the Zephyrus G14/G16 2024).
enum class SlashMode : uint8_t {
    staticLight = 0x06, bounce = 0x10, slash = 0x12, loading = 0x13, bitStream = 0x1D, transmission = 0x1A,
    flow = 0x19, flux = 0x25, phantom = 0x24, spectrum = 0x26, hazard = 0x32, interfacing = 0x33,
    ramp = 0x34, gameOver = 0x42, start = 0x43, buzzer = 0x44,
};
const std::vector<SlashMode>& slashModes();
const wchar_t* slashModeName(SlashMode mode); // English names, as ASUS calls them
struct Slash {
    bool on = true;
    SlashMode mode = SlashMode::bounce;
    unsigned brightness = 2;    // 1-3 (G-Helper's dim, medium, bright)
    unsigned interval = 0;      // 0-5: animation pause
    bool offOnBattery = false;  // the firmware turns it off on battery
    bool remembered = false;    // BatteryScope has set it before (otherwise left as ASUS left it)
};
namespace packet {
std::vector<std::vector<uint8_t>> slash(uint8_t reportId, const Slash& settings); // wake, on/off, set up, mode, options, save
}
Slash slashSettings();                      // remembered, or the defaults
bool slashPresent();
DWORD setSlash(const Slash& settings);      // applies and remembers

// What the Fn keys do once the ASUS software is gone, done the Windows way.
namespace os {
DWORD stepScreenBrightness(int percent);    // WMI WmiMonitorBrightnessMethods; call on a worker thread
std::optional<int> screenBrightness();
std::optional<bool> toggleMicrophone();     // mutes or unmutes every microphone; the new state
void toggleTouchpad();                      // the keyboard's own switch on ROG, else Win+Ctrl+F24 (the Windows one)
std::optional<bool> touchpadOn();           // the precision touchpad setting
void emojiPanel();                          // Win+.
void screenOff(HWND window);
void tap(std::initializer_list<WORD> keys); // presses the keys together and lets go
bool serviceRunning(const wchar_t* name);
bool processRunning(const wchar_t* exe);    // case-insensitive, e.g. L"asusoptimization.exe"
}

// ASUS and Armoury Crate services. Stopping and starting them needs administrator
// rights (BatteryScope runs itself elevated for it); their start types are saved
// so starting restores exactly what was there.
// managed: one of the services G-Helper stops (Armoury Crate, ASUS optimization, update and
// link services); the rest (touchpad, system control...) are listed but never stopped.
struct Service { std::wstring name, display; bool running = false, managed = false; DWORD startType = 0; };
std::vector<Service> services();
DWORD stopServices(bool keepOff);   // the managed ones; keepOff: also set them to disabled until started again
DWORD startServices();              // puts back the saved start types
// ASUS's own setup, whatever stopped them (G-Helper sets them to disabled): automatic
// start (manual for Armoury Crate's lighting and AC services, as ASUS installs them) and started now.
DWORD restoreServiceDefaults();
bool servicesKeptOff();             // stopServices(true) was used and not undone
bool servicesDisabled();            // any ASUS service is set to disabled

// The keys ASUS firmware reports itself (Fn+F5, the ROG key, keyboard backlight
// keys...). They come two ways, as G-Helper reads them: WMI events from the ATK
// driver, and input reports (id 0x5A) on the keyboard's own HID interface, which
// only ASUS Optimization reads otherwise. Each key's code is posted to the window
// as `message` with the code in wParam and the source in lParam (0 WMI, 1 HID).
namespace key {
constexpr unsigned BrightnessDown = 0x10, BrightnessUp = 0x20;            // Fn+F7, Fn+F8
constexpr unsigned KeyboardUp = 0xC4, KeyboardDown = 0xC5, KeyboardCycle = 0xC7;
constexpr unsigned Mode = 0xAE, ModeOld = 0x99;                           // Fn+F5
constexpr unsigned Rog = 0x38, MicMute = 0x7C;                            // M4 (ROG key), M3
constexpr unsigned Touchpad = 0x6B, Sleep = 0x6C, Airplane = 0x88, Camera = 0x85;
constexpr unsigned ScreenOff = 0x35, ScreenOffOld = 0x33, Emoji = 0x7E;
constexpr unsigned FnLock = 0x4E, FnC = 0x9E, Aura = 0xB3, AuraOld = 0xB2; // Fn+Esc, Fn+C, Fn+F4
constexpr unsigned Calculator = 0xB5;                                       // Fn+numpad Enter
// Handled by ASUS's optimization service while it runs (BatteryScope steps in once it is stopped).
bool optimization(unsigned code);
}
const wchar_t* keyName(unsigned code); // Chinese UI string, or nullptr for an unknown key
class Hotkeys {
public:
    ~Hotkeys() { stop(); }
    void start(HWND target, UINT message);
    void stop();
    bool running() const { return wmi_.joinable(); }
    // WMI: S_OK once listening; the failing step's HRESULT otherwise; S_FALSE before it has started.
    HRESULT status() const { return status_; }
    bool keyboard() const { return keyboard_; } // reading the keyboard's HID interface
private:
    void listenWmi(HWND target, UINT message);
    void listenKeyboard(HWND target, UINT message);
    std::thread wmi_, hid_;
    std::atomic<bool> stop_{false}, keyboard_{false};
    std::atomic<HRESULT> status_{S_FALSE};
    HANDLE wake_ = nullptr;     // set on stop, ends a pending HID read
};

// HID interfaces of ASUS devices (keyboard, lighting), for diagnosis: "vid_0b05&pid_19b6 ... feature 64".
std::vector<std::wstring> hidDevices();
}
