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
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Power lab: records what the PC is doing and where the battery energy goes,
// and runs A/B experiments that alternate one setting in timed blocks, so each
// setting's effect on power and smoothness is measured on this machine instead
// of assumed. Everything here is local; nothing leaves the PC.
namespace bs::lab {

// What the user is doing, from signals that need no content inspection.
enum class Context { idle, interactive, media, call, heavy, game, count };
struct Signals {
    double idleSeconds = 0;     // since the last keyboard or mouse input
    bool audio = false;         // something is playing sound
    bool call = false;          // microphone or camera in use
    bool fullscreenGame = false; // a Direct3D app owns the full screen
    Value systemCpu;            // percent of all logical processors
};
Context classify(const Signals& signals);
const wchar_t* contextKey(Context context); // a Chinese UI string, translated by the caller

// One sample, about every two seconds while the lab is on.
struct Sample {
    uint64_t ms = 0;
    std::wstring time;            // local time, for the trace
    bool onBattery = false;
    Value batteryWatts, packageWatts, coreWatts, systemCpu;
    Signals signals;
    Context context = Context::interactive;
    std::wstring foreground;
    int refreshHz = 0;
    Value dgpuPercent, igpuPercent;
    int dgpuState = -1;           // device power state: 0 = D0 (awake) ... 3 = D3, -1 unknown, 9 = not present (off)
    std::wstring dgpuApps;        // processes using the discrete GPU, ";"-separated
    double framesComposed = 0, framesDropped = 0, refreshes = 0; // DWM deltas since the previous sample
    int boostDc = -1, eppDc = -1; // processor settings of the active power plan, on battery
    bool ghelper = false;
    std::wstring experiment;
    int arm = -1, block = -1;
    bool counted = false;
    // The part of battery power outside the CPU package: display, discrete GPU, memory, platform.
    Value restWatts() const;
};
std::string csvHeader();
std::string csvRow(const Sample& sample);

// Power use per activity, from recorded samples on battery.
class Breakdown {
public:
    struct Totals { double seconds = 0, battery = 0, package = 0, core = 0, rest = 0, restSeconds = 0; };
    void add(const Sample& sample, double seconds);
    const Totals& totals(Context context) const { return totals_[size_t(context)]; }
    double dgpuAwakeSeconds() const { return dgpuAwake_; }
    double batterySeconds() const { return batterySeconds_; }
    // Apps seen on the discrete GPU while on battery, most seconds first.
    std::vector<std::pair<std::wstring, double>> dgpuApps(size_t limit) const;
private:
    std::array<Totals, size_t(Context::count)> totals_{};
    double dgpuAwake_ = 0, batterySeconds_ = 0;
    std::map<std::wstring, double> dgpuApps_;
};

// Pairs of blocks in random order (AB or BA) so slow drift in workload or
// battery behavior affects both arms equally.
class Schedule {
public:
    explicit Schedule(uint64_t seed) : seed_(seed ? seed : 1) {}
    int armFor(int block) const;
private:
    uint64_t seed_;
};

// The measurements of one block, without its warm-up.
struct Block {
    std::wstring experiment;
    uint64_t run = 0;
    int pair = 0, arm = 0;
    std::wstring armName, started;
    double seconds = 0;
    double battery = 0, package = 0, core = 0, rest = 0, cpu = 0;
    double dropsPerMinute = 0, composedPerSecond = 0;
    std::array<double, size_t(Context::count)> contextShare{};
    std::string csv() const;
    static bool parse(const std::string& line, Block& block);
    static std::string csvHeader();
};
class BlockRecorder {
public:
    static constexpr double WarmupSeconds = 20, MinimumSeconds = 60;
    void begin(int pair, int arm, uint64_t ms);
    // A sample during the block; ignored during warm-up or when not countable.
    bool add(const Sample& sample, double seconds);
    void invalidate() { invalid_ = true; }
    bool invalid() const { return invalid_; }
    double counted() const { return seconds_; }
    uint64_t startMs() const { return start_; }
    int pair() const { return pair_; }
    int arm() const { return arm_; }
    // The finished block, or false if it was invalidated or too short.
    bool finish(Block& block) const;
private:
    int pair_ = 0, arm_ = 0;
    uint64_t start_ = 0;
    bool invalid_ = false;
    double seconds_ = 0, battery_ = 0, package_ = 0, core_ = 0, rest_ = 0, restSeconds_ = 0, cpu_ = 0, cpuSeconds_ = 0;
    double drops_ = 0, composed_ = 0;
    std::array<double, size_t(Context::count)> contexts_{};
};

// Paired comparison of the two arms: the mean of (arm 1 - arm 0) over complete
// pairs, with a 95% interval from Student's t.
struct Comparison {
    int pairs = 0;
    double mean0 = 0, mean1 = 0, difference = 0, halfWidth = 0;
    bool significant() const { return pairs >= 2 && std::abs(difference) > halfWidth; }
};
Comparison comparePairs(const std::vector<std::pair<double, double>>& pairs);
struct ExperimentResult {
    std::wstring experiment, arm0, arm1;
    Comparison battery, package, core, rest, cpu, drops;
};
// Groups blocks by experiment and pairs them by run and pair number.
std::vector<ExperimentResult> analyze(const std::vector<Block>& blocks);

// ----- Windows side (lab_system.cpp) -----

enum class Knob { cpuBoost, energyPreference, refreshRate, powerMode, count };
const wchar_t* knobKey(Knob knob); // stable id used in files: cpu-boost, epp, refresh, power-mode
struct KnobPlan {
    bool available = false;
    DWORD error = ERROR_SUCCESS;  // why it is not available
    DWORD original = 0;           // value before the experiment (for power mode: a PowerMode)
    std::array<DWORD, 2> values{};
    std::array<std::wstring, 2> names;
};
KnobPlan planKnob(Knob knob);
DWORD applyKnob(Knob knob, DWORD value);
DWORD readKnob(Knob knob, DWORD& value);

// Reads the signals of one sample that are not part of the regular snapshot.
class Probe {
public:
    Probe();
    ~Probe();
    Probe(const Probe&) = delete;
    Probe& operator=(const Probe&) = delete;
    void read(Sample& sample, const std::vector<std::pair<std::wstring, double>>& gpuInstances,
        const std::vector<ProcessRow>& processes);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Remembers the original value of the setting under test in the registry, so a
// crash or power loss mid-experiment is undone at the next start.
void rememberOriginal(Knob knob, DWORD value);
void forgetOriginal();
bool restoreInterrupted();

std::wstring labFolder(); // %LOCALAPPDATA%\BatteryScope\lab, created on demand

// How the controller reads and changes settings; replaced by a fake in tests.
struct Actuator {
    virtual ~Actuator() = default;
    virtual KnobPlan plan(Knob knob) { return planKnob(knob); }
    virtual DWORD apply(Knob knob, DWORD value) { return applyKnob(knob, value); }
    virtual DWORD read(Knob knob, DWORD& value) { return readKnob(knob, value); }
    virtual void remember(Knob knob, DWORD value) { rememberOriginal(knob, value); }
    virtual void forget() { forgetOriginal(); }
};

// Runs recording and one experiment at a time. Commands come from the UI
// thread, samples from the sampler thread. An empty folder writes no files.
class Controller {
public:
    static constexpr double BlockSeconds = 120;
    static constexpr int TargetPairs = 8;
    struct Status {
        bool recording = false, running = false, paused = false, ghelper = false;
        double recordedSeconds = 0;
        Knob knob = Knob::cpuBoost;
        std::array<std::wstring, 2> armNames;
        int arm = -1, block = 0, pairsDone = 0, externalChanges = 0;
        double blockElapsed = 0;
        DWORD lastError = ERROR_SUCCESS;
        bool finished = false;
        Breakdown breakdown;
        std::vector<ExperimentResult> results;
    };
    explicit Controller(std::wstring folder = labFolder(), std::shared_ptr<Actuator> actuator = std::make_shared<Actuator>());
    void setRecording(bool on);
    DWORD start(Knob knob);
    void stop();
    bool active() const;
    void onSample(Sample& sample, double seconds);
    Status status() const;
    void shutdown(); // restores the setting under test and writes pending rows
    void flushNow(); // writes pending trace rows, e.g. before an export
private:
    void finishRun(bool completed);
    void flush();
    mutable std::mutex lock_;
    std::wstring folder_;
    std::shared_ptr<Actuator> actuator_;
    bool recording_ = false, running_ = false, applied_ = false, paused_ = false, finished_ = false, ghelper_ = false;
    double recordedSeconds_ = 0;
    Knob knob_ = Knob::cpuBoost;
    KnobPlan plan_;
    Schedule schedule_{1};
    uint64_t run_ = 0;
    int block_ = 0, externalChanges_ = 0;
    std::map<int, std::array<bool, 2>> pairs_;
    BlockRecorder recorder_;
    DWORD lastError_ = ERROR_SUCCESS;
    Breakdown breakdown_;
    std::vector<Block> blocks_;
    std::string pendingRows_;
    std::wstring traceDate_;
    size_t pendingCount_ = 0;
};
}
