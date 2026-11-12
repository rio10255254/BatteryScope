#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "model.h"
#include "activity.h"
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

// Synthetic work the automatic test runs while a setting is measured, so both
// settings of a pair face exactly the same load.
// browse, video and office run as a page in a separate Edge (or Chrome) window
// the lab drives; the others are built in.
enum class Workload { none, idle, scroll, bursts, sustained, browse, video, office, count };
const wchar_t* workloadKey(Workload workload);   // stable id: idle, scroll, bursts, sustained
const wchar_t* workloadName(Workload workload);  // a Chinese UI string
// What the workload engine measured since the previous sample.
struct WorkloadMetrics {
    double frames = 0, lateFrames = 0;   // scrolling frames, and frames later than 1.5 refresh periods
    double bursts = 0, burstMs = 0;      // completed bursts and their total time
    double work = 0;                     // fixed work units done by the sustained threads
};

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
    std::wstring topProcesses;    // "name cpu%;..." busiest processes this sample
    WorkloadMetrics workload;
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
    double burstMs = 0, workPerSecond = 0, framesPerSecond = 0; // automatic test workload results (0 when unused)
    std::string csv() const;
    static bool parse(const std::string& line, Block& block);
    static std::string csvHeader();
};
class BlockRecorder {
public:
    static constexpr double WarmupSeconds = 20, MinimumSeconds = 60;
    void begin(int pair, int arm, uint64_t ms, double warmup = WarmupSeconds, double minimum = MinimumSeconds);
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
    double warmup_ = WarmupSeconds, minimum_ = MinimumSeconds;
    WorkloadMetrics workload_;
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
    Comparison burstMs, workRate, energyPerWork; // automatic test only
    bool frames = false, bursts = false, work = false; // which workload results exist
};
// Groups blocks by experiment and pairs them by run and pair number.
std::vector<ExperimentResult> analyze(const std::vector<Block>& blocks);

// ----- Windows side (lab_system.cpp) -----

// brightness, display and backgroundEco are used by the automatic test only:
// display turns the screen off for a block, backgroundEco puts every background
// app of this user in EcoQoS (the "App Nap" idea).
// darkMode switches Windows' app theme (light or dark); on an OLED panel a dark page draws much less power.
// asusMode is the ASUS firmware performance mode (silent / balanced / turbo), on ASUS laptops only.
enum class Knob { cpuBoost, energyPreference, refreshRate, powerMode, brightness, display, backgroundEco, darkMode, asusMode, count };
constexpr int ManualKnobs = 4; // the first four can also be run one at a time from the menu
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
std::vector<int> availableRefreshRates();  // at the current resolution, lowest first

// Reads the signals of one sample that are not part of the regular snapshot.
class Probe {
public:
    Probe();
    ~Probe();
    Probe(const Probe&) = delete;
    Probe& operator=(const Probe&) = delete;
    void read(Sample& sample, const std::vector<std::pair<std::wstring, double>>& gpuInstances,
        const std::vector<ProcessRow>& processes);
    struct Impl; // also used by SignalReader
private:
    std::unique_ptr<Impl> impl_;
};

// The activity signals alone, cheap enough to read every second (no process
// or GPU scan): input idle time, sound, microphone/camera, full-screen games.
class SignalReader {
public:
    SignalReader();
    ~SignalReader();
    SignalReader(const SignalReader&) = delete;
    SignalReader& operator=(const SignalReader&) = delete;
    Signals read(Value systemCpu);
    // Everything the activity detector reads itself (see activity.h): the above
    // plus the foreground program, sound per program, how much the screen is
    // changing and whether the session is locked. Raw input, the display state
    // and GPU engines come from the app.
    gov::Observation observe(Value systemCpu);
    const std::wstring& lastForeground() const; // lower-case exe of the last program other than BatteryScope
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Programs using the discrete GPU in these "GPU Engine" counter instances (by process
// name), or empty; checked before the discrete GPU is switched off.
std::vector<std::wstring> discreteGpuUsers(const std::vector<std::pair<std::wstring, double>>& gpu, const std::vector<ProcessRow>& processes);
// EcoQoS for one process (no priority change), checked against its start time.
DWORD setProcessEco(uint32_t pid, uint64_t created, bool on);

// Remembers the original value of the setting under test in the registry, so a
// crash or power loss mid-experiment is undone at the next start.
void rememberOriginal(Knob knob, DWORD value);
void forgetOriginal();
bool restoreInterrupted();

std::wstring labFolder(); // %LOCALAPPDATA%\BatteryScope\lab, created on demand
// Processes in this job (the test browser) are never throttled by backgroundEco.
void setProtectedJob(HANDLE job);
// Power plans, battery report, hardware, display, processes, startup apps and
// running tasks, written to <folder>\system-<time>. Takes up to a minute.
void writeSystemSnapshot(const std::wstring& folder);

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
// One measured setting: a knob under a workload, run as alternating pairs.
struct Step {
    Knob knob = Knob::cpuBoost;
    Workload workload = Workload::none;
    int pairs = 8;
    double blockSeconds = 120, warmupSeconds = 20, minimumSeconds = 60;
    double prepareSeconds = 0; // load started but not measured, before the first block (e.g. making the test video)
};
// The automatic test: each knob under the workloads it can affect. With a
// browser it uses real web browsing, video and typing; without one, built-in loads.
std::vector<Step> automaticPlan(bool browser);
// About 25 minutes: power mode in every activity (detection is scored on all of them),
// dark mode and brightness on a light page; 30-second blocks.
std::vector<Step> quickPlan(bool browser);
std::wstring experimentKey(Knob knob, Workload workload); // "cpu-boost" or "cpu-boost@scroll"

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
        bool plan = false;            // running the automatic test
        int step = 0, steps = 0, skipped = 0;
        Workload workload = Workload::none;
        double remainingSeconds = 0;  // of battery time, for the whole plan
        uint64_t run = 0;
        Breakdown breakdown;
        std::vector<ExperimentResult> results;
    };
    explicit Controller(std::wstring folder = labFolder(), std::shared_ptr<Actuator> actuator = std::make_shared<Actuator>());
    void setRecording(bool on);
    DWORD start(Knob knob);
    // Runs the steps one after another, restoring each setting before the next.
    DWORD startPlan(const std::vector<Step>& steps);
    void stop();
    Workload workload() const;    // what the workload engine should run now
    bool active() const;
    void onSample(Sample& sample, double seconds);
    Status status() const;
    void shutdown(); // restores the setting under test and writes pending rows
    void flushNow(); // writes pending trace rows, e.g. before an export
private:
    void finishRun(bool completed);
    bool beginStep();             // plans the current step's knob; false when none is left
    void finishStep();
    void log(const std::wstring& text);
    void flush();
    mutable std::mutex lock_;
    std::wstring folder_;
    std::shared_ptr<Actuator> actuator_;
    bool recording_ = false, running_ = false, applied_ = false, paused_ = false, finished_ = false, ghelper_ = false;
    double recordedSeconds_ = 0;
    Knob knob_ = Knob::cpuBoost;
    std::vector<Step> steps_;
    size_t stepIndex_ = 0;
    bool planMode_ = false;
    int skipped_ = 0;
    uint64_t prepareStart_ = 0;
    bool prepared_ = false;
    KnobPlan plan_;
    Schedule schedule_{1};
    uint64_t run_ = 0;
    int block_ = 0, externalChanges_ = 0;
    std::map<int, std::array<bool, 2>> pairs_;
    BlockRecorder recorder_;
    DWORD lastError_ = ERROR_SUCCESS;
    Breakdown breakdown_;
    std::vector<Block> blocks_;
    std::string pendingRows_, pendingLog_;
    std::wstring traceDate_;
    size_t pendingCount_ = 0;
};

// Runs the automatic test's synthetic loads: a full-screen window that scrolls
// a mock page with a fixed amount of work per frame and counts its own late
// frames, short fixed bursts every 250 ms, and two threads of sustained fixed
// work. Each load is the same for both settings of a pair. Esc asks to stop.
class WorkloadEngine {
public:
    WorkloadEngine();
    ~WorkloadEngine();
    WorkloadEngine(const WorkloadEngine&) = delete;
    WorkloadEngine& operator=(const WorkloadEngine&) = delete;
    void start(Controller* controller);
    void stop();
    bool running() const;
    bool abortRequested() const;
    WorkloadMetrics take(); // and reset, once per sample
    // Edge, else Chrome; empty when neither is installed.
    static std::wstring browserPath();
    int port() const; // the local server's port in browser mode, else 0
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
