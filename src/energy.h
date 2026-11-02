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
};
class CpuAttributor {
    struct History { double watts = 0, low = 0, high = 0, milliWh = 0; uint64_t last = 0; };
    std::map<std::pair<uint32_t,uint64_t>, History> history_;
public:
    void apply(std::vector<ProcessRow>& rows, CpuEnergyReport& report, uint64_t ms, unsigned logicalProcessors);
};
}
