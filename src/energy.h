#pragma once
#include "model.h"
#include <map>
#include <memory>

namespace bs {
struct Allocation {
    double watts = 0, low = 0, high = 0;
};
Value energyDeltaWatts(uint64_t beforeEnergy, uint64_t afterEnergy, uint64_t beforeTime, uint64_t afterTime);
Allocation allocateCpuEnergy(const std::vector<CoreEnergy>& cores, double processBusyCores, double observedBusyCores);
void alignProcessObservation(CpuEnergyReport& report, double processIntervalSeconds,
    uint64_t processSampleMs, bool systemProcessSnapshot);
ObservationQuality classifyObservation(const CpuEnergyReport& report);
// EMI channel names, e.g. RAPL_Package0_Core3_CORE, RAPL_Package0_PP0, RAPL_Package1_PKG.
enum class ChannelKind { other, core, cores, package };
struct ChannelInfo { ChannelKind kind = ChannelKind::other; unsigned package = 0, core = 0; };
ChannelInfo classifyChannel(const std::wstring& name);
// Busy logical processors from two GetSystemTimes readings (idle, kernel incl. idle, user).
Value busyProcessors(uint64_t idle0, uint64_t kernel0, uint64_t user0,
    uint64_t idle1, uint64_t kernel1, uint64_t user1, unsigned logicalProcessors);
// The lowest package power seen over a sliding window: the share of package power
// (uncore, idle, graphics) that is not attributed to processes.
class PackageFloor {
    std::vector<std::pair<uint64_t,double>> samples_; // time ms, watts; newest last
    uint64_t window_;
public:
    explicit PackageFloor(uint64_t windowMs = 600000) : window_(windowMs) {}
    double update(uint64_t ms, double watts);
    void reset() { samples_.clear(); }
};
// Attribution pool for the coreTotal and package sources: one virtual core with
// the given watts and busy processors, plus the range factors.
void setAggregatePool(CpuEnergyReport& report, EnergySource source, double poolWatts, double busy,
    double rangeLow, double rangeHigh);

class EnergyReader {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    EnergyReader();
    ~EnergyReader();
    CpuEnergyReport sample();
    // Uses the collector's thread snapshot when available, which avoids a second
    // system-wide thread enumeration; falls back to Toolhelp otherwise.
    void sampleThreadPlacement(std::vector<ProcessRow>& rows, const ThreadSnapshot* threads = nullptr);
    // Forget thread history, e.g. while hidden placement sampling is skipped.
    void clearThreadPlacement();
    void reset();
    // Every EMI channel found, for diagnostics.
    std::vector<std::wstring> channelNames() const;
};
// Integrates each process's attributed CPU watts over the session and keeps
// exited processes, so short-lived or finished work still counts.
class EnergyLedger {
    struct Item { std::wstring name; double milliWh = 0; bool running = false; };
    std::map<std::pair<uint32_t,uint64_t>, Item> items_;
    double coreMilliWh_ = 0;
    uint64_t lastMs_ = 0, startedMs_ = 0;
public:
    void update(const std::vector<ProcessRow>& rows, const CpuEnergyReport& report, uint64_t ms);
    // Aggregated by executable name (case-insensitive), highest energy first.
    std::vector<LedgerEntry> top(size_t limit) const;
    double coreMilliWh() const { return coreMilliWh_; }
    uint64_t startedMs() const { return startedMs_; }
    size_t tracked() const { return items_.size(); }
};
class CpuAttributor {
    struct History { double watts = 0, low = 0, high = 0, milliWh = 0; uint64_t last = 0; };
    std::map<std::pair<uint32_t,uint64_t>, History> history_;
public:
    void apply(std::vector<ProcessRow>& rows, CpuEnergyReport& report, uint64_t ms, unsigned logicalProcessors);
};
}
