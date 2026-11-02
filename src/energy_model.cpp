#include "energy.h"
#include <numeric>
#include <cwctype>
#include <unordered_map>

namespace bs {
Value energyDeltaWatts(uint64_t e0, uint64_t e1, uint64_t t0, uint64_t t1) {
    if (e1 < e0 || t1 <= t0) return {};
    double dt = (t1 - t0) / 1e7;
    if (dt < .1 || dt > 30) return {};
    // EMI: picowatt-hours; timestamps: 100 nanoseconds.
    double watts = double(e1 - e0) * 3.6e-9 / dt;
    if (!std::isfinite(watts) || watts < 0 || watts > 1000) return {};
    return watts;
}
Allocation allocateCpuEnergy(const std::vector<CoreEnergy>& cores, double demand, double observed) {
    Allocation result;
    if (!std::isfinite(demand) || demand <= 0 || !std::isfinite(observed)) return result;
    double power = 0, busy = 0;
    std::vector<std::pair<double,double>> rates;
    for (const auto& core : cores) {
        if (!std::isfinite(core.watts) || !std::isfinite(core.busyLogicalCores) || core.watts < 0 || core.busyLogicalCores <= .00001) continue;
        power += core.watts; busy += core.busyLogicalCores;
        rates.emplace_back(core.watts / core.busyLogicalCores, core.busyLogicalCores);
    }
    if (busy <= 0) return result;
    // Small sampling skew cannot allocate more energy than the hardware supplied.
    const double effectiveDemand = std::clamp(demand * busy / std::max(busy, observed), 0.0, busy);
    result.watts = power * effectiveDemand / busy;
    std::sort(rates.begin(), rates.end());
    auto extreme = [&](bool reverse) {
        double remaining = effectiveDemand, watts = 0;
        for (size_t j = 0; j < rates.size(); ++j) {
            const auto& r = rates[reverse ? rates.size() - 1 - j : j];
            double assigned = std::min(remaining, r.second);
            watts += assigned * r.first; remaining -= assigned;
            if (remaining <= 0) break;
        }
        return watts;
    };
    result.low = std::min(result.watts, extreme(false));
    result.high = std::max(result.watts, extreme(true));
    return result;
}
void alignProcessObservation(CpuEnergyReport& report, double processInterval, uint64_t processSampleMs, bool systemSnapshot) {
    report.processIntervalSeconds = processInterval;
    report.systemProcessSnapshot = systemSnapshot;
    report.sampleSkewMs = report.sampleMs && processSampleMs
        ? double(report.sampleMs > processSampleMs ? report.sampleMs - processSampleMs : processSampleMs - report.sampleMs) : 0;
    if (!report.ready) return;
    const double intervalTolerance = std::max(.12, report.intervalSeconds * .05);
    const double endpointToleranceMs = std::max(75.0, report.intervalSeconds * 50.0);
    if (processInterval <= 0 || std::abs(processInterval - report.intervalSeconds) > intervalTolerance
        || report.sampleSkewMs > endpointToleranceMs) {
        report.timingMismatch = true;
        report.status = L"程序與能源採樣未對齊，本次暫不分攤";
    }
}
void EnergyLedger::update(const std::vector<ProcessRow>& rows, const CpuEnergyReport& report, uint64_t ms) {
    if (!startedMs_) startedMs_ = ms;
    if (ms <= lastMs_) return; // A replayed snapshot must not count twice.
    lastMs_ = ms;
    for (auto& item : items_) item.second.running = false;
    const bool valid = report.ready && !report.timingMismatch && std::isfinite(report.intervalSeconds)
        && report.intervalSeconds > 0 && report.intervalSeconds <= 30;
    if (valid && report.coreWatts && std::isfinite(*report.coreWatts) && *report.coreWatts > 0)
        coreMilliWh_ += *report.coreWatts * report.intervalSeconds / 3.6;
    for (const auto& row : rows) {
        const double add = valid && row.cpuWatts && std::isfinite(*row.cpuWatts) && *row.cpuWatts > 0
            ? *row.cpuWatts * report.intervalSeconds / 3.6 : 0;
        auto found = items_.find({row.pid, row.created});
        if (found == items_.end()) {
            if (add <= 0) continue;
            found = items_.emplace(std::make_pair(row.pid, row.created),
                Item{row.services.size() == 1 ? row.services.front().display : row.name, 0, false}).first;
        }
        found->second.running = true;
        found->second.milliWh += add;
    }
    // Bound memory on long sessions: drop the smallest exited entries.
    if (items_.size() > 600) {
        std::vector<std::pair<double, std::pair<uint32_t,uint64_t>>> exited;
        for (const auto& item : items_) if (!item.second.running) exited.push_back({item.second.milliWh, item.first});
        std::sort(exited.begin(), exited.end());
        for (size_t i = 0; i < exited.size() && items_.size() > 500; ++i) items_.erase(exited[i].second);
    }
}
std::vector<LedgerEntry> EnergyLedger::top(size_t limit) const {
    std::unordered_map<std::wstring, LedgerEntry> byName;
    for (const auto& item : items_) {
        std::wstring key = item.second.name;
        std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
        auto& entry = byName[key];
        if (entry.name.empty()) entry.name = item.second.name;
        entry.milliWh += item.second.milliWh; ++entry.processes; entry.running |= item.second.running;
    }
    std::vector<LedgerEntry> result;
    result.reserve(byName.size());
    for (auto& entry : byName) result.push_back(std::move(entry.second));
    std::sort(result.begin(), result.end(), [](const LedgerEntry& a, const LedgerEntry& b) {
        return a.milliWh != b.milliWh ? a.milliWh > b.milliWh : a.name < b.name;
    });
    if (result.size() > limit) result.resize(limit);
    return result;
}
ObservationQuality classifyObservation(const CpuEnergyReport& report) {
    if (!report.ready || report.timingMismatch) return ObservationQuality::unavailable;
    if (report.systemProcessSnapshot && report.observedCpuCoverage >= .9) return ObservationQuality::strong;
    if (report.observedCpuCoverage >= .65) return ObservationQuality::partial;
    return ObservationQuality::limited;
}
void CpuAttributor::apply(std::vector<ProcessRow>& rows, CpuEnergyReport& report, uint64_t ms, unsigned processors) {
    report.unassignedCpuWatts.reset();
    std::map<std::pair<uint32_t,uint64_t>, History> next;
    double observed = 0, busy = 0, attributed = 0;
    for (const auto& row : rows) if (row.measured) observed += row.cpu / 100 * processors;
    for (const auto& core : report.cores) busy += core.busyLogicalCores;
    report.observedCpuCoverage = busy > 0 ? std::min(1.0, observed / busy) : 0;
    report.timingMismatch = report.timingMismatch || (busy > .01 && observed > busy * 1.15);
    // Known single-core thread constraints reserve that core's budget first.
    // All unmapped execution still shares only the remaining, not the original, budget.
    std::vector<std::vector<double>> fixed(rows.size(),std::vector<double>(report.cores.size(),0));
    std::vector<double> fixedTotals(report.cores.size(),0), fixedPower(rows.size(),0), unknownDemand(rows.size(),0);
    for(size_t i=0;i<rows.size();++i) {
        double demand=rows[i].measured?std::max(0.0,rows[i].cpu/100*processors):0;
        double sum=0;
        for(size_t k=0;k<report.cores.size() && k<rows[i].fixedCoreBusy.size();++k) {
            double value=rows[i].fixedCoreBusy[k];
            if(std::isfinite(value) && value>0) { fixed[i][k]=value; sum+=value; }
        }
        double scale=sum>0?std::min(1.0,demand/sum):0;
        for(size_t k=0;k<report.cores.size();++k) { fixed[i][k]*=scale; fixedTotals[k]+=fixed[i][k]; }
    }
    auto remainder=report.cores;
    for(size_t k=0;k<report.cores.size();++k) {
        double capacity=report.cores[k].busyLogicalCores;
        double scale=fixedTotals[k]>0?std::min(1.0,std::max(0.0,capacity)/fixedTotals[k]):0;
        double used=0;
        for(size_t i=0;i<rows.size();++i) {
            fixed[i][k]*=scale; used+=fixed[i][k];
            if(capacity>.00001) fixedPower[i]+=report.cores[k].watts*fixed[i][k]/capacity;
        }
        double fraction=capacity>.00001?std::clamp(used/capacity,0.0,1.0):0;
        remainder[k].watts*=1-fraction; remainder[k].busyLogicalCores=std::max(0.0,capacity-used);
    }
    double unknownTotal=0;
    for(size_t i=0;i<rows.size();++i) {
        double demand=rows[i].measured?std::max(0.0,rows[i].cpu/100*processors):0;
        double mapped=std::accumulate(fixed[i].begin(),fixed[i].end(),0.0);
        rows[i].placementCoverage=demand>0?std::min(1.0,mapped/demand):0;
        unknownDemand[i]=std::max(0.0,demand-mapped); unknownTotal+=unknownDemand[i];
    }
    // Cycle weighting: among processes that report CPU cycles, split the unknown
    // demand by cycles rather than time, since a second of CPU time at a higher
    // clock uses more energy. The group's total demand is unchanged, and each
    // process moves at most 3x either way to contain counter glitches.
    report.cycleWeighted = false;
    {
        double groupTime = 0, groupCycles = 0; size_t groupSize = 0;
        std::vector<double> cycles(rows.size(), 0);
        for (size_t i = 0; i < rows.size(); ++i) {
            const double demand = rows[i].measured ? std::max(0.0, rows[i].cpu / 100 * processors) : 0;
            if (unknownDemand[i] <= 0 || demand <= 0 || !rows[i].cycleRate || !std::isfinite(*rows[i].cycleRate) || *rows[i].cycleRate <= 0) continue;
            cycles[i] = *rows[i].cycleRate * unknownDemand[i] / demand;
            groupTime += unknownDemand[i]; groupCycles += cycles[i]; ++groupSize;
        }
        if (groupSize >= 2 && groupCycles > 0 && groupTime > 0) {
            std::vector<double> weighted(rows.size(), 0); double weightedSum = 0;
            for (size_t i = 0; i < rows.size(); ++i) if (cycles[i] > 0) {
                weighted[i] = std::clamp(groupTime * cycles[i] / groupCycles, unknownDemand[i] / 3, unknownDemand[i] * 3);
                weightedSum += weighted[i];
            }
            for (size_t i = 0; i < rows.size(); ++i) if (cycles[i] > 0) unknownDemand[i] = weighted[i] * groupTime / weightedSum;
            report.cycleWeighted = true;
        }
    }
    size_t rowIndex=0;
    for (auto& row : rows) {
        row.cpuWatts.reset(); row.averageCpuWatts.reset(); row.cpuWattsLow.reset(); row.cpuWattsHigh.reset(); row.cpuMilliWh.reset();
        auto key = std::make_pair(row.pid, row.created);
        History saved; auto old = history_.find(key);
        if (old != history_.end()) saved = old->second;
        if (report.ready && row.measured && !report.timingMismatch) {
            auto estimate = allocateCpuEnergy(remainder,unknownDemand[rowIndex],unknownTotal);
            estimate.watts+=fixedPower[rowIndex]; estimate.low+=fixedPower[rowIndex]; estimate.high+=fixedPower[rowIndex];
            row.cpuWatts = estimate.watts;
            const double dt = saved.last && ms > saved.last ? (ms - saved.last) / 1000.0 : 0;
            if (saved.last && dt <= 30 && dt > 0) {
                saved.watts = smooth(saved.watts, estimate.watts, dt);
                saved.low = smooth(saved.low, estimate.low, dt);
                saved.high = smooth(saved.high, estimate.high, dt);
            } else { saved.watts = estimate.watts; saved.low = estimate.low; saved.high = estimate.high; }
            if (!saved.last || ms > saved.last) saved.milliWh += estimate.watts * report.intervalSeconds / 3.6;
            row.averageCpuWatts = saved.watts; row.cpuWattsLow = saved.low; row.cpuWattsHigh = saved.high; row.cpuMilliWh = saved.milliWh;
            saved.last = ms; attributed += estimate.watts;
        } else saved.last = 0;
        next[key] = saved;
        ++rowIndex;
    }
    history_ = std::move(next);
    if (report.ready && !report.timingMismatch && report.coreWatts)
        report.unassignedCpuWatts = std::max(0.0, *report.coreWatts - attributed);
    if (report.timingMismatch) report.status = L"採樣時間不同步，本次暫不分攤";
    report.quality = classifyObservation(report);
    std::sort(rows.begin(), rows.end(), [](const ProcessRow& a, const ProcessRow& b) {
        if (a.averageCpuWatts && b.averageCpuWatts) {
            if (*a.averageCpuWatts != *b.averageCpuWatts) return *a.averageCpuWatts > *b.averageCpuWatts;
        } else if (a.averageCpuWatts.has_value() != b.averageCpuWatts.has_value()) return a.averageCpuWatts.has_value();
        return a.score == b.score ? a.pid < b.pid : a.score > b.score;
    });
}
}
