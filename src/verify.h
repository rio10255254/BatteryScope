#pragma once
// Control check: does each hardware and power control really take effect on this PC?
// Every control is written, then read back; where the effect can be measured it is
// (package power and clock under a short all-core load), and the original is restored.
// A control the firmware only accepts, with nothing to read or measure, is reported
// as exactly that, not as working.
#include <windows.h>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace bs::verify {

enum class Control { refreshRate, powerMode, cpuBoost, energyPreference, brightness, asusMode, powerLimit, fanCurve, gpuEco, chargeLimit, keyboard, count };
enum class Verdict {
    works,        // written, and the read-back or the measured effect confirms it
    noEffect,     // written without error, but nothing changed
    refused,      // the write failed
    writeOnly,    // accepted, with nothing to read back or measure
    inconclusive, // measured, but the difference is too small to call (e.g. on battery)
    unsupported,  // this PC does not have it
    skipped       // not tried (why is in `note`)
};
struct Result {
    Control control = Control::count;
    Verdict verdict = Verdict::skipped;
    DWORD status = ERROR_SUCCESS;         // of the write
    std::optional<double> before, after;  // read-backs or measurements (unit depends on the control)
    int note = 0;                         // a reason code, see Note
};
enum Note { none = 0, onBatteryOnly, pluggedInOnly, customPreset, notToggled, needsTime, lookAtIt, noSecondValue, noPowerReading, guardHeld };
const wchar_t* controlKey(Control control); // stable id for files: refresh, power-mode, ...
const wchar_t* verdictKey(Verdict verdict);

// ---- Judgements (pure).
Verdict judgeReadback(DWORD status, std::optional<double> readBack, double expected, double tolerance = 0);
// A change that should lower (or raise) a measurement: `changed` against `baseline`.
// ratio 0.8 with lower=true: works when changed <= 0.8 * baseline; no effect when within 7%.
Verdict judgeMeasured(std::optional<double> baseline, std::optional<double> changed, double ratio, bool lower);
// The charge limit can only be seen over time: plugged in and holding near the limit means it works.
Verdict judgeChargeLimit(unsigned limit, double percent, bool pluggedIn, bool charging);

// ---- Measurement: every logical processor busy for a while.
struct LoadReading { std::optional<double> packageWatts, mhz; };
LoadReading measureLoad(int milliseconds);

struct Options {
    bool onBattery = false, asus = false;
    std::optional<double> batteryPercent; bool charging = false, pluggedIn = false;
    std::function<void(Control)> progress; // called before each control
    std::function<bool()> cancelled;
};
// Runs the whole check on the calling thread (about 40 seconds on an ASUS laptop).
std::vector<Result> run(const Options& options);
std::wstring saveText(const std::vector<Result>& results);   // one line per control, for files and reports
std::vector<Result> loadText(const std::wstring& text);

} // namespace bs::verify
