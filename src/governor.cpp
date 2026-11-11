#include "governor.h"
#include <algorithm>

namespace bs::gov {
namespace {
constexpr size_t Settings = size_t(Setting::count);
Activity derive(const Input& in) {
    switch (in.context) {
    case lab::Context::game: return Activity::game;
    case lab::Context::call: return Activity::call;
    case lab::Context::media: return Activity::media;
    case lab::Context::heavy: return Activity::heavy;
    case lab::Context::idle: return Activity::idle;
    default:
        if (in.idleSeconds < Governor::ResponsiveSeconds) return Activity::responsive;
        if (in.idleSeconds >= Governor::ReadingAfterSeconds) return Activity::reading;
        return Activity::interactive;
    }
}
// The next more saving activity, used when the battery will not last to the target.
Activity stretched(Activity a) {
    switch (a) {
    case Activity::burst: return Activity::responsive;
    case Activity::heavy: case Activity::responsive: return Activity::interactive;
    case Activity::exempt: return a;
    case Activity::interactive: return Activity::reading;
    case Activity::reading: case Activity::media: return Activity::idle;
    default: return a;
    }
}
constexpr DWORD Efficiency = 0, Balanced = 1, Performance = 2; // PowerMode
// Values parsed from the lab's arm names, which BatteryScope itself writes.
std::optional<DWORD> armValue(Setting setting, const std::wstring& name) {
    auto number = [&]() -> std::optional<DWORD> {
        size_t at = name.find_first_of(L"0123456789");
        if (at == std::wstring::npos) return std::nullopt;
        return DWORD(std::wcstoul(name.c_str() + at, nullptr, 10));
    };
    switch (setting) {
    case Setting::powerMode:
        if (name == L"效率") return Efficiency;
        if (name == L"平衡") return Balanced;
        if (name == L"效能") return Performance;
        return std::nullopt;
    case Setting::cpuBoost:
        if (name == L"Boost 關") return 0u;
        if (name == L"Boost 開") return 2u;
        return std::nullopt;
    case Setting::energyPreference: case Setting::refreshRate: return number();
    default: return std::nullopt;
    }
}
std::vector<Activity> activitiesFor(const std::wstring& workload) {
    if (workload == L"browse" || workload == L"scroll") return {Activity::interactive, Activity::reading, Activity::responsive};
    if (workload == L"office" || workload == L"bursts") return {Activity::interactive, Activity::responsive};
    if (workload == L"video") return {Activity::media};
    if (workload == L"idle") return {Activity::idle, Activity::reading};
    if (workload == L"sustained") return {Activity::heavy};
    return {};
}
}

lab::Knob knobFor(Setting s) {
    switch (s) {
    case Setting::powerMode: return lab::Knob::powerMode;
    case Setting::energyPreference: return lab::Knob::energyPreference;
    case Setting::cpuBoost: return lab::Knob::cpuBoost;
    default: return lab::Knob::refreshRate;
    }
}

std::vector<Evidence> learn(const std::vector<lab::ExperimentResult>& results) {
    std::vector<Evidence> evidence;
    for (const auto& r : results) {
        const size_t at = r.experiment.find(L'@');
        if (at == std::wstring::npos || r.battery.pairs < 2) continue;
        // The battery reading is noisy (screen, Wi-Fi, other apps), so a clear CPU core
        // difference in the same direction also counts for the settings that act on the CPU.
        const bool coreClear = r.core.significant() && r.core.difference * r.battery.difference > 0;
        if (!r.battery.significant() && !coreClear) continue;
        const std::wstring knob = r.experiment.substr(0, at), workload = r.experiment.substr(at + 1);
        std::optional<Setting> setting;
        if (knob == L"power-mode") setting = Setting::powerMode;
        else if (knob == L"epp") setting = Setting::energyPreference;
        else if (knob == L"cpu-boost") setting = Setting::cpuBoost;
        else if (knob == L"refresh") setting = Setting::refreshRate;
        if (!setting) continue;
        // Which arm used less power, and what choosing it costs.
        const bool secondSaves = r.battery.difference < 0;
        const auto value = armValue(*setting, secondSaves ? r.arm1 : r.arm0);
        if (!value) continue;
        const double saved = r.battery.significant() ? std::abs(r.battery.difference) : std::abs(r.core.difference);
        const double sign = secondSaves ? 1 : -1; // turns "arm1 - arm0" into "saving arm - other arm"
        bool cost = false;
        // More than one late frame per minute. Not for video: the test clip's own dropped-frame
        // count (in results from 0.15 and earlier) is an artifact of how it was recorded.
        if (r.frames && workload != L"video" && sign * r.drops.difference > 1.0) cost = true;
        if (r.bursts && r.burstMs.mean0 > 0 && r.burstMs.mean1 > 0) {
            const double saving = secondSaves ? r.burstMs.mean1 : r.burstMs.mean0, other = secondSaves ? r.burstMs.mean0 : r.burstMs.mean1;
            // Responses over 15% slower and by more than 5 ms: a few milliseconds on a 7 ms task cannot be felt.
            if (saving / other > 1.15 && saving - other > 5) cost = true;
        }
        if (r.work && r.workRate.mean0 > 0 && r.workRate.mean1 > 0) {
            const double ratio = secondSaves ? r.workRate.mean1 / r.workRate.mean0 : r.workRate.mean0 / r.workRate.mean1;
            if (ratio < .9) cost = true;                                                      // work over 10% slower
        }
        for (Activity a : activitiesFor(workload)) evidence.push_back({*setting, a, *value, saved, saved >= .15 && !cost});
    }
    return evidence;
}

Governor::Governor() = default;
double Governor::minimumGap(Setting s) {
    switch (s) {
    case Setting::energyPreference: return 4;
    case Setting::refreshRate: return 30;
    default: return 8;
    }
}
int Governor::direction(Setting s, DWORD a, DWORD b) {
    if (a == b) return 0;
    if (s == Setting::energyPreference) return b < a ? 1 : -1; // lower EPP favours performance
    return b > a ? 1 : -1;                                     // power mode, boost, refresh rate
}
void Governor::note(const std::wstring& text) {
    log.push_back(text);
    while (log.size() > 30) log.pop_front();
}
Profile Governor::profileFor(Activity activity, int stretch) const {
    for (int i = 0; i < stretch; ++i) activity = stretched(activity);
    Profile p;
    auto set = [&](Setting s, std::optional<DWORD> v) { p.values[size_t(s)] = v; };
    const std::optional<DWORD> low = options_.refreshRate ? std::optional<DWORD>(60) : std::nullopt;
    const std::optional<DWORD> top = options_.refreshRate ? std::optional<DWORD>(DWORD(options_.topRefresh)) : std::nullopt;
    switch (activity) {
    // Defaults from the first real test (Zephyrus G14): efficiency saved 2-3 W in every activity
    // for a few milliseconds of response, and CPU Boost cost about 5 W in light use. So
    // everything except a burst, heavy work and calls runs in efficiency with Boost off;
    // bursts and heavy work get balanced and the user's own Boost setting.
    case Activity::idle: set(Setting::powerMode, Efficiency); set(Setting::energyPreference, 90u); set(Setting::cpuBoost, 0u); set(Setting::refreshRate, low); break;
    case Activity::reading: set(Setting::powerMode, Efficiency); set(Setting::energyPreference, 80u); set(Setting::cpuBoost, 0u); set(Setting::refreshRate, low); break;
    case Activity::media: set(Setting::powerMode, Efficiency); set(Setting::energyPreference, 80u); set(Setting::cpuBoost, 0u); set(Setting::refreshRate, low); break;
    case Activity::interactive: set(Setting::powerMode, Efficiency); set(Setting::energyPreference, 70u); set(Setting::cpuBoost, 0u); set(Setting::refreshRate, top); break;
    case Activity::responsive: set(Setting::powerMode, Efficiency); set(Setting::energyPreference, 50u); set(Setting::cpuBoost, 0u); set(Setting::refreshRate, top); break;
    case Activity::burst: set(Setting::powerMode, Balanced); set(Setting::energyPreference, 33u); set(Setting::refreshRate, top); break;
    case Activity::call: set(Setting::powerMode, Balanced); set(Setting::energyPreference, 55u); set(Setting::cpuBoost, 0u); break;
    case Activity::heavy: set(Setting::powerMode, Balanced); set(Setting::energyPreference, 30u); break;
    default: break; // game: everything as the user had it
    }
    // Lab evidence: a vetoed saving leaves the setting alone; a proven one is used.
    for (size_t s = 0; s < Settings; ++s) {
        if (Setting(s) == Setting::refreshRate && !options_.refreshRate) continue;
        bool approved = false, vetoed = false; DWORD value = 0;
        for (const auto& e : evidence_) {
            if (size_t(e.setting) != s || e.activity != activity) continue;
            if (e.approved && !approved) { approved = true; value = e.savingValue; }
            if (!e.approved) vetoed = true;
        }
        if (approved && !vetoed) p.values[s] = value;
        else if (vetoed) {
            // A proven cost: efficiency falls back to balanced; other settings stay as the user had them.
            if (Setting(s) == Setting::powerMode && p.values[s] == Efficiency) p.values[s] = Balanced;
            else p.values[s].reset();
        }
    }
    return p;
}
Decision Governor::update(const Input& in) {
    Decision d;
    if (!in.onBattery) {
        if (active_) {
            // Plugged in: every setting the governor changed goes back, and throttled apps are released.
            for (size_t s = 0; s < Settings; ++s) if (lastSet_[s] && originals_[s]) d.apply[s] = originals_[s];
            for (const auto& app : throttled_) d.ecoOff.push_back(app);
            throttled_.clear(); busySince_.clear(); lastSet_ = {};
            active_ = false; d.restore = true; d.reason = L"插電：已還原";
            note(d.reason);
        }
        return d;
    }
    if (!active_) {
        active_ = true; originals_ = in.actual; lastSet_ = {}; changedAt_ = {}; rankAtChange_ = {};
        activity_ = candidate_ = in.detected ? *in.detected : derive(in); candidateSince_ = in.ms;
        note(L"開始管理（用電池）");
    }
    const Activity raw = in.detected ? *in.detected : derive(in);
    if (raw != candidate_) { candidate_ = raw; candidateSince_ = in.ms; }
    // A less certain detection must hold longer before power is saved on it.
    const double settle = SettleSeconds / std::clamp(in.confidence, .5, 1.0);
    if (rank(raw) > rank(activity_)) activity_ = raw;                                         // fast attack
    else if (raw != activity_ && in.ms - candidateSince_ >= uint64_t(settle * 1000)) activity_ = raw; // slow decay
    // Extra saving: one level when the battery will not last to the target (or the power goal
    // is exceeded), one when it is low, two when it is very low.
    const bool behind = options_.targetHours && in.hoursLeft && *in.hoursLeft < *options_.targetHours;
    int stretch = behind ? 1 : 0;
    if (in.batteryPercent && options_.lowBattery) {
        if (*in.batteryPercent <= options_.lowBattery / 2.0) stretch = 2;
        else if (*in.batteryPercent <= options_.lowBattery) stretch = std::max(stretch, 1);
    }
    stretch_ = stretch;
    const Profile profile = profileFor(activity_, stretch);
    for (size_t s = 0; s < Settings; ++s) {
        if (!in.actual[s]) continue;
        // Someone else changed what the governor set: hand the setting back for a while.
        if (lastSet_[s] && *in.actual[s] != *lastSet_[s] && in.ms >= pausedUntil_[s]) {
            pausedUntil_[s] = in.ms + uint64_t(OwnershipPauseSeconds * 1000);
            if (const int way = direction(Setting(s), *lastSet_[s], *in.actual[s])) d.overrides.push_back({Setting(s), way});
            originals_[s] = in.actual[s]; lastSet_[s].reset();
            note(L"設定被其他程式或使用者改動，暫停管理 10 分鐘");
            continue;
        }
        if (in.ms < pausedUntil_[s]) continue;
        const std::optional<DWORD> target = profile.values[s] ? profile.values[s] : originals_[s];
        if (!target || *target == *in.actual[s]) continue;
        // Toward performance (the activity rose since the last change) only waits a moment.
        const double gap = rank(activity_) > rankAtChange_[s] ? BoostGapSeconds : minimumGap(Setting(s));
        if (changedAt_[s] && in.ms - changedAt_[s] < uint64_t(gap * 1000)) continue;
        d.apply[s] = target; lastSet_[s] = target; changedAt_[s] = in.ms; rankAtChange_[s] = rank(activity_);
    }
    // Background apps busy for 30 s get efficiency mode until they come to the front.
    std::set<std::pair<uint32_t, uint64_t>> seen;
    for (const auto& app : in.apps) {
        const auto key = std::make_pair(app.pid, app.created);
        seen.insert(key);
        if (!options_.backgroundEco || app.foreground) {
            if (throttled_.erase(key)) d.ecoOff.push_back(key);
            busySince_.erase(key);
            continue;
        }
        if (app.cpu >= 1.0) {
            auto& since = busySince_[key];
            if (!since) since = in.ms;
            const double after = stretch ? options_.ecoAfterSeconds / 2 : options_.ecoAfterSeconds;
            if (in.ms - since >= uint64_t(after * 1000) && throttled_.insert(key).second) { d.ecoOn.push_back(key); note(L"背景程式改為效率模式：" + app.name); }
        } else busySince_.erase(key);
    }
    for (auto it = throttled_.begin(); it != throttled_.end();) it = seen.count(*it) ? std::next(it) : throttled_.erase(it);
    d.reason = std::wstring(activityKey(activity_)) + (stretch == 2 ? L" · 電量很低，最大省電" : stretch && behind ? L" · 耗電超過目標，加強省電" : stretch ? L" · 電量偏低，加強省電" : L"");
    bool changed = false; for (const auto& value : d.apply) changed |= value.has_value();
    if (changed) note(L"→ " + d.reason);
    return d;
}
}
