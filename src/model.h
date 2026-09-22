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
struct ProcessRow {
    uint32_t pid = 0;
    uint64_t created = 0;
    std::wstring name, path, kind, reason;
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
};
// Conservative cadence: never exceed the energy model's 30 second continuity limit.
class SamplingPolicy {
    uint64_t quietSince_ = 0, lastMs_ = 0;
    Value lastCpu_, lastWatts_;
public:
    void reset() { quietSince_ = lastMs_ = 0; lastCpu_.reset(); lastWatts_.reset(); }
    unsigned update(bool background, uint64_t now, Value cpuPercent, Value coreWatts);
};
}
