#include "model.h"

namespace bs {
ChargingMetrics calculateCharging(const std::deque<PowerSample>& h) {
    ChargingMetrics result;
    auto valid = [](const PowerSample& p) {
        return p.present && p.acKnown && p.onAc && p.charging && p.watts && std::isfinite(*p.watts) && *p.watts < 0;
    };
    if (h.empty() || !valid(h.back())) return result;
    const auto& last = h.back();
    result.watts = -*last.watts;
    size_t start = h.size() - 1;
    while (start > 0) {
        const auto& a = h[start - 1]; const auto& b = h[start];
        if (!valid(a) || b.ms <= a.ms || b.ms - a.ms > 30000 || last.ms - a.ms > 120000) break;
        if (a.fullWh && b.fullWh && std::abs(*a.fullWh - *b.fullWh) > .01) break;
        if (a.remainingWh && b.remainingWh) {
            double delta = *b.remainingWh - *a.remainingWh;
            double energy = (-*a.watts - *b.watts) * .5 * (b.ms - a.ms) / 3600000.0;
            if (!std::isfinite(delta) || delta < -.001 || delta > std::max(.5, energy * 4)) break;
        }
        --start;
    }
    double energy = 0, squared = 0;
    for (size_t i = start + 1; i < h.size(); ++i) {
        const double dt = (h[i].ms - h[i - 1].ms) / 1000.0;
        const double a = -*h[i - 1].watts, b = -*h[i].watts;
        result.coverageSeconds += dt; energy += (a + b) * .5 * dt;
        squared += (a*a + b*b) * .5 * dt;
    }
    if (result.coverageSeconds < 10) return result;
    result.averageWatts = energy / result.coverageSeconds;
    const double mean = *result.averageWatts;
    if (last.fullWh && std::isfinite(*last.fullWh) && *last.fullWh > 0)
        result.percentPerHour = mean / *last.fullWh * 100;
    if (result.coverageSeconds < 60 || mean < .5 || *result.watts < .5) return result;
    const double variation = std::sqrt(std::max(0.0, squared / result.coverageSeconds - mean * mean)) / mean;
    if (variation > .35 || *result.watts < mean * .5 || *result.watts > mean * 1.5) return result;
    if (last.fullWh && last.remainingWh && std::isfinite(*last.fullWh) && std::isfinite(*last.remainingWh)
        && *last.fullWh > 0 && *last.remainingWh >= 0 && *last.remainingWh < *last.fullWh) {
        // Use the slower of current and recent rates during taper. This remains
        // an at-current-rate estimate; firmware charge limits are not known.
        const double hours = (*last.fullWh - *last.remainingWh) / std::min(mean, *result.watts);
        if (hours <= 72) result.hoursToFull = hours;
    }
    return result;
}
unsigned SamplingPolicy::update(bool background, uint64_t now, Value cpu, Value watts) {
    if (!background) { reset(); return 2; }
    bool valid = cpu && watts && std::isfinite(*cpu) && std::isfinite(*watts) && *cpu >= 0 && *watts >= 0;
    bool stable = valid && lastCpu_ && lastWatts_ && now > lastMs_ && now - lastMs_ <= 30000 &&
        *cpu < 10 && std::abs(*cpu - *lastCpu_) <= 2 &&
        std::abs(*watts - *lastWatts_) <= std::max(.5, *lastWatts_ * .2);
    if (!stable) quietSince_ = 0;
    else if (!quietSince_) quietSince_ = now;
    lastCpu_ = valid ? cpu : Value{}; lastWatts_ = valid ? watts : Value{}; lastMs_ = now;
    return stable && now - quietSince_ >= 60000 ? 20 : 10;
}
bool sameDischargeSession(const PowerSample& a, const PowerSample& b) {
    return a.present && b.present && a.acKnown && b.acKnown && !a.onAc && !b.onAc
        && !a.charging && !b.charging && b.ms > a.ms && b.ms - a.ms <= 30000;
}
void appendPower(std::deque<PowerSample>& h, PowerSample p) {
    if (!h.empty() && p.ms <= h.back().ms) return;
    h.push_back(p);
    while (!h.empty() && p.ms - h.front().ms > 1800000) h.pop_front();
}
PowerMetrics calculatePower(const std::deque<PowerSample>& h) {
    PowerMetrics m;
    if (h.empty()) return m;
    const auto& last = h.back();
    if (!last.present || !last.acKnown || last.onAc || last.charging) return m;
    if (last.watts && *last.watts >= 0 && std::isfinite(*last.watts)) m.currentWatts = last.watts;
    size_t start = h.size() - 1;
    while (start > 0 && sameDischargeSession(h[start - 1], h[start])
           && last.ms - h[start - 1].ms <= 300000) {
        const auto& a = h[start - 1];
        const auto& b = h[start];
        // Capacity increases during discharge usually mean gauge recalibration / battery replacement.
        if (a.remainingWh && b.remainingWh && *b.remainingWh > *a.remainingWh + .001) break;
        if (a.fullWh && b.fullWh && std::abs(*a.fullWh - *b.fullWh) > .01) break;
        if (a.percent && b.percent && *b.percent > *a.percent + .01) break;
        --start;
    }
    double energyWs = 0, covered = 0;
    for (size_t i = start + 1; i < h.size(); ++i) {
        const auto& a = h[i - 1];
        const auto& b = h[i];
        if (a.watts && b.watts && *a.watts >= 0 && *b.watts >= 0
            && std::isfinite(*a.watts) && std::isfinite(*b.watts)) {
            const double dt = (b.ms - a.ms) / 1000.0;
            energyWs += (*a.watts + *b.watts) * .5 * dt;
            covered += dt;
        }
    }
    m.coverageSeconds = covered;
    if (covered >= 10) m.averageWatts = energyWs / covered;
    const auto& first = h[start];
    const double spanHours = (last.ms - first.ms) / 3600000.0;
    if (spanHours >= 120.0 / 3600) {
        if (!m.currentWatts && first.remainingWh && last.remainingWh) {
            const double drop = *first.remainingWh - *last.remainingWh;
            if (drop > 0) {
                m.currentWatts = drop / spanHours;
                m.estimated = true;
                m.averageWatts = m.currentWatts;
                m.coverageSeconds = spanHours * 3600;
            }
        }
        if (first.percent && last.percent && *first.percent > *last.percent)
            m.percentPerHour = (*first.percent - *last.percent) / spanHours;
    }
    if (m.averageWatts && m.coverageSeconds >= 30 && *m.averageWatts > .05) {
        if (last.remainingWh) m.hoursRemaining = *last.remainingWh / *m.averageWatts;
        if (last.fullWh && *last.fullWh > 0) m.percentPerHour = *m.averageWatts / *last.fullWh * 100;
    } else if (m.percentPerHour && *m.percentPerHour > 0 && last.percent) {
        m.hoursRemaining = *last.percent / *m.percentPerHour;
    }
    return m;
}
double cpuPercent(uint64_t before, uint64_t after, double seconds, unsigned processors) {
    if (after < before || seconds <= 0 || processors == 0) return 0;
    return std::clamp((after - before) / 10000000.0 / seconds / processors * 100, 0.0, 100.0);
}
double smooth(double previous, double current, double seconds) {
    return previous + (current - previous) * (1 - std::exp(-std::max(seconds, 0.0) / 30));
}
}
