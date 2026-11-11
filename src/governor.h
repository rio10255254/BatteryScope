#pragma once
#include "activity.h"
#include "lab.h"
#include <array>
#include <deque>
#include <optional>
#include <set>

// Dynamic power governor: picks power settings for what the user is doing right
// now, on battery only. It is pure logic (tested without Windows); the app reads
// the actual settings, feeds them in with each sample, and applies the decision.
//
// Layers, from fast to slow:
//   1. Activity: from the Detector (activity.h) when given, otherwise the lab's
//      context refined to "reading" and "responsive" by input timing.
//   2. Hysteresis: a move toward more performance is immediate; a move toward
//      saving power waits until the activity has held for a few seconds, longer
//      when the detector is less sure.
//   3. Per-setting pacing: a minimum time between changes of each setting,
//      except a change toward performance, which only waits a moment.
//   4. Ownership: when something else (the user, G-Helper) changes a setting
//      the governor set, it stops touching that setting for 10 minutes.
//   5. Learning: a lab result that proves a saving without a smoothness or
//      response cost unlocks that saving for the matching activities; a proven
//      cost vetoes it.
//   6. Battery target: when the time left falls short of the target, each
//      activity uses the next more saving profile.
namespace bs::gov {

// The settings the governor manages; a subset of the lab's knobs.
// firmwareMode: the ASUS performance mode (0 balanced, 1 turbo, 2 silent); unreadable elsewhere, so left alone.
enum class Setting { powerMode, energyPreference, cpuBoost, refreshRate, firmwareMode, count };
lab::Knob knobFor(Setting setting);

// Desired value per setting; std::nullopt leaves the setting as the user had it.
struct Profile { std::array<std::optional<DWORD>, size_t(Setting::count)> values{}; };

struct Options {
    bool refreshRate = false;     // switching the refresh rate makes the screen blink, so it is opt-in
    bool backgroundEco = true;
    std::optional<double> targetHours; // battery should last this long from now
    unsigned lowBattery = 20;     // at or under this percent: one level more saving; at half of it: two
    double ecoAfterSeconds = 30;  // a background app busy this long gets efficiency mode (half when saving harder)
    int topRefresh = 120;         // the panel's highest rate, used when refresh switching is on
};

struct BackgroundApp {
    uint32_t pid = 0; uint64_t created = 0;
    std::wstring name;
    double cpu = 0;               // percent of the whole machine
    bool foreground = false;
};

struct Input {
    uint64_t ms = 0;
    bool onBattery = false;
    lab::Context context = lab::Context::interactive;
    double idleSeconds = 0;
    Value hoursLeft;
    Value batteryPercent;
    // The settings as they are now (from the actuators), or nullopt if unreadable.
    std::array<std::optional<DWORD>, size_t(Setting::count)> actual{};
    std::vector<BackgroundApp> apps;
    std::optional<Activity> detected;  // from the Detector; overrides context and idleSeconds
    double confidence = 1;
};

struct Decision {
    std::array<std::optional<DWORD>, size_t(Setting::count)> apply{}; // values to write now
    std::vector<std::pair<uint32_t, uint64_t>> ecoOn, ecoOff;           // background apps to throttle / release
    bool restore = false;          // put every original back (plugged in, disabled, game)
    std::wstring reason;
    // Settings someone else changed away from the governor's value this sample:
    // +1 toward more performance, -1 toward saving. Used to learn per-app rules.
    std::vector<std::pair<Setting, int>> overrides;
};

// What the lab proved for one setting under one activity.
struct Evidence {
    Setting setting; Activity activity;
    DWORD savingValue = 0;         // the arm that used less power
    double wattsSaved = 0;
    bool approved = false;         // saves power without a measured cost
};
// Turns lab results into evidence: browse/office -> interactive and reading,
// video -> media, idle -> idle, sustained -> heavy.
std::vector<Evidence> learn(const std::vector<lab::ExperimentResult>& results);

class Governor {
public:
    static constexpr double SettleSeconds = 6, ResponsiveSeconds = 3, ReadingAfterSeconds = 10;
    static constexpr double OwnershipPauseSeconds = 600;
    static constexpr double BoostGapSeconds = .3;  // between changes toward performance
    Governor();
    void setOptions(const Options& options) { options_ = options; }
    const Options& options() const { return options_; }
    void setEvidence(std::vector<Evidence> evidence) { evidence_ = std::move(evidence); }
    // The profile used for an activity, after evidence and `stretch` levels of extra saving
    // (from the battery target or the power goal, and low battery).
    Profile profileFor(Activity activity, int stretch) const;
    int stretchLevel() const { return stretch_; }
    // One sample. Feeding a sample with onBattery = false also stops the governor and
    // returns the originals to restore, which is how it is switched off.
    Decision update(const Input& input);
    Activity activity() const { return activity_; }
    // The settings as found before the governor first changed them.
    const std::array<std::optional<DWORD>, size_t(Setting::count)>& originals() const { return originals_; }
    bool managing(Setting setting, uint64_t ms) const { return ms >= pausedUntil_[size_t(setting)]; }
    // The user chose this setting's value in BatteryScope: leave it for the rest of this
    // battery session (until plugged in) or until handed back. It is not restored either.
    void hold(Setting setting);
    void release(Setting setting);
    void releaseAll() { for (size_t s = 0; s < held_.size(); ++s) if (held_[s]) release(Setting(s)); }
    bool held(Setting setting) const { return held_[size_t(setting)]; }
    // A background app the user put in (or out of) efficiency mode themselves: not managed.
    void forget(uint32_t pid, uint64_t created) { throttled_.erase({pid, created}); busySince_.erase({pid, created}); }
    std::deque<std::wstring> log; // newest last, at most 30 entries
private:
    static double minimumGap(Setting setting);
    // +1 when value b gives more performance than a, -1 when less, 0 when equal.
    static int direction(Setting setting, DWORD a, DWORD b);
    void note(const std::wstring& text);
    Options options_;
    std::vector<Evidence> evidence_;
    Activity activity_ = Activity::interactive, candidate_ = Activity::interactive;
    int stretch_ = 0;
    uint64_t candidateSince_ = 0;
    bool active_ = false;
    std::array<std::optional<DWORD>, size_t(Setting::count)> originals_{}, lastSet_{};
    std::array<uint64_t, size_t(Setting::count)> changedAt_{}, pausedUntil_{};
    std::array<int, size_t(Setting::count)> rankAtChange_{};
    std::array<bool, size_t(Setting::count)> held_{};
    std::set<std::pair<uint32_t, uint64_t>> throttled_;
    std::map<std::pair<uint32_t, uint64_t>, uint64_t> busySince_;
};
}
