#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "model.h"
#include <map>

namespace bs {
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE h = nullptr) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    operator HANDLE() const { return value; }
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};
uint64_t fileTime(FILETIME t);
std::wstring errorText(DWORD code);
std::wstring executablePath();
PowerSample readPower();
std::vector<ServiceInfo> readServices(bool& ok);
bool safeProcess(HANDLE process, DWORD pid, std::wstring* reason = nullptr);
DWORD closeProcess(const ProcessRow& expected, bool force);
DWORD stopService(const std::wstring& name, DWORD expectedPid, uint64_t expectedCreated);
bool setSamplerEcoQoS(bool enabled);
// Windows power mode (Settings > System > Power > Power mode), configured
// separately for plugged in and on battery. "other" is an OEM or unknown mode.
enum class PowerMode { efficiency, balanced, performance, other };
PowerMode powerModeFromGuid(const GUID& overlay);
struct PowerModes {
    DWORD error = ERROR_SUCCESS; // ERROR_PROC_NOT_FOUND when Windows lacks the API
    PowerMode ac = PowerMode::other, dc = PowerMode::other;
    bool acKnown = false, dcKnown = false;
    // Windows 11 sets either side at any time; the Windows 10 slider only changes
    // the side that matches the current power source.
    bool acSettable = false, dcSettable = false;
    bool legacy = false;     // Windows 10 slider interface
    bool restorable = false; // a mode BatteryScope changed still differs from its original
};
PowerModes queryPowerModes();
// The first change to each side saves the original so restorePowerModes can undo it.
DWORD setPowerMode(bool pluggedIn, PowerMode mode);
DWORD restorePowerModes();
// The active power plan's display timers on battery: dim after and turn off after (seconds, 0 =
// never) and the dimmed brightness (percent). Windows applies them itself: it keeps the display
// on while a program asks for it (a playing video, a presentation) and brings it back at the
// first input. On Modern Standby laptops a display turned off this way also starts standby.
struct DisplayTimers { DWORD dimSeconds = 0, offSeconds = 0, dimPercent = 50; };
bool readBatteryDisplayTimers(DisplayTimers& timers);
DWORD writeBatteryDisplayTimers(const DisplayTimers& timers);
// Dim before off: a dim time at or past the off time would never show, so it is dropped.
DisplayTimers consistentTimers(DisplayTimers timers);
// The mode Windows applies right now (documented EFFECTIVE_POWER_MODE).
enum class EffectiveMode { unknown, energySaver, efficiency, balanced, performance, maxPerformance, game, mixedReality };
EffectiveMode effectiveModeFromValue(int value);
// The callback runs on a system thread, first with the current mode. Returns
// null if unsupported; pass the token to unwatchEffectivePowerMode.
// Automatic rules change a mode and later undo only their own change, without
// the Restore-original bookkeeping. overridePowerMode returns the overlay it
// replaced; undoPowerModeOverride puts it back only if the side is still at
// `mode` (so a change the user made meanwhile is kept).
DWORD overridePowerMode(bool pluggedIn, PowerMode mode, GUID& replaced);
DWORD undoPowerModeOverride(bool pluggedIn, PowerMode mode, const GUID& replaced);
// Windows 11 "Efficiency mode" for one process, as in Task Manager: EcoQoS
// execution-speed throttling plus idle priority. The same identity and safety
// checks as closing apply. Enabling stores the original priority class in
// `priority`; disabling restores it and hands throttling back to Windows.
DWORD setProcessEfficiency(const ProcessRow& expected, bool enable, DWORD& priority);
bool processEfficiencyOn(DWORD pid);
void* watchEffectivePowerMode(void (*callback)(EffectiveMode, void*), void* context);
void unwatchEffectivePowerMode(void* token);

// Per-process GPU utilization from the Windows "GPU Engine" performance
// counters (the source Task Manager uses). Utilization, not watts.
class GpuUsage {
    void* query_ = nullptr;
    void* counter_ = nullptr;
    bool primed_ = false;
public:
    GpuUsage();
    ~GpuUsage();
    GpuUsage(const GpuUsage&) = delete;
    GpuUsage& operator=(const GpuUsage&) = delete;
    bool available() const { return counter_ != nullptr; }
    // Usage since the previous call, by PID; false until two samples exist.
    bool sample(std::unordered_map<uint32_t, double>& usage);
    void reset() { primed_ = false; instances_.clear(); }
    // Every engine instance of the last sample (name, percent), for per-adapter use.
    const std::vector<std::pair<std::wstring, double>>& instances() const { return instances_; }
private:
    std::vector<std::pair<std::wstring, double>> instances_;
};

struct CollectionWork { size_t paths = 0, safety = 0, memory = 0, processOpens = 0; };

class Collector {
    struct Previous {
        uint64_t created = 0, cpu = 0, io = 0, ms = 0;
        double averageCpu = 0, averageIo = 0, busySeconds = 0;
        bool ioKnown = false;
        uint64_t cycles = 0;
        bool cyclesKnown = false;
    };
    std::map<DWORD, Previous> previous_;
    struct Metadata {
        uint64_t created = 0, pathAt = 0, safetyAt = 0, memoryAt = 0;
        std::wstring path;
        bool safe = false, memoryKnown = false, readable = false;
        double memoryMB = 0;
    };
    std::map<DWORD, Metadata> metadata_;
    bool cacheEnabled_;
    bool forceToolhelp_;
    CollectionWork work_;
    std::vector<ServiceInfo> services_;
    bool servicesKnown_ = false;
    uint64_t servicesAt_ = 0;
    uint64_t sampleMs_ = 0, previousSampleMs_ = 0;
    double intervalSeconds_ = 0, snapshotDurationMs_ = 0;
    bool systemSnapshot_ = false;
    ThreadSnapshot threads_;
    bool threadsCaptured_ = false;
public:
    explicit Collector(bool cacheEnabled = true, bool forceToolhelp = false)
        : cacheEnabled_(cacheEnabled), forceToolhelp_(forceToolhelp) {}
    std::vector<ProcessRow> collect(size_t& total, size_t& inaccessible, bool background = false);
    const CollectionWork& work() const { return work_; }
    uint64_t sampleMs() const { return sampleMs_; }
    double intervalSeconds() const { return intervalSeconds_; }
    double snapshotDurationMs() const { return snapshotDurationMs_; }
    bool systemSnapshot() const { return systemSnapshot_; }
    bool servicesKnown() const { return servicesKnown_; }
    // Thread CPU times captured with the last foreground system snapshot, or null.
    const ThreadSnapshot* threadSnapshot() const { return threadsCaptured_ ? &threads_ : nullptr; }
    void reset() { previous_.clear(); metadata_.clear(); servicesAt_ = sampleMs_ = previousSampleMs_ = 0; intervalSeconds_ = snapshotDurationMs_ = 0; systemSnapshot_ = false; threads_.threads.clear(); threadsCaptured_ = false; }
};
}
