#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "model.h"
#include <array>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

// Activity detection: what the user is doing right now, from many cheap signals
// read without looking at any content (no screenshots, no window text, no
// keystrokes recorded; only timings, the foreground program's name, device and
// engine usage). Pure logic: the app fills an Observation, the Detector
// weighs every candidate activity and returns the most demanding one it is
// confident about, with the reasons.
namespace bs::gov {

// burst: input or a program switch together with a CPU spike (opening a program, loading a page).
enum class Activity { idle, reading, responsive, burst, interactive, media, call, heavy, game, exempt, count };
const wchar_t* activityKey(Activity activity); // a Chinese UI string
int rank(Activity activity);                    // higher needs more performance

// What kind of program the foreground app is, from its executable name.
enum class Category { other, browser, document, code, call, video, creative, game, launcher, count };
Category categoryOf(const std::wstring& exe);   // case-insensitive
const wchar_t* categoryKey(Category category);  // a Chinese UI string

// A per-program choice. Learned rules come from the user overriding the governor.
enum class AppRule { automatic, saving, balanced, performance, exempt, count };
const wchar_t* ruleKey(AppRule rule);           // a Chinese UI string

// The latest input of each kind, from raw input. Pointer movement alone does
// not count as "active": reading with the cursor drifting should not boost.
enum class InputKind { none, pointer, click, scroll, typing };
const wchar_t* inputKey(InputKind kind);

struct EngineUse { double engine3d = 0, decode = 0, compute = 0; };
// GPU engine use per process and in total, from the "GPU Engine" counters.
struct GpuActivity {
    bool known = false;
    EngineUse total;                            // the busiest adapter's engines
    std::unordered_map<uint32_t, EngineUse> processes;
};
GpuActivity summarizeGpu(const std::vector<std::pair<std::wstring, double>>& instances);

struct Observation {
    uint64_t ms = 0;
    double idleSeconds = 0;                     // since any input (GetLastInputInfo)
    Value activeSeconds;                        // since a key, click or wheel; nullopt without raw input
    InputKind lastInput = InputKind::none;
    double foregroundSwitchSeconds = 1e9;       // since the foreground program changed
    bool locked = false, displayOff = false;
    bool call = false;                          // microphone or camera in use
    bool exclusiveFullscreen = false;           // a Direct3D app owns the screen
    std::wstring foregroundExe;                 // lower case; the last program other than BatteryScope
    uint32_t foregroundPid = 0;
    bool foregroundCoversScreen = false;        // borderless or exclusive full screen
    bool audio = false;                         // anything is playing sound
    bool foregroundAudio = false;               // the foreground program is playing sound
    Value systemCpu;                            // percent of all logical processors
    GpuActivity gpu;
    Value framesPerSecond;                      // frames the compositor drew; ~0 when nothing moves (nullopt when the counter does not work)
    Value compositorGpu;                        // 3D engine use of the compositor (dwm.exe); preferred over framesPerSecond
};

struct Candidate { Activity activity; double confidence; std::wstring reason; };
struct Assessment {
    Activity activity = Activity::interactive;
    double confidence = .5;
    std::wstring reason;                        // Chinese, for the status page
    Category category = Category::other;
    AppRule rule = AppRule::automatic;
    bool learnedRule = false;
    std::vector<Candidate> candidates;          // everything considered, strongest first
};

class Detector {
public:
    static constexpr double MinConfidence = .6;     // weaker candidates are ignored
    static constexpr double HeavyAfterSeconds = 4;  // busy this long before "heavy"
    static constexpr double ResponsiveSeconds = 3, ReadingAfterSeconds = 10, IdleAfterSeconds = 120;
    void setRules(std::map<std::wstring, AppRule> rules, std::map<std::wstring, AppRule> learned) { rules_ = std::move(rules); learned_ = std::move(learned); }
    AppRule ruleFor(const std::wstring& exe, bool* learned = nullptr) const;
    Assessment assess(const Observation& o);
private:
    std::map<std::wstring, AppRule> rules_, learned_;
    uint64_t busySince_ = 0, gpuBusySince_ = 0;
};

// Scores detection during the automatic test, where the true activity is known.
class DetectionTally {
public:
    void add(const std::wstring& workload, Activity detected, double seconds);
    static std::vector<Activity> expected(const std::wstring& workload); // empty: not scored
    // Share of the workload's time detected as an expected activity; nullopt without data.
    Value accuracy(const std::wstring& workload) const;
    Value overall() const;
    std::wstring csv() const;                   // workload,activity,seconds,expected
    std::wstring summary() const;               // Chinese lines for the report
    bool empty() const { return seconds_.empty(); }
private:
    std::map<std::wstring, std::array<double, size_t(Activity::count)>> seconds_;
};
}
