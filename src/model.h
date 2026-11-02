#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace bs {
using Value = std::optional<double>;
struct PowerSample {
    uint64_t ms = 0;
    bool present = false, acKnown = false, onAc = false, charging = false;
    bool saverKnown = false, saverOn = false;
    Value percent, remainingWh, fullWh, watts; // Positive = discharge; negative = charge.
};
struct PowerMetrics {
    Value currentWatts, averageWatts, percentPerHour, hoursRemaining;
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
    ObservationQuality quality = ObservationQuality::unavailable;
    std::wstring status = L"正在尋找硬體能源計數器";
    std::vector<CoreEnergy> cores;
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
};
// Review candidates are opportunities to inspect, never a safety verdict or automatic action list.
std::vector<size_t> reviewCandidates(const Snapshot& state, size_t limit = 5);
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
// Conservative cadence: never exceed the energy model's 30 second continuity limit.
class SamplingPolicy {
    uint64_t quietSince_ = 0, lastMs_ = 0;
    Value lastCpu_, lastWatts_;
public:
    void reset() { quietSince_ = lastMs_ = 0; lastCpu_.reset(); lastWatts_.reset(); }
    unsigned update(bool background, uint64_t now, Value cpuPercent, Value coreWatts);
};
}
