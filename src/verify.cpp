#include "verify.h"
#include "asus.h"
#include "energy.h"
#include "lab.h"
#include "stability.h"

#include <pdh.h>
#include <emmintrin.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <sstream>
#include <thread>

namespace bs::verify {

const wchar_t* controlKey(Control c) {
    switch (c) {
    case Control::refreshRate: return L"refresh";
    case Control::powerMode: return L"power-mode";
    case Control::cpuBoost: return L"cpu-boost";
    case Control::energyPreference: return L"epp";
    case Control::brightness: return L"brightness";
    case Control::asusMode: return L"asus-mode";
    case Control::powerLimit: return L"power-limit";
    case Control::fanCurve: return L"fan-curve";
    case Control::gpuEco: return L"gpu-eco";
    case Control::chargeLimit: return L"charge-limit";
    case Control::keyboard: return L"keyboard";
    case Control::loadBaseline: return L"load-baseline";
    case Control::powerRestore: return L"power-restore";
    default: return L"";
    }
}
const wchar_t* verdictKey(Verdict v) {
    switch (v) {
    case Verdict::works: return L"works";
    case Verdict::noEffect: return L"no-effect";
    case Verdict::refused: return L"refused";
    case Verdict::writeOnly: return L"write-only";
    case Verdict::inconclusive: return L"inconclusive";
    case Verdict::unsupported: return L"unsupported";
    default: return L"skipped";
    }
}

Verdict judgeReadback(DWORD status, std::optional<double> readBack, double expected, double tolerance) {
    if (status == ERROR_NOT_SUPPORTED && !readBack) return Verdict::unsupported;
    if (!readBack) return status == ERROR_SUCCESS ? Verdict::writeOnly : Verdict::refused;
    if (std::abs(*readBack - expected) <= tolerance) return Verdict::works;
    return status == ERROR_SUCCESS || status == ERROR_NOT_SUPPORTED ? Verdict::noEffect : Verdict::refused;
}
Verdict judgeMeasured(std::optional<double> baseline, std::optional<double> changed, double ratio, bool lower) {
    if (!baseline || !changed || *baseline <= 0 || *changed <= 0) return Verdict::inconclusive;
    const double relative = *changed / *baseline;
    if (lower ? relative <= ratio : relative >= ratio) return Verdict::works;
    if (std::abs(relative - 1) <= .07) return Verdict::noEffect;
    return Verdict::inconclusive;
}
Verdict judgeChargeLimit(unsigned limit, double percent, bool pluggedIn, bool charging) {
    if (limit < 20 || limit >= 100) return Verdict::skipped;
    if (pluggedIn && charging && percent > limit + 3) return Verdict::noEffect; // still charging past the limit
    if (pluggedIn && !charging && std::abs(percent - double(limit)) <= 4) return Verdict::works; // holding at the limit
    return Verdict::inconclusive;
}

namespace {
double baseMhz() {
    struct Info { ULONG number, maxMhz, currentMhz, mhzLimit, maxIdleState, currentIdleState; };
    LONG (WINAPI* information)(int, PVOID, ULONG, PVOID, ULONG) = nullptr;
    if (HMODULE module = LoadLibraryExW(L"powrprof.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        FARPROC symbol = GetProcAddress(module, "CallNtPowerInformation");
        std::memcpy(&information, &symbol, sizeof(information));
    }
    if (!information) return 0;
    std::vector<Info> cpus(std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));
    if (information(11 /* ProcessorInformation */, nullptr, 0, cpus.data(), ULONG(cpus.size() * sizeof(Info))) != 0) return 0;
    return double(cpus.front().maxMhz);
}
void pause(int ms, const Options& o) {
    for (int waited = 0; waited < ms && !(o.cancelled && o.cancelled()); waited += 100) Sleep(100);
}
std::optional<double> knob(lab::Knob k) {
    DWORD value = 0;
    return lab::readKnob(k, value) == ERROR_SUCCESS ? std::optional<double>(double(value)) : std::nullopt;
}
// Write `target`, read back, restore `original`.
Result readbackCheck(Control control, lab::Knob k, DWORD original, DWORD target, double tolerance = 0) {
    Result r; r.control = control; r.before = double(original);
    r.status = lab::applyKnob(k, target);
    Sleep(150);
    r.after = knob(k);
    r.verdict = judgeReadback(r.status, r.after, double(target), tolerance);
    lab::applyKnob(k, original);
    return r;
}
} // namespace

LoadReading measureLoad(int milliseconds) {
    LoadReading reading;
    // The clock, as Task Manager shows it: "% Processor Performance" times the base clock.
    PDH_HQUERY query = nullptr; PDH_HCOUNTER counter = nullptr;
    const double base = baseMhz();
    if (PdhOpenQueryW(nullptr, 0, &query) == ERROR_SUCCESS) {
        if (PdhAddEnglishCounterW(query, L"\\Processor Information(_Total)\\% Processor Performance", 0, &counter) != ERROR_SUCCESS) { PdhCloseQuery(query); query = nullptr; }
    } else query = nullptr;
    std::atomic<bool> stop{false};
    std::vector<std::thread> workers;
    const DWORD processors = std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    // The whole process, and each worker, at full speed: Windows' efficiency throttling
    // (EcoQoS) would hold the clock down and hide every power limit.
    struct { ULONG version, controlMask, stateMask; } full{1, 0x1 /* execution speed */, 0 /* off */};
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &full, sizeof(full));
    for (DWORD i = 0; i < processors; ++i) workers.emplace_back([&stop, full] {
        auto state = full;
        SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &state, sizeof(state));
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
        // Eight independent chains of packed multiply, add and square root: every execution
        // port busy, so the processor draws what its limits allow (a light loop sat at 20 W on
        // the G14 whatever the mode, far under any limit).
        __m128d a[8];
        for (int k = 0; k < 8; ++k) a[k] = _mm_set_pd(1.0001 + k, 2.0003 + k);
        const __m128d scale = _mm_set1_pd(1.0000001), shift = _mm_set1_pd(0.5);
        while (!stop.load(std::memory_order_relaxed))
            for (int k = 0; k < 4000; ++k)
                for (int c = 0; c < 8; ++c) a[c] = _mm_sqrt_pd(_mm_add_pd(_mm_mul_pd(a[c], scale), shift));
        volatile double sink = _mm_cvtsd_f64(a[0]); (void)sink;
    });
    // Let the limits settle for a third of the time, then measure the rest.
    Sleep(DWORD(std::max(300, milliseconds / 3)));
    EnergyReader energy;
    energy.sample();
    if (query) PdhCollectQueryData(query);
    Sleep(DWORD(std::max(600, milliseconds * 2 / 3)));
    const CpuEnergyReport report = energy.sample();
    if (query && PdhCollectQueryData(query) == ERROR_SUCCESS) {
        PDH_FMT_COUNTERVALUE value{};
        if (base > 0 && PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, nullptr, &value) == ERROR_SUCCESS && value.CStatus == ERROR_SUCCESS)
            reading.mhz = base * value.doubleValue / 100;
    }
    stop = true;
    for (auto& worker : workers) worker.join();
    if (query) PdhCloseQuery(query);
    // Back to the system's own choice for this process.
    struct { ULONG version, controlMask, stateMask; } automatic{1, 0, 0};
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &automatic, sizeof(automatic));
    if (base > 0) reading.baseMhz = base;
    if (report.ready && !report.timingMismatch) reading.packageWatts = report.packageWatts ? report.packageWatts : report.coreWatts;
    return reading;
}

// The plan's CPU Boost mode while plugged in (0 off), or nothing when it cannot be read.
std::optional<double> pluggedInBoost() {
    HMODULE module = LoadLibraryExW(L"powrprof.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return std::nullopt;
    DWORD (WINAPI* active)(HKEY, GUID**) = nullptr;
    DWORD (WINAPI* readAc)(HKEY, const GUID*, const GUID*, const GUID*, LPDWORD) = nullptr;
    FARPROC a = GetProcAddress(module, "PowerGetActiveScheme"), b = GetProcAddress(module, "PowerReadACValueIndex");
    std::memcpy(&active, &a, sizeof(active)); std::memcpy(&readAc, &b, sizeof(readAc));
    if (!active || !readAc) return std::nullopt;
    constexpr GUID Processor = {0x54533251, 0x82be, 0x4824, {0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00}};
    constexpr GUID Boost = {0xbe337238, 0x0d82, 0x4146, {0xa9, 0x60, 0x4f, 0x37, 0x49, 0xd4, 0x70, 0xc7}};
    GUID* scheme = nullptr; DWORD value = 0;
    if (active(nullptr, &scheme) != ERROR_SUCCESS || !scheme) return std::nullopt;
    const DWORD status = readAc(nullptr, scheme, &Processor, &Boost, &value);
    LocalFree(scheme);
    return status == ERROR_SUCCESS ? std::optional<double>(double(value)) : std::nullopt;
}
bool heldBack(const LoadReading& r) { return r.mhz && r.baseMhz && *r.mhz < *r.baseMhz * .7; }

std::vector<Result> run(const Options& o) {
    std::vector<Result> results;
    auto cancelled = [&] { return (o.cancelled && o.cancelled()) || stability::writeGuard().held(); };
    auto begin = [&](Control c) { if (o.progress) o.progress(c); stability::note(std::wstring(L"verify ") + controlKey(c)); };
    auto skip = [&](Control c, Verdict v, int note) { Result r; r.control = c; r.verdict = v; r.note = note; results.push_back(r); };
    stability::note(L"verify start");

    // Refresh rate: the other available rate, read back from the display.
    begin(Control::refreshRate);
    if (const auto now = knob(lab::Knob::refreshRate)) {
        const auto rates = lab::availableRefreshRates();
        int other = 0;
        for (int hz : rates) if (hz != int(*now) && hz >= 48) other = hz; // the highest other rate
        if (other) results.push_back(readbackCheck(Control::refreshRate, lab::Knob::refreshRate, DWORD(*now), DWORD(other)));
        else skip(Control::refreshRate, Verdict::skipped, noSecondValue);
    } else skip(Control::refreshRate, Verdict::unsupported, none);

    // Windows power mode on battery, CPU Boost and the energy preference: written and read back.
    begin(Control::powerMode);
    if (const auto now = knob(lab::Knob::powerMode); now && !cancelled())
        results.push_back(readbackCheck(Control::powerMode, lab::Knob::powerMode, DWORD(*now), *now == 0 ? 1 : 0));
    else skip(Control::powerMode, now ? Verdict::skipped : Verdict::unsupported, now ? guardHeld : none);

    begin(Control::cpuBoost);
    if (const auto now = knob(lab::Knob::cpuBoost); now && !cancelled()) {
        Result r = readbackCheck(Control::cpuBoost, lab::Knob::cpuBoost, DWORD(*now), *now == 0 ? 2 : 0);
        if (r.verdict == Verdict::works && o.onBattery) {
            // On battery the effect shows in the clock under load: boost lifts it above the base clock.
            lab::applyKnob(lab::Knob::cpuBoost, 0); pause(600, o);
            const auto off = measureLoad(2400);
            lab::applyKnob(lab::Knob::cpuBoost, 2); pause(600, o);
            const auto on = measureLoad(2400);
            lab::applyKnob(lab::Knob::cpuBoost, DWORD(*now));
            r.before = off.mhz; r.after = on.mhz;
            const Verdict measured = judgeMeasured(off.mhz, on.mhz, 1.05, false);
            r.verdict = measured == Verdict::works ? Verdict::works : measured;
        } else if (r.verdict == Verdict::works) r.note = onBatteryOnly; // the setting is for battery: nothing to observe while plugged in
        results.push_back(r);
    } else skip(Control::cpuBoost, now ? Verdict::skipped : Verdict::unsupported, now ? guardHeld : none);

    begin(Control::energyPreference);
    if (const auto now = knob(lab::Knob::energyPreference); now && !cancelled())
        results.push_back(readbackCheck(Control::energyPreference, lab::Knob::energyPreference, DWORD(*now), *now >= 70 ? 50 : 80));
    else skip(Control::energyPreference, now ? Verdict::skipped : Verdict::unsupported, now ? guardHeld : none);

    // Brightness: ten points up or down, read back from the panel.
    begin(Control::brightness);
    if (const int level = lab::screenBrightness(); level >= 0 && !cancelled()) {
        const int target = level >= 60 ? level - 15 : level + 15;
        Result r; r.control = Control::brightness; r.before = level;
        r.status = lab::applyKnob(lab::Knob::brightness, DWORD(target));
        const int after = lab::screenBrightness();
        if (after >= 0) r.after = after;
        r.verdict = judgeReadback(r.status == ERROR_NOT_SUPPORTED && after >= 0 ? ERROR_SUCCESS : r.status, r.after, target, 3);
        lab::applyKnob(lab::Knob::brightness, DWORD(level));
        results.push_back(r);
    } else skip(Control::brightness, level < 0 ? Verdict::unsupported : Verdict::skipped, level < 0 ? none : guardHeld);

    // ASUS firmware controls.
    const asus::Snapshot snapshot = o.asus ? asus::read() : asus::Snapshot{};
    if (!o.asus || !snapshot.driver) {
        for (Control c : {Control::asusMode, Control::powerLimit, Control::powerRestore, Control::fanCurve, Control::gpuEco, Control::chargeLimit, Control::keyboard}) skip(c, Verdict::unsupported, none);
    } else {
        const asus::Tuning tuning = asus::tuning();
        const asus::Mode mode = asus::remembered().mode.value_or(asus::Mode::balanced);
        // The performance mode cannot be read back: its effect is the package power under load,
        // silent against turbo, with the factory fans and limits.
        begin(Control::asusMode);
        if (!snapshot.modeDevice) skip(Control::asusMode, Verdict::unsupported, none);
        else if (cancelled()) skip(Control::asusMode, Verdict::skipped, guardHeld);
        else {
            // First, the load as things stand, before anything is written.
            {
                begin(Control::loadBaseline);
                const auto base = measureLoad(3600);
                Result b; b.control = Control::loadBaseline; b.before = base.packageWatts; b.mhz = base.mhz;
                b.after = pluggedInBoost(); // the plan's CPU Boost while plugged in: 0 means off
                b.verdict = base.packageWatts ? Verdict::skipped : Verdict::unsupported;
                results.push_back(b);
            }
            Result r; r.control = Control::asusMode;
            if (tuning.fans != asus::FanPreset::factory || tuning.power != asus::PowerPreset::factory) asus::setTuning({});
            r.status = asus::setMode(asus::Mode::silent); pause(1500, o);
            const auto silent = measureLoad(3600);
            const DWORD turboStatus = asus::setMode(asus::Mode::turbo); pause(1500, o);
            const auto turbo = measureLoad(3600);
            if (r.status == ERROR_SUCCESS) r.status = turboStatus;
            r.before = silent.packageWatts; r.after = turbo.packageWatts; r.mhz = turbo.mhz;
            if (r.status != ERROR_SUCCESS) r.verdict = r.status == ERROR_NOT_SUPPORTED ? Verdict::unsupported : Verdict::refused;
            else if (!silent.packageWatts || !turbo.packageWatts) { r.verdict = Verdict::writeOnly; r.note = noPowerReading; }
            else {
                r.verdict = judgeMeasured(silent.packageWatts, turbo.packageWatts, 1.15, false);
                if (r.verdict != Verdict::works && o.onBattery) { r.verdict = Verdict::inconclusive; r.note = pluggedInOnly; } // limits are close together on battery
                else if (r.verdict != Verdict::works && (heldBack(silent) || heldBack(turbo))) { r.verdict = Verdict::inconclusive; r.note = loadHeldBack; }
            }
            results.push_back(r);

            // CPU power limit: the 15 W preset against factory, in turbo, under the same load.
            begin(Control::powerLimit);
            if (!asus::powerLimitsSupported()) skip(Control::powerLimit, Verdict::unsupported, none);
            else if (cancelled()) skip(Control::powerLimit, Verdict::skipped, guardHeld);
            else if (!turbo.packageWatts || *turbo.packageWatts < 28) {
                // The load already draws less than the limit would allow: writing the limit could show
                // nothing, so it is not written at all.
                Result p; p.control = Control::powerLimit; p.verdict = Verdict::inconclusive; p.note = turbo.packageWatts ? loadHeldBack : noPowerReading;
                p.before = turbo.packageWatts; p.mhz = turbo.mhz;
                results.push_back(p);
            }
            else {
                Result p; p.control = Control::powerLimit; p.before = turbo.packageWatts;
                p.status = asus::setTuning({asus::FanPreset::factory, asus::PowerPreset::saver}); pause(1200, o);
                const auto limited = measureLoad(4200);
                p.after = limited.packageWatts; p.mhz = limited.mhz;
                if (p.status != ERROR_SUCCESS) p.verdict = p.status == ERROR_NOT_SUPPORTED ? Verdict::unsupported : Verdict::refused;
                else if (!p.before || !p.after) { p.verdict = Verdict::writeOnly; p.note = noPowerReading; }
                else p.verdict = *p.after <= 30 && *p.after < *p.before * .9 ? Verdict::works : judgeMeasured(p.before, p.after, .8, true);
                // A load that never got near a limit says nothing about the limit.
                if (p.verdict != Verdict::works && (heldBack(turbo) || *p.before < 28)) { p.verdict = Verdict::inconclusive; p.note = loadHeldBack; }
                asus::setTuning({}); // back to factory (switches mode away and back)
                results.push_back(p);
                // Did the limit really go away? The same load again, in turbo, must come back to
                // where it was. If it does not, the limit is still in force until a restart.
                begin(Control::powerRestore);
                pause(1500, o);
                const auto restored = measureLoad(4200);
                Result back; back.control = Control::powerRestore; back.before = turbo.packageWatts; back.after = restored.packageWatts; back.mhz = restored.mhz;
                if (!back.before || !back.after) { back.verdict = Verdict::inconclusive; back.note = noPowerReading; }
                else if (p.verdict != Verdict::works) { back.verdict = Verdict::skipped; back.note = loadHeldBack; } // the limit showed no effect: nothing to see return
                else back.verdict = *back.after >= *back.before * .88 ? Verdict::works : Verdict::noEffect;
                results.push_back(back);
            }
            // Fan curve: the firmware's answer is all there is; the speed only shows as it warms up.
            begin(Control::fanCurve);
            if (!asus::fanCurvesSupported()) skip(Control::fanCurve, Verdict::unsupported, none);
            else if (cancelled()) skip(Control::fanCurve, Verdict::skipped, guardHeld);
            else {
                Result f; f.control = Control::fanCurve;
                f.status = asus::setTuning({asus::FanPreset::quiet, asus::PowerPreset::factory}); pause(500, o);
                f.verdict = f.status == ERROR_SUCCESS ? Verdict::writeOnly : f.status == ERROR_NOT_SUPPORTED ? Verdict::unsupported : Verdict::refused;
                f.note = needsTime;
                asus::setTuning({});
                results.push_back(f);
            }
            // Put the mode and the presets back as they were.
            asus::setMode(mode);
            if (tuning.fans != asus::FanPreset::factory || tuning.power != asus::PowerPreset::factory) { pause(1300, o); asus::setTuning(tuning); }
        }
        // The discrete GPU switch is read back every time it is used; it is not toggled here
        // (switching it closes programs that use it).
        {
            Result g; g.control = Control::gpuEco;
            g.verdict = snapshot.gpuEco ? Verdict::skipped : Verdict::unsupported;
            g.note = snapshot.gpuEco ? notToggled : none;
            if (snapshot.gpuEco) g.before = *snapshot.gpuEco ? 1 : 0;
            results.push_back(g);
        }
        // Charge limit: seen only over time.
        { Result c; c.control = Control::chargeLimit;
          const auto limit = asus::remembered().chargeLimit;
          if (!snapshot.chargeDevice) c.verdict = Verdict::unsupported;
          else if (!limit || !o.batteryPercent) { c.verdict = Verdict::skipped; c.note = needsTime; }
          else { c.before = *limit; c.after = *o.batteryPercent; c.verdict = judgeChargeLimit(*limit, *o.batteryPercent, o.pluggedIn, o.charging); if (c.verdict == Verdict::inconclusive) c.note = needsTime; }
          results.push_back(c); }
        // Keyboard backlight: sent to the keyboard, which reports nothing back.
        { Result k; k.control = Control::keyboard; k.verdict = snapshot.keyboardDevice ? Verdict::writeOnly : Verdict::unsupported; k.note = snapshot.keyboardDevice ? lookAtIt : none; results.push_back(k); }
    }
    stability::note(L"verify end");
    return results;
}

std::wstring saveText(const std::vector<Result>& results) {
    std::wstring text;
    for (const auto& r : results) {
        wchar_t line[200];
        swprintf(line, 200, L"%ls\t%ls\t%lu\t%ls\t%ls\t%d\t%ls\n", controlKey(r.control), verdictKey(r.verdict), static_cast<unsigned long>(r.status),
            r.before ? std::to_wstring(*r.before).c_str() : L"-", r.after ? std::to_wstring(*r.after).c_str() : L"-", r.note, r.mhz ? std::to_wstring(*r.mhz).c_str() : L"-");
        text += line;
    }
    return text;
}
std::vector<Result> loadText(const std::wstring& text) {
    std::vector<Result> results;
    std::wistringstream in(text);
    std::wstring line;
    while (std::getline(in, line)) {
        std::vector<std::wstring> parts; size_t start = 0;
        for (;;) { const size_t tab = line.find(L'\t', start); parts.push_back(line.substr(start, tab == std::wstring::npos ? tab : tab - start)); if (tab == std::wstring::npos) break; start = tab + 1; }
        if (parts.size() < 6) continue;
        Result r;
        for (int c = 0; c < int(Control::count); ++c) if (parts[0] == controlKey(Control(c))) r.control = Control(c);
        if (r.control == Control::count) continue;
        for (int v = 0; v <= int(Verdict::skipped); ++v) if (parts[1] == verdictKey(Verdict(v))) r.verdict = Verdict(v);
        r.status = DWORD(std::wcstoul(parts[2].c_str(), nullptr, 10));
        if (parts[3] != L"-") r.before = std::wcstod(parts[3].c_str(), nullptr);
        if (parts[4] != L"-") r.after = std::wcstod(parts[4].c_str(), nullptr);
        r.note = _wtoi(parts[5].c_str());
        if (parts.size() > 6 && parts[6] != L"-") r.mhz = std::wcstod(parts[6].c_str(), nullptr);
        results.push_back(r);
    }
    return results;
}

} // namespace bs::verify
