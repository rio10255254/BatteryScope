#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <string>
#include <vector>

namespace bs {
using Value = std::optional<double>;
struct PowerSample {
    uint64_t ms = 0;
    bool present = false, acKnown = false, onAc = false, charging = false;
    bool saverKnown = false, saverOn = false;
    Value percent, remainingWh, fullWh, watts; // Positive = discharge; negative = charge.
    Value designWh;                         // design capacity (sum over batteries)
    std::optional<unsigned> cycleCount;     // single battery with a cycle counter only
};
// Full-charge capacity as a percentage of design capacity; may exceed 100 on new batteries.
Value batteryHealth(const PowerSample& sample);
struct PowerMetrics {
    Value currentWatts, averageWatts, percentPerHour, hoursRemaining;
    // Remaining time at the faster and slower of the 5-minute and up-to-30-minute means.
    Value longAverageWatts, hoursRemainingLow, hoursRemainingHigh;
    bool estimated = false;
    double coverageSeconds = 0;
};
struct ChargingMetrics {
    Value watts, averageWatts, percentPerHour, hoursToFull;
    double coverageSeconds = 0;
};
ChargingMetrics calculateCharging(const std::deque<PowerSample>& history);
bool sameDischargeSession(const PowerSample& a, const PowerSample& b);
PowerMetrics calculatePower(const std::deque<PowerSample>& history);
void appendPower(std::deque<PowerSample>& history, PowerSample sample);
double cpuPercent(uint64_t before, uint64_t after, double seconds, unsigned processors);
double smooth(double previous, double current, double seconds);

struct ServiceInfo {
    std::wstring name, display;
    uint32_t pid = 0;
    bool stoppable = false, shared = false;
};
// Why a process ranks where it does; the UI turns this into localized text.
enum class ActivityReason { limited, warming, low, cpu, io };
struct ProcessRow {
    uint32_t pid = 0;
    uint32_t parentPid = 0;
    uint64_t created = 0;
    std::wstring name, path, kind;
    ActivityReason activity = ActivityReason::warming;
    double cpu = 0, averageCpu = 0, ioMB = 0, averageIo = 0, memoryMB = 0, score = 0;
    Value cycleRate;  // CPU cycles per second over the last interval, when Windows reports them
    Value gpuPercent; // busiest GPU engine utilization, panel visible only
    bool measured = false, ioKnown = false, memoryKnown = false, window = false;
    bool actionable = false;
    double busySeconds = 0;
    double memoryAgeSeconds = 0;
    // Hardware-constrained CPU energy attribution, never total process power.
    Value cpuWatts, averageCpuWatts, cpuWattsLow, cpuWattsHigh, cpuMilliWh;
    std::vector<double> fixedCoreBusy; // Only threads whose observed affinity stays inside one physical core.
    double placementCoverage = 0;
    std::vector<ServiceInfo> services;
};
// Per-thread CPU time from the same system snapshot as the process rows.
// Threads of one process are stored contiguously, in snapshot order.
struct ThreadSample {
    uint32_t pid = 0, tid = 0;
    uint64_t created = 0, cpu = 0;
};
struct ThreadSnapshot {
    uint64_t sampleMs = 0;
    std::vector<ThreadSample> threads;
};
struct CoreEnergy {
    double watts = 0;
    double busyLogicalCores = 0;
};
enum class ObservationQuality { unavailable, limited, partial, strong };
struct CpuEnergyReport {
    Value coreWatts, packageWatts, otherSystemWatts, unassignedCpuWatts;
    size_t channels = 0;
    double intervalSeconds = 0;
    double processIntervalSeconds = 0, sampleSkewMs = 0;
    double observedCpuCoverage = 0;
    uint64_t sampleMs = 0;
    bool ready = false, timingMismatch = false, systemProcessSnapshot = false;
    bool cycleWeighted = false; // unknown-core demand split by CPU cycles instead of time
    ObservationQuality quality = ObservationQuality::unavailable;
    std::wstring status = L"正在尋找硬體能源計數器";
    std::vector<CoreEnergy> cores;
};
// Daily totals kept on disk for the history page (local time, newest last).
struct DayStats {
    int date = 0; // yyyymmdd
    double batteryMinutes = 0, dischargedWh = 0, chargedWh = 0;
    std::vector<std::pair<std::wstring, double>> apps; // top apps by CPU mWh, highest first
    double sleepMinutes = 0, sleepDropPercent = 0;      // sleep on battery, counted on the wake day
    Value healthPercent;                                 // last battery health seen that day
    std::optional<unsigned> cycles;
};
// One sleep, measured from the last battery sample before a sampling gap and
// the first one after it (system sleep, Modern Standby, or a suspended app).
struct SleepRecord {
    uint64_t startMs = 0, endMs = 0;
    double before = 0, after = 0; // battery percent
    double hours() const { return (endMs - startMs) / 3600000.0; }
    double drop() const { return before - after; }
    double ratePerHour() const { return hours() > 0 ? drop() / hours() : 0; }
};
// A sleep worth recording: at least 10 minutes, on battery on both sides, levels known.
std::optional<SleepRecord> evaluateSleep(const PowerSample& before, const PowerSample& after);
// High standby drain: at least 3 points lost at more than 1.5 points per hour.
bool unusualSleepDrain(const SleepRecord& sleep);
// Minutes of battery gained if `appWatts` of the current drain stopped, at the
// 5-minute average; empty when plugged in or the gain is under a minute.
Value extraMinutesIfStopped(const PowerMetrics& power, const PowerSample& sample, double appWatts);
// Fires once when plugged in and the level reaches `target`; re-arms when
// unplugged or 5 points below the target.
class ChargeReminder {
    bool armed_ = true;
public:
    void reset() { armed_ = true; }
    bool update(const PowerSample& sample, unsigned target);
};
// Session CPU energy for one executable name, summed over its processes,
// including processes that have already exited.
struct LedgerEntry {
    std::wstring name;
    double milliWh = 0;
    unsigned processes = 0;
    bool running = false;
};
struct Snapshot {
    std::deque<PowerSample> history;
    PowerMetrics power;
    CpuEnergyReport energy;
    std::vector<ProcessRow> processes;
    size_t totalProcesses = 0, inaccessible = 0;
    bool servicesKnown = false;
    uint64_t ms = 0;
    unsigned samplingSeconds = 2;
    bool samplerEcoQoS = false;
    std::vector<LedgerEntry> ledger;
    double ledgerCoreMilliWh = 0;
    uint64_t ledgerStartedMs = 0;
    std::vector<DayStats> days; // last 7 days of history, newest last
    std::optional<SleepRecord> lastSleep;
    unsigned sleepCount = 0;    // increments with each detected sleep
    bool gpuKnown = false;      // GPU counters sampled for this process list
};
// Review candidates are opportunities to inspect, never a safety verdict or automatic action list.
std::vector<size_t> reviewCandidates(const Snapshot& state, size_t limit = 5);
// A firmware charge limit (battery care): plugged in, not charging, and the
// level steady (within 1 point) between 40% and 96% for at least 3 minutes.
// Returns the rounded level; it may also be a pause for heat or a weak charger.
std::optional<int> detectChargeLimit(const std::deque<PowerSample>& history);
// Learned charging rate per 5% band, so time-to-full follows this machine's
// taper instead of assuming the current rate holds to 100%.
class ChargeCurve {
public:
    static constexpr int Bands = 20;
    void add(double percent, double watts);
    // Hours to charge from `percent` to `target`, with the learned shape scaled
    // to `currentWatts`; empty until every band on the way has enough samples.
    Value hoursTo(double percent, double target, double fullWh, double currentWatts) const;
    std::vector<uint8_t> save() const;
    bool load(const std::vector<uint8_t>& bytes);
    unsigned samples(int band) const { return band >= 0 && band < Bands ? count_[band] : 0; }
private:
    float watts_[Bands] = {};
    uint32_t count_[Bands] = {};
};
// Fires once when a discharging battery reaches the threshold (percent). It
// re-arms on external power or after recovering five points above the threshold.
class LowBatteryMonitor {
    bool armed_ = true;
public:
    void reset() { armed_ = true; }
    bool update(const PowerSample& sample, unsigned threshold);
};
// One RFC 4180 CSV field. Text that a spreadsheet would treat as a formula is
// prefixed with an apostrophe, since process names and paths are untrusted.
std::wstring csvField(const std::wstring& value);
// Whole minutes for a duration in hours, rounded up and at least 1; 0 if invalid.
int roundedMinutes(double hours);
// A background process worth a notification: on battery, a review candidate
// (see reviewCandidates) that has stayed busy for `minSeconds` at `minWatts` or
// more (30 s average), and has not been reported yet.
std::optional<size_t> drainAlertCandidate(const Snapshot& state, const std::set<std::pair<uint32_t,uint64_t>>& notified,
    double minWatts = 2, double minSeconds = 600);

// "pid_1234_luid_0x0_0x0_phys_0_eng_0_engtype_3D" -> pid 1234, engine "3D".
bool parseGpuInstance(const std::wstring& name, uint32_t& pid, std::wstring& engine);
// Per-process GPU use as Task Manager shows it: utilization summed per engine
// type, then the busiest type, capped at 100.
std::unordered_map<uint32_t, double> aggregateGpu(const std::vector<std::pair<std::wstring, double>>& instances);

class DailyHistory {
public:
    static constexpr size_t KeepDays = 60, TopApps = 5;
    void addPower(const PowerSample& sample, int date);
    void addApps(const std::vector<ProcessRow>& rows, const CpuEnergyReport& report, uint64_t ms, int date);
    void addSleep(const SleepRecord& sleep, int date);
    std::vector<DayStats> recent(size_t days) const;
    std::wstring save() const;
    // Replaces the history with the parsed days; malformed lines are skipped. False if none parsed.
    bool load(const std::wstring& text);
    void clear() { *this = DailyHistory(); }
private:
    DayStats& day(int date);
    std::vector<std::pair<std::wstring, double>> topApps() const;
    std::vector<DayStats> days_;
    std::map<std::wstring, double> todayApps_;
    int appsDate_ = 0;
    PowerSample last_;
    bool hasLast_ = false;
    uint64_t lastAppsMs_ = 0;
};

// Automatic power mode rules. Performance while a listed app runs takes
// precedence; otherwise "saver" (on-battery mode set to Efficiency) once on
// battery at or below the threshold, until external power or 10 points above it.
enum class AutoMode { none, saver, performance };
AutoMode decideAutoMode(AutoMode current, bool performanceAppActive, const PowerSample& sample, unsigned saverThreshold);
// Conservative cadence: never exceed the energy model's 30 second continuity limit.
class SamplingPolicy {
    uint64_t quietSince_ = 0, lastMs_ = 0;
    Value lastCpu_, lastWatts_;
public:
    void reset() { quietSince_ = lastMs_ = 0; lastCpu_.reset(); lastWatts_.reset(); }
    unsigned update(bool background, uint64_t now, Value cpuPercent, Value coreWatts);
};
}
