#include "model.h"
#include <cstring>
#include <cwctype>
#include <locale>
#include <iomanip>
#include <sstream>
#include <unordered_map>

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
namespace {
// First index of the continuous discharge segment ending at the last sample,
// at most `window` ms long.
size_t dischargeStart(const std::deque<PowerSample>& h, uint64_t window) {
    const auto& last = h.back();
    size_t start = h.size() - 1;
    while (start > 0 && sameDischargeSession(h[start - 1], h[start]) && last.ms - h[start - 1].ms <= window) {
        const auto& a = h[start - 1];
        const auto& b = h[start];
        // Capacity increases during discharge usually mean gauge recalibration / battery replacement.
        if (a.remainingWh && b.remainingWh && *b.remainingWh > *a.remainingWh + .001) break;
        if (a.fullWh && b.fullWh && std::abs(*a.fullWh - *b.fullWh) > .01) break;
        if (a.percent && b.percent && *b.percent > *a.percent + .01) break;
        --start;
    }
    return start;
}
// Trapezoid-integrated mean of reported discharge watts from `start` to the end.
double meanWatts(const std::deque<PowerSample>& h, size_t start, double& covered) {
    double energyWs = 0; covered = 0;
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
    return covered > 0 ? energyWs / covered : 0;
}
}
PowerMetrics calculatePower(const std::deque<PowerSample>& h) {
    PowerMetrics m;
    if (h.empty()) return m;
    const auto& last = h.back();
    if (!last.present || !last.acKnown || last.onAc || last.charging) return m;
    if (last.watts && *last.watts >= 0 && std::isfinite(*last.watts)) m.currentWatts = last.watts;
    const size_t start = dischargeStart(h, 300000);
    double covered = 0;
    const double shortMean = meanWatts(h, start, covered);
    m.coverageSeconds = covered;
    if (covered >= 10) m.averageWatts = shortMean;
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
    // Range from the 5-minute and up-to-30-minute means: a short burst or lull
    // widens it instead of making a single number jump.
    if (m.hoursRemaining && !m.estimated && m.averageWatts && last.remainingWh) {
        double longCovered = 0;
        const double longMean = meanWatts(h, dischargeStart(h, 1800000), longCovered);
        if (longCovered >= 120 && longMean > .05) {
            m.longAverageWatts = longMean;
            const double fast = std::max(*m.averageWatts, longMean), slow = std::min(*m.averageWatts, longMean);
            m.hoursRemainingLow = *last.remainingWh / fast;
            m.hoursRemainingHigh = *last.remainingWh / slow;
        }
    }
    return m;
}
Value batteryHealth(const PowerSample& p) {
    if (!p.fullWh || !p.designWh || !std::isfinite(*p.fullWh) || !std::isfinite(*p.designWh) || *p.designWh <= 0 || *p.fullWh <= 0) return {};
    return *p.fullWh / *p.designWh * 100;
}
std::optional<int> detectChargeLimit(const std::deque<PowerSample>& h) {
    if (h.empty()) return {};
    const auto& last = h.back();
    auto held = [](const PowerSample& p) {
        return p.present && p.acKnown && p.onAc && !p.charging && p.percent && std::isfinite(*p.percent);
    };
    if (!held(last) || *last.percent < 40 || *last.percent > 96) return {};
    size_t start = h.size() - 1;
    while (start > 0 && held(h[start - 1]) && h[start].ms > h[start - 1].ms && h[start].ms - h[start - 1].ms <= 30000
           && std::abs(*h[start - 1].percent - *last.percent) <= 1) --start;
    if (last.ms - h[start].ms < 180000) return {};
    return int(std::lround(*last.percent));
}
void ChargeCurve::add(double percent, double watts) {
    if (!std::isfinite(percent) || !std::isfinite(watts) || watts <= 0 || percent < 0 || percent >= 100) return;
    const int band = std::clamp(int(percent / 5), 0, Bands - 1);
    // Running mean for the first samples, then a slow exponential average so the
    // curve follows battery aging and different chargers.
    const float weight = count_[band] < 20 ? 1.0f / float(count_[band] + 1) : .05f;
    watts_[band] += (float(watts) - watts_[band]) * weight;
    if (count_[band] < 1000000) ++count_[band];
}
Value ChargeCurve::hoursTo(double percent, double target, double fullWh, double currentWatts) const {
    if (!std::isfinite(percent) || !std::isfinite(target) || !std::isfinite(fullWh) || !std::isfinite(currentWatts)
        || fullWh <= 0 || currentWatts <= 0 || target <= percent || percent < 0 || target > 100) return {};
    constexpr unsigned enough = 6;
    const int here = std::clamp(int(percent / 5), 0, Bands - 1);
    if (count_[here] < enough || watts_[here] <= 0) return {};
    // The current band's learned rate versus the live rate scales the whole curve.
    const double scale = std::clamp(currentWatts / watts_[here], .5, 1.5);
    double hours = 0;
    for (double level = percent; level < target - 1e-9;) {
        const int band = std::clamp(int(level / 5), 0, Bands - 1);
        if (count_[band] < enough || watts_[band] <= 0) return {};
        const double next = std::min(target, (band + 1) * 5.0);
        hours += fullWh * (next - level) / 100 / (watts_[band] * scale);
        level = next;
    }
    return hours <= 72 ? Value(hours) : Value{};
}
std::vector<uint8_t> ChargeCurve::save() const {
    std::vector<uint8_t> bytes(sizeof(watts_) + sizeof(count_) + 1);
    bytes[0] = 1; // format version
    std::memcpy(bytes.data() + 1, watts_, sizeof(watts_));
    std::memcpy(bytes.data() + 1 + sizeof(watts_), count_, sizeof(count_));
    return bytes;
}
bool ChargeCurve::load(const std::vector<uint8_t>& bytes) {
    if (bytes.size() != sizeof(watts_) + sizeof(count_) + 1 || bytes[0] != 1) return false;
    float watts[Bands]; uint32_t count[Bands];
    std::memcpy(watts, bytes.data() + 1, sizeof(watts));
    std::memcpy(count, bytes.data() + 1 + sizeof(watts), sizeof(count));
    for (int i = 0; i < Bands; ++i) if (!std::isfinite(watts[i]) || watts[i] < 0 || watts[i] > 500) return false;
    std::memcpy(watts_, watts, sizeof(watts)); std::memcpy(count_, count, sizeof(count));
    return true;
}
bool LowBatteryMonitor::update(const PowerSample& s, unsigned threshold) {
    if (!threshold || !s.present || !s.acKnown || !s.percent || !std::isfinite(*s.percent)) return false;
    if (s.onAc || s.charging || *s.percent >= threshold + 5.0) { armed_ = true; return false; }
    if (*s.percent <= threshold && armed_) { armed_ = false; return true; }
    return false;
}
std::wstring csvField(const std::wstring& value) {
    std::wstring text = value;
    if (!text.empty() && std::wstring(L"=+-@\t\r").find(text.front()) != std::wstring::npos) text.insert(text.begin(), L'\'');
    if (text.find_first_of(L",\"\r\n") == std::wstring::npos) return text;
    std::wstring quoted = L"\"";
    for (wchar_t c : text) { if (c == L'"') quoted += L'"'; quoted += c; }
    return quoted + L"\"";
}
int roundedMinutes(double hours) {
    if (!std::isfinite(hours) || hours <= 0) return 0;
    return std::max(1, int(std::ceil(hours * 60)));
}
double cpuPercent(uint64_t before, uint64_t after, double seconds, unsigned processors) {
    if (after < before || seconds <= 0 || processors == 0) return 0;
    return std::clamp((after - before) / 10000000.0 / seconds / processors * 100, 0.0, 100.0);
}
double smooth(double previous, double current, double seconds) {
    return previous + (current - previous) * (1 - std::exp(-std::max(seconds, 0.0) / 30));
}
std::vector<size_t> reviewCandidates(const Snapshot& state, size_t limit) {
    std::vector<size_t> result;
    if (!limit || !state.energy.ready || state.energy.timingMismatch || state.energy.observedCpuCoverage < .75)
        return result;
    std::unordered_map<uint32_t, const ProcessRow*> byPid;
    byPid.reserve(state.processes.size());
    for (const auto& row : state.processes) byPid[row.pid] = &row;
    for (size_t i = 0; i < state.processes.size(); ++i) {
        const auto& row = state.processes[i];
        if (!row.actionable || row.window || !row.services.empty() || !row.measured || row.path.empty() ||
            row.busySeconds < 60 || !row.averageCpuWatts || !std::isfinite(*row.averageCpuWatts) ||
            *row.averageCpuWatts < .5) continue;
        bool linkedToForegroundOrService = false;
        uint32_t parentPid = row.parentPid;
        for (unsigned depth = 0; parentPid && depth < 8; ++depth) {
            auto found = byPid.find(parentPid);
            if (found == byPid.end() || found->second->pid == row.pid) break;
            const auto& parent = *found->second;
            if (row.created && parent.created && parent.created > row.created) break; // PID reuse.
            if (parent.window || !parent.services.empty()) { linkedToForegroundOrService = true; break; }
            if (parent.parentPid == parent.pid) break;
            parentPid = parent.parentPid;
        }
        if (!linkedToForegroundOrService) result.push_back(i);
    }
    std::sort(result.begin(), result.end(), [&](size_t a, size_t b) {
        return *state.processes[a].averageCpuWatts > *state.processes[b].averageCpuWatts;
    });
    if (result.size() > limit) result.resize(limit);
    return result;
}
std::optional<SleepRecord> evaluateSleep(const PowerSample& before, const PowerSample& after) {
    auto onBattery = [](const PowerSample& p) { return p.present && p.acKnown && !p.onAc && !p.charging && p.percent && std::isfinite(*p.percent); };
    if (!onBattery(before) || !onBattery(after) || after.ms <= before.ms || after.ms - before.ms < 600000) return {};
    SleepRecord sleep;
    sleep.startMs = before.ms; sleep.endMs = after.ms; sleep.before = *before.percent; sleep.after = *after.percent;
    if (sleep.drop() < -1) return {}; // charged while asleep: not a battery sleep
    return sleep;
}
bool unusualSleepDrain(const SleepRecord& sleep) { return sleep.drop() >= 3 && sleep.ratePerHour() > 1.5; }
Value extraMinutesIfStopped(const PowerMetrics& m, const PowerSample& p, double appWatts) {
    if (!p.present || !p.acKnown || p.onAc || p.charging || !p.remainingWh || !m.averageWatts || !std::isfinite(appWatts)) return {};
    const double average = *m.averageWatts;
    if (average <= .5 || appWatts <= 0 || appWatts >= average * .9) return {};
    const double minutes = (*p.remainingWh / (average - appWatts) - *p.remainingWh / average) * 60;
    return minutes >= 1 ? Value(std::min(minutes, 24.0 * 60)) : Value{};
}
AutoMode decideAutoMode(AutoMode current, bool performanceAppActive, const PowerSample& p, unsigned threshold) {
    if (performanceAppActive) return AutoMode::performance;
    const bool known = p.present && p.acKnown && p.percent && std::isfinite(*p.percent);
    if (!threshold || !known || p.onAc || p.charging) return AutoMode::none;
    if (*p.percent <= threshold) return AutoMode::saver;
    if (current == AutoMode::saver && *p.percent < threshold + 10.0) return AutoMode::saver;
    return AutoMode::none;
}
bool ChargeReminder::update(const PowerSample& p, unsigned target) {
    if (!target || !p.present || !p.acKnown || !p.percent || !std::isfinite(*p.percent)) return false;
    if (!p.onAc || *p.percent < target - 5.0) { armed_ = true; return false; }
    if (armed_ && *p.percent >= target - .5) { armed_ = false; return true; }
    return false;
}
std::optional<size_t> drainAlertCandidate(const Snapshot& state, const std::set<std::pair<uint32_t,uint64_t>>& notified,
    double minWatts, double minSeconds) {
    if (state.history.empty()) return {};
    const auto& p = state.history.back();
    if (!p.present || !p.acKnown || p.onAc || p.charging) return {};
    for (size_t i : reviewCandidates(state, state.processes.size())) {
        const auto& row = state.processes[i];
        if (row.busySeconds >= minSeconds && row.averageCpuWatts && *row.averageCpuWatts >= minWatts && !notified.count({row.pid, row.created}))
            return i;
    }
    return {};
}
bool parseGpuInstance(const std::wstring& name, uint32_t& pid, std::wstring& engine) {
    if (name.compare(0, 4, L"pid_") != 0) return false;
    size_t end = 4;
    while (end < name.size() && iswdigit(name[end])) ++end;
    if (end == 4 || end - 4 > 10) return false;
    const unsigned long long value = std::stoull(name.substr(4, end - 4));
    if (value > 0xFFFFFFFFull) return false;
    const size_t type = name.find(L"_engtype_", end);
    if (type == std::wstring::npos || type + 9 >= name.size()) return false;
    pid = uint32_t(value); engine = name.substr(type + 9);
    return true;
}
std::unordered_map<uint32_t, double> aggregateGpu(const std::vector<std::pair<std::wstring, double>>& instances) {
    std::map<std::pair<uint32_t, std::wstring>, double> perType;
    for (const auto& instance : instances) {
        uint32_t pid = 0; std::wstring engine;
        if (!std::isfinite(instance.second) || instance.second <= 0 || !parseGpuInstance(instance.first, pid, engine) || !pid) continue;
        perType[{pid, engine}] += instance.second;
    }
    std::unordered_map<uint32_t, double> result;
    for (const auto& item : perType) {
        double& value = result[item.first.first];
        value = std::max(value, std::min(100.0, item.second));
    }
    return result;
}
DayStats& DailyHistory::day(int date) {
    if (days_.empty() || days_.back().date < date) {
        DayStats fresh; fresh.date = date;
        days_.push_back(fresh);
        if (days_.size() > KeepDays) days_.erase(days_.begin(), days_.end() - KeepDays);
        return days_.back();
    }
    for (auto it = days_.rbegin(); it != days_.rend(); ++it) if (it->date == date) return *it;
    return days_.back(); // clock moved backwards: keep counting on the newest day
}
void DailyHistory::addSleep(const SleepRecord& sleep, int date) {
    DayStats& today = day(date);
    today.sleepMinutes += sleep.hours() * 60;
    today.sleepDropPercent += std::max(0.0, sleep.drop());
}
void DailyHistory::addPower(const PowerSample& sample, int date) {
    DayStats& today = day(date);
    if (auto health = batteryHealth(sample)) today.healthPercent = health;
    if (sample.cycleCount) today.cycles = sample.cycleCount;
    if (hasLast_ && sample.ms > last_.ms && sample.ms - last_.ms <= 30000) {
        const double dt = (sample.ms - last_.ms) / 1000.0;
        auto valid = [](const Value& w) { return w && std::isfinite(*w); };
        if (sameDischargeSession(last_, sample)) {
            today.batteryMinutes += dt / 60;
            if (valid(last_.watts) && valid(sample.watts) && *last_.watts >= 0 && *sample.watts >= 0)
                today.dischargedWh += (*last_.watts + *sample.watts) / 2 * dt / 3600;
        } else if (last_.charging && sample.charging && last_.onAc && sample.onAc && valid(last_.watts) && valid(sample.watts)
                   && *last_.watts < 0 && *sample.watts < 0) {
            today.chargedWh += -(*last_.watts + *sample.watts) / 2 * dt / 3600;
        }
    }
    last_ = sample; hasLast_ = true;
}
std::vector<std::pair<std::wstring, double>> DailyHistory::topApps() const {
    std::vector<std::pair<std::wstring, double>> apps(todayApps_.begin(), todayApps_.end());
    std::sort(apps.begin(), apps.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
    if (apps.size() > TopApps) apps.resize(TopApps);
    return apps;
}
void DailyHistory::addApps(const std::vector<ProcessRow>& rows, const CpuEnergyReport& report, uint64_t ms, int date) {
    if (ms <= lastAppsMs_) return;
    lastAppsMs_ = ms;
    if (appsDate_ != date) {
        if (appsDate_) day(appsDate_).apps = topApps();
        todayApps_.clear(); appsDate_ = date;
        DayStats& today = day(date);
        for (const auto& app : today.apps) todayApps_[app.first] = app.second; // continue a day loaded from disk
    }
    if (!report.ready || report.timingMismatch || !(report.intervalSeconds > 0 && report.intervalSeconds <= 30)) return;
    for (const auto& row : rows) {
        if (!row.cpuWatts || !std::isfinite(*row.cpuWatts) || *row.cpuWatts <= 0) continue;
        todayApps_[row.services.size() == 1 ? row.services.front().display : row.name] += *row.cpuWatts * report.intervalSeconds / 3.6;
    }
    day(date).apps = topApps();
}
std::vector<DayStats> DailyHistory::recent(size_t count) const {
    const size_t first = days_.size() > count ? days_.size() - count : 0;
    return std::vector<DayStats>(days_.begin() + std::ptrdiff_t(first), days_.end());
}
std::wstring DailyHistory::save() const {
    std::wostringstream out;
    out.imbue(std::locale::classic());
    out << L"date,battery_minutes,discharged_wh,charged_wh,top_apps,sleep_minutes,sleep_drop_pct,health_pct,cycles\n" << std::fixed;
    for (const auto& d : days_) {
        out << d.date << L',' << std::setprecision(1) << d.batteryMinutes << L',' << std::setprecision(3) << d.dischargedWh
            << L',' << d.chargedWh << L',';
        for (size_t i = 0; i < d.apps.size(); ++i) {
            std::wstring name = d.apps[i].first;
            for (auto& c : name) if (c == L',' || c == L'|' || c == L'=' || c == L'\r' || c == L'\n') c = L' ';
            out << (i ? L"|" : L"") << name << L'=' << std::setprecision(1) << d.apps[i].second;
        }
        out << L',' << std::setprecision(1) << d.sleepMinutes << L',' << d.sleepDropPercent << L',';
        if (d.healthPercent) out << *d.healthPercent;
        out << L',';
        if (d.cycles) out << *d.cycles;
        out << L'\n';
    }
    return out.str();
}
bool DailyHistory::load(const std::wstring& text) {
    std::vector<DayStats> days;
    std::wistringstream lines(text);
    std::wstring line;
    auto number = [](const std::wstring& field, double& value) {
        std::wistringstream in(field); in.imbue(std::locale::classic());
        return bool(in >> value) && std::isfinite(value) && value >= 0 && value < 1e7;
    };
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        std::vector<std::wstring> fields;
        size_t from = 0;
        for (int i = 0; i < 4; ++i) {
            const size_t comma = line.find(L',', from);
            if (comma == std::wstring::npos) break;
            fields.push_back(line.substr(from, comma - from)); from = comma + 1;
        }
        if (fields.size() != 4) continue;
        {
            // Remaining columns: top_apps, then optional sleep/health columns (absent in older files).
            const std::wstring rest = line.substr(from);
            size_t start = 0;
            for (;;) {
                const size_t comma = rest.find(L',', start);
                fields.push_back(rest.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start));
                if (comma == std::wstring::npos) break;
                start = comma + 1;
            }
        }
        DayStats d; long date = 0;
        { std::wistringstream in(fields[0]); in.imbue(std::locale::classic()); if (!(in >> date)) continue; }
        if (date < 20000101 || date > 21001231 || !number(fields[1], d.batteryMinutes)
            || !number(fields[2], d.dischargedWh) || !number(fields[3], d.chargedWh)) continue;
        d.date = int(date);
        double extra = 0;
        if (fields.size() > 5 && number(fields[5], extra)) d.sleepMinutes = extra;
        if (fields.size() > 6 && number(fields[6], extra)) d.sleepDropPercent = extra;
        if (fields.size() > 7 && number(fields[7], extra) && extra > 0 && extra < 200) d.healthPercent = extra;
        if (fields.size() > 8 && number(fields[8], extra) && extra >= 1 && extra < 100000) d.cycles = unsigned(extra);
        std::wistringstream apps(fields[4]);
        std::wstring app;
        while (std::getline(apps, app, L'|')) {
            const size_t equals = app.rfind(L'=');
            double energy = 0;
            if (equals == std::wstring::npos || equals == 0 || !number(app.substr(equals + 1), energy)) continue;
            if (d.apps.size() < TopApps) d.apps.push_back({app.substr(0, equals), energy});
        }
        days.push_back(std::move(d));
    }
    std::sort(days.begin(), days.end(), [](const DayStats& a, const DayStats& b) { return a.date < b.date; });
    days.erase(std::unique(days.begin(), days.end(), [](const DayStats& a, const DayStats& b) { return a.date == b.date; }), days.end());
    if (days.size() > KeepDays) days.erase(days.begin(), days.end() - KeepDays);
    *this = DailyHistory();
    days_ = std::move(days);
    return !days_.empty();
}
}
