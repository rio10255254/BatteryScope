#include "../src/platform.h"
#include "../src/energy.h"
#include "../src/update.h"
#include "../src/lab.h"
#include "../src/governor.h"
#include "../src/asus.h"
#include "../src/stability.h"
#include "../src/motion.h"
#include "../src/i18n.h"
#include <shellapi.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <cctype>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <set>

using namespace bs;
unsigned passed = 0;
void check(bool condition, const char* name) {
    if (!condition) throw std::runtime_error(name);
    ++passed; std::cout << "PASS " << name << '\n';
}
bool nearlyEqual(Value value, double expected) { return value && std::abs(*value - expected) < .0001; }
PowerSample sample(uint64_t ms, Value watts = 10.0) {
    PowerSample p; p.ms = ms; p.present = p.acKnown = true;
    p.fullWh = 50; p.remainingWh = 25; p.percent = 50; p.watts = watts; return p;
}
// Reads a source file from the working directory: the repository root under
// CTest, or src/ under build.ps1.
std::string sourceText(const char* name) {
    for (std::string prefix : {"src/", ""}) {
        std::ifstream in(prefix + name, std::ios::binary);
        if (in) return std::string(std::istreambuf_iterator<char>(in), {});
    }
    throw std::runtime_error(std::string("source file not found from working directory: ") + name);
}
std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0);
    if (size <= 0) throw std::runtime_error("invalid UTF-8 in source literal");
    std::wstring result(size_t(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), result.data(), size);
    return result;
}
// Collects the wide string literal after every occurrence of `marker`; counts
// occurrences that are not a single literal so they cannot be skipped silently.
std::vector<std::wstring> sourceLiterals(const std::string& text, const std::string& marker, bool requireClose, size_t& unparsed) {
    std::vector<std::wstring> result;
    for (size_t at = text.find(marker); at != std::string::npos; at = text.find(marker, at + 1)) {
        if (at && (std::isalnum(static_cast<unsigned char>(text[at - 1])) || text[at - 1] == '_')) continue;
        std::string literal; size_t i = at + marker.size(); bool closed = false;
        for (; i < text.size(); ++i) {
            const char c = text[i];
            if (c == '\\' && i + 1 < text.size()) {
                const char n = text[++i];
                literal += n == 'n' ? '\n' : n == 'r' ? '\r' : n == 't' ? '\t' : n;
            } else if (c == '"') { closed = true; break; }
            else literal += c;
        }
        size_t after = i + 1;
        while (after < text.size() && text[after] == ' ') ++after;
        if (!closed || (requireClose && (after >= text.size() || text[after] != ')'))) { ++unparsed; continue; }
        result.push_back(widen(literal));
    }
    return result;
}
LRESULT CALLBACK childProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wp, lp);
}
int child(const wchar_t* eventName) {
    WNDCLASSW wc{}; wc.lpfnWndProc = childProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"BatteryScope.TestChild";
    RegisterClassW(&wc);
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"BatteryScope owned test process", WS_OVERLAPPEDWINDOW,
        -30000, -30000, 200, 100, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    Handle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName)); if (ready) SetEvent(ready);
    SetTimer(window, 1, 20000, nullptr);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_TIMER) DestroyWindow(window);
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    return 0;
}
struct OwnedChild {
    Handle process;
    ProcessRow row;
    explicit OwnedChild(int index) {
        std::wstring eventName = L"Local\\BatteryScopeTest." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(index);
        Handle event(CreateEventW(nullptr, TRUE, FALSE, eventName.c_str()));
        auto path = executablePath(); std::wstring command = L"\"" + path + L"\" --child " + eventName;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info)) throw std::runtime_error("spawn owned child");
        process.value = info.hProcess; CloseHandle(info.hThread);
        row.pid = info.dwProcessId; row.name = L"BatteryScopeTests.exe";
        FILETIME c{}, e{}, k{}, u{};
        if (!GetProcessTimes(process, &c, &e, &k, &u)) throw std::runtime_error("child identity");
        row.created = fileTime(c);
        if (WaitForSingleObject(event, 5000) != WAIT_OBJECT_0) throw std::runtime_error("child ready");
    }
    ~OwnedChild() {
        if (process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
            // Cleanup only the process handle this fixture created, never a discovered process.
            TerminateProcess(process, 99); WaitForSingleObject(process, 2000);
        }
    }
};
int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring(argv[1]) == L"--child") return child(argv[2]);
    if (argc == 2 && std::wstring(argv[1]) == L"--power-mode-cycle") {
        // Opt-in local integration check; regular CI tests never change system settings.
        const auto before = queryPowerModes();
        if (before.error || !before.dcKnown || before.restorable || before.dc == PowerMode::other) return 2;
        const PowerMode original = before.dc, other = original == PowerMode::efficiency ? PowerMode::balanced : PowerMode::efficiency;
        const DWORD change = setPowerMode(false, other);
        const auto changed = queryPowerModes();
        const DWORD restore = restorePowerModes();
        const auto restored = queryPowerModes();
        std::cout << "change=" << change << " changed=" << (changed.dc == other) << " restorable=" << changed.restorable
            << " restore=" << restore << " restored=" << (restored.dc == original) << '\n';
        return !change && changed.dc == other && changed.restorable && !restore && restored.dc == original && !restored.restorable ? 0 : 1;
    }
    try {
        Transition easing;
        easing.move(100, 1000, 180);
        check(easing.value(1000) == 0 && easing.velocity(1000) == 0, "motion starts at rest without a jump");
        const double position = easing.value(1060), speed = easing.velocity(1060);
        easing.move(200, 1060, 180);
        check(std::abs(easing.value(1060) - position) < 1e-9 && std::abs(easing.velocity(1060) - speed) < 1e-9,
            "same-direction scroll retarget preserves position and velocity");
        bool monotonic = true; double previous = easing.value(1060);
        for (int t = 1061; t <= 1240; ++t) { double v = easing.value(t); monotonic &= v >= previous && v <= 200; previous = v; }
        check(monotonic && easing.velocity(1240) == 0 && easing.value(1240) == 200, "motion settles exactly without overshoot");
        easing.move(300, 1300, 180); double reversal = easing.value(1380); easing.move(0, 1380, 180);
        check(easing.value(1380) == reversal && easing.value(1381) < reversal, "scroll reversal immediately follows input without a jump");
        easing.move(50, 1382, 0); check(easing.value(1382) == 50 && !easing.active(1382), "reduced motion resolves immediately");
        english = true; check(std::wstring(tr(L"充電功率")) == L"Charging power", "charging strings are localized");
        english = false; check(std::wstring(tr(L"充電功率")) == L"充電功率", "language switch returns to Chinese");
        {
            size_t unparsed = 0, keyUnparsed = 0;
            std::vector<std::wstring> missing;
            size_t literals = 0;
            for (const char* file : {"app.cpp", "platform.cpp"}) {
                for (const auto& literal : sourceLiterals(sourceText(file), "tr(L\"", true, unparsed)) {
                    ++literals;
                    if (!translations.count(literal)) missing.push_back(literal);
                }
            }
            for (const auto& literal : missing) {
                std::string utf8(size_t(WideCharToMultiByte(CP_UTF8, 0, literal.c_str(), int(literal.size()), nullptr, 0, nullptr, nullptr)), '\0');
                WideCharToMultiByte(CP_UTF8, 0, literal.c_str(), int(literal.size()), utf8.data(), int(utf8.size()), nullptr, nullptr);
                std::cerr << "untranslated: " << utf8 << '\n';
            }
            check(unparsed == 0 && literals > 150 && missing.empty(), "every tr() literal in the UI has an English translation");
            const auto keys = sourceLiterals(sourceText("i18n.h"), "{L\"", false, keyUnparsed);
            check(keyUnparsed == 0 && keys.size() == translations.size(), "translation table has no duplicate keys");
        }
        std::deque<PowerSample> charging;
        for (uint64_t t = 0; t <= 60000; t += 10000) {
            auto p = sample(t, -20); p.onAc = p.charging = true; p.remainingWh = 25 + t / 3600000.0 * 20;
            charging.push_back(p);
        }
        auto charge = calculateCharging(charging);
        check(nearlyEqual(charge.watts, 20) && nearlyEqual(charge.averageWatts, 20), "charging uses positive battery-side watts");
        check(nearlyEqual(charge.hoursToFull, (50 - *charging.back().remainingWh) / 20) && nearlyEqual(charge.percentPerHour, 40), "stable charging predicts time and percentage-point rate");
        check(!calculatePower(charging).currentWatts, "charging is never reported as whole-machine discharge");
        auto modified = charging; modified.pop_back();
        check(!calculateCharging(modified).hoursToFull, "charging ETA requires sixty continuous seconds");
        modified = charging; modified.back().charging = false;
        check(!calculateCharging(modified).watts && !calculateCharging(modified).hoursToFull, "plugged in without charging clears charging estimates");
        modified = charging; modified.back().onAc = false;
        check(!calculateCharging(modified).watts, "unplug clears charging measurements immediately");
        modified = charging; modified.back().watts.reset();
        check(!calculateCharging(modified).hoursToFull, "unknown charge rate cannot produce an ETA");
        modified = charging; modified[3].watts.reset();
        check(!calculateCharging(modified).hoursToFull, "missing sample breaks charging continuity");
        modified = charging; modified.back().ms += 60000;
        check(!calculateCharging(modified).hoursToFull, "sleep gap restarts charging warmup");
        modified = charging; modified.back().remainingWh = 45;
        check(!calculateCharging(modified).hoursToFull, "capacity recalibration invalidates charging ETA");
        modified = charging; modified.back().watts = -2;
        check(!calculateCharging(modified).hoursToFull, "sharp charge taper hides an unstable ETA");
        modified = charging; for (auto& p : modified) { p.fullWh.reset(); p.remainingWh.reset(); }
        check(!calculateCharging(modified).hoursToFull && calculateCharging(modified).watts.has_value(), "relative capacity retains watts but cannot predict time to full");
        modified = charging; modified.back().remainingWh = 50;
        check(!calculateCharging(modified).hoursToFull, "full battery has no future completion estimate");
        SamplingPolicy policy;
        check(policy.update(false, 1000, 2, 3) == 2, "foreground always samples at two seconds");
        check(policy.update(true, 2000, 2, 3) == 10, "background starts with responsive cadence");
        unsigned cadence = 0;
        for (uint64_t t = 12000; t <= 72000; t += 10000) cadence = policy.update(true, t, 2, 3);
        check(cadence == 20, "one quiet minute backs off to twenty seconds");
        check(policy.update(true, 92000, 2, 3) == 20, "adaptive interval stays below model continuity limit");
        check(policy.update(true, 112000, 15, 3) == 10, "load spike restores ten second cadence");
        check(policy.update(true, 122000, {}, {}) == 10, "unknown data never earns slow cadence");
        check(policy.update(false, 123000, 2, 3) == 2 && policy.update(true, 124000, 2, 3) == 10,
            "foreground transition discards stale quiet history");
        for (uint64_t t = 134000; t <= 194000; t += 10000) policy.update(true, t, 2, 3);
        check(policy.update(true, 234000, 2, 3) == 10, "sleep gap resets quiet history");
        check(policy.update(true, 244000, 2, 10) == 10, "core power jump prevents backoff");
        check(!calculatePower({}).currentWatts, "empty history");
        std::deque<PowerSample> h;
        for (uint64_t t = 1000; t <= 181000; t += 10000) appendPower(h, sample(t));
        auto m = calculatePower(h);
        check(nearlyEqual(m.currentWatts, 10) && nearlyEqual(m.averageWatts, 10), "measured discharge watts");
        check(nearlyEqual(m.percentPerHour, 20) && nearlyEqual(m.hoursRemaining, 2.5), "remaining energy / power and percentage rate");
        auto plugged = sample(191000, -25); plugged.onAc = plugged.charging = true; appendPower(h, plugged);
        m = calculatePower(h); check(!m.currentWatts && !m.averageWatts && !m.hoursRemaining, "charging watts are not system load");
        appendPower(h, sample(201000));
        check(!calculatePower(h).averageWatts, "unplug starts a fresh discharge segment");
        h.clear(); appendPower(h, sample(1000)); appendPower(h, sample(61000));
        check(!calculatePower(h).averageWatts, "sleep gap is not integrated");
        h.clear();
        for (uint64_t t = 0; t <= 180000; t += 10000) {
            auto p = sample(t, {}); p.remainingWh = 25 - t / 3600000.0 * 12; p.percent = *p.remainingWh * 2; appendPower(h, p);
        }
        m = calculatePower(h);
        check(m.estimated && nearlyEqual(m.currentWatts, 12), "missing sensor uses capacity slope after 2 minutes");
        h.erase(h.begin() + 10, h.end());
        check(!calculatePower(h).currentWatts, "short history never fabricates a watt estimate");
        h.clear();
        for (uint64_t t = 0; t <= 180000; t += 10000) {
            auto p = sample(t, {}); p.remainingWh.reset(); p.fullWh.reset(); p.percent = 50 - t / 180000.0; appendPower(h, p);
        }
        m = calculatePower(h);
        check(!m.currentWatts && nearlyEqual(m.percentPerHour, 20), "relative capacity can estimate percent but never watts");
        h.clear(); for (uint64_t t = 0; t <= 60000; t += 10000) appendPower(h, sample(t, 0));
        m = calculatePower(h); check(nearlyEqual(m.averageWatts, 0) && !m.hoursRemaining, "zero watts never divides by zero");
        auto changed = sample(70000); changed.remainingWh = 30; appendPower(h, changed);
        check(!calculatePower(h).averageWatts, "gauge recalibration starts a new segment");
        h.clear(); auto unknown = sample(0); unknown.acKnown = false; appendPower(h, unknown);
        check(!calculatePower(h).currentWatts, "unknown power source stays unknown");
        h.clear(); for (uint64_t t = 0; t <= 1900000; t += 10000) appendPower(h, sample(t));
        check(h.back().ms - h.front().ms <= 1800000 && h.size() <= 181, "bounded 30 minute history");
        auto count = h.size(); appendPower(h, sample(0)); check(h.size() == count, "reject out of order samples");
        check(std::abs(cpuPercent(0, 10000000, 2, 8) - 6.25) < .0001, "CPU normalized to all logical processors");
        check(cpuPercent(100, 10, 1, 8) == 0 && cpuPercent(0, 100, 0, 0) == 0, "CPU reset and invalid elapsed time");
        check(std::abs(smooth(0, 100, 30) - (1 - std::exp(-1.0)) * 100) < .0001, "time based activity smoothing");
        {
            const GUID efficiency{0x961cc777, 0x2547, 0x4f9d, {0x81, 0x74, 0x7d, 0x86, 0x18, 0x1b, 0x8a, 0x7a}};
            const GUID performance{0xded574b5, 0x45a0, 0x4f42, {0x87, 0x37, 0x46, 0x34, 0x5c, 0x09, 0xc2, 0x38}};
            const GUID legacy{0x3af9b8d9, 0x7c97, 0x431d, {0xad, 0x78, 0x34, 0xa8, 0xbf, 0xea, 0x43, 0x9f}};
            const GUID oem{0x12345678, 0x1234, 0x1234, {1, 2, 3, 4, 5, 6, 7, 8}};
            check(powerModeFromGuid(efficiency) == PowerMode::efficiency && powerModeFromGuid(GUID{}) == PowerMode::balanced &&
                powerModeFromGuid(performance) == PowerMode::performance && powerModeFromGuid(legacy) == PowerMode::balanced &&
                powerModeFromGuid(oem) == PowerMode::other, "power mode overlays map to efficiency, balanced, performance, or other");
            check(effectiveModeFromValue(0) == EffectiveMode::energySaver && effectiveModeFromValue(2) == EffectiveMode::balanced &&
                effectiveModeFromValue(4) == EffectiveMode::maxPerformance && effectiveModeFromValue(9) == EffectiveMode::unknown,
                "effective power mode values follow EFFECTIVE_POWER_MODE");
        }
        check(nearlyEqual(energyDeltaWatts(0,555555556,0,20000000),1), "EMI picowatt-hours and 100ns convert to watts");
        check(!energyDeltaWatts(100,50,0,20000000) && !energyDeltaWatts(0,100,20,10), "EMI resets and reverse time rejected");
        check(!energyDeltaWatts(0,100,0,400000000), "EMI sleep gap rejected");
        CpuEnergyReport aligned; aligned.ready = true; aligned.intervalSeconds = 2; aligned.sampleMs = 10000;
        alignProcessObservation(aligned, 2.03, 10020, true);
        aligned.observedCpuCoverage = .95;
        check(!aligned.timingMismatch && classifyObservation(aligned) == ObservationQuality::strong,
            "aligned system snapshot earns strong observability");
        CpuEnergyReport partial = aligned; partial.observedCpuCoverage = .7;
        check(classifyObservation(partial) == ObservationQuality::partial, "coverage classification is not a precision claim");
        CpuEnergyReport skewed = aligned; skewed.timingMismatch = false;
        alignProcessObservation(skewed, 2.4, 10400, true);
        check(skewed.timingMismatch && classifyObservation(skewed) == ObservationQuality::unavailable,
            "misaligned process and energy intervals are rejected");
        CpuAttributor alignmentGate; ProcessRow gated; gated.pid = 9; gated.created = 9; gated.measured = true; gated.cpu = 25;
        skewed.cores = {{2, 1}}; skewed.coreWatts = 2;
        std::vector<ProcessRow> gatedRows{gated}; alignmentGate.apply(gatedRows, skewed, 10000, 4);
        check(!gatedRows.front().cpuWatts, "attributor preserves an earlier alignment rejection");
        std::vector<CoreEnergy> heterogeneous{{4,1},{1,1}};
        auto allocation=allocateCpuEnergy(heterogeneous,.5,.5);
        check(nearlyEqual(allocation.watts,1.25) && nearlyEqual(allocation.low,.5) && nearlyEqual(allocation.high,2), "heterogeneous core assignment bounds");
        allocation=allocateCpuEnergy(heterogeneous,1.5,1.5);
        check(nearlyEqual(allocation.watts,3.75) && nearlyEqual(allocation.low,3) && nearlyEqual(allocation.high,4.5), "multi-core process assignment bounds");
        allocation=allocateCpuEnergy({{5,0},{1,1}},1,1);
        check(nearlyEqual(allocation.watts,1), "idle core power is not attributed to an unrelated process");
        double allocatedSum=0;
        for(int i=0;i<3;++i) allocatedSum+=allocateCpuEnergy(heterogeneous,1,3).watts;
        check(nearlyEqual(allocatedSum,5), "sampling skew cannot create more watts than the hardware budget");
        CpuAttributor estimator; CpuEnergyReport report; report.ready=true; report.source=EnergySource::perCore; report.coreWatts=5;
        report.cores=heterogeneous; report.intervalSeconds=2;
        ProcessRow a,b; a.pid=10; a.created=100; a.measured=true; a.cpu=12.5;
        b.pid=11; b.created=101; b.measured=true; b.cpu=25;
        std::vector<ProcessRow> energyRows{a,b};
        estimator.apply(energyRows,report,1000,4);
        check(energyRows[0].pid==11 && nearlyEqual(energyRows[0].cpuWatts,2.5), "process list is sorted by estimated watts descending");
        check(nearlyEqual(report.unassignedCpuWatts,1.25), "unobserved processes retain their unassigned energy budget");
        check(nearlyEqual(energyRows[0].cpuMilliWh,2.5*2/3.6), "per-process watt seconds integrate into milliwatt-hours");
        auto beforeEnergy=energyRows[0].cpuMilliWh;
        estimator.apply(energyRows,report,1000,4);
        check(nearlyEqual(energyRows[0].cpuMilliWh,*beforeEnergy), "replayed snapshot does not double count energy");
        report.ready=false; estimator.apply(energyRows,report,3000,4);
        check(!energyRows[0].cpuWatts, "missing sensor data hides estimates instead of showing stale watts");
        report.ready=true; report.cores={{1,.1}}; report.coreWatts=1;
        estimator.apply(energyRows,report,5000,4);
        check(report.timingMismatch && !energyRows[0].cpuWatts, "large CPU clock mismatch disables attribution");
        CpuAttributor constrained;
        report.cores=heterogeneous; report.coreWatts=5; report.ready=true; report.timingMismatch=false;
        a.cpu=25; a.fixedCoreBusy={0,1}; b.cpu=25; b.fixedCoreBusy={};
        energyRows={a,b}; constrained.apply(energyRows,report,1000,4);
        check(energyRows[0].pid==11 && nearlyEqual(energyRows[0].cpuWatts,4) && nearlyEqual(energyRows[1].cpuWatts,1), "fixed-core thread time uses that core's actual energy budget");
        check(nearlyEqual(report.unassignedCpuWatts,0), "fixed and unknown execution never double count core energy");
        check(nearlyEqual(energyRows[1].cpuWattsLow,1) && nearlyEqual(energyRows[1].cpuWattsHigh,1), "known placement removes only the placement uncertainty");
        a.fixedCoreBusy={0,3}; energyRows={a,b}; constrained.apply(energyRows,report,3000,4);
        check(*energyRows[0].cpuWatts+*energyRows[1].cpuWatts<=5.00001, "thread sampling skew remains within the hardware budget");
        // Hardware differences: Intel PP0, multi-package desktops, package-only EMI.
        auto channel=classifyChannel(L"RAPL_Package0_Core11_CORE");
        check(channel.kind==ChannelKind::core && channel.package==0 && channel.core==11, "AMD per-core channel is parsed");
        channel=classifyChannel(L"RAPL_Package1_PP0");
        check(channel.kind==ChannelKind::cores && channel.package==1, "Intel PP0 channel counts as all cores of its package");
        check(classifyChannel(L"rapl_package0_pkg").kind==ChannelKind::package, "channel names are case-insensitive");
        check(classifyChannel(L"RAPL_Package0_PP1").kind==ChannelKind::other && classifyChannel(L"RAPL_Package0_DRAM").kind==ChannelKind::other
            && classifyChannel(L"CPU_CLUSTER_0").kind==ChannelKind::other && classifyChannel(L"RAPL_PackageX_PKG").kind==ChannelKind::other,
            "graphics, memory and unknown channels are never treated as CPU energy");
        check(nearlyEqual(busyProcessors(100,200,300, 150,300,400, 8),6) && !busyProcessors(150,300,400, 100,200,300, 8)
            && !busyProcessors(0,0,0, 50,40,0, 8), "system-wide busy processors come from GetSystemTimes deltas and reject bad readings");
        PackageFloor floor(600000);
        check(nearlyEqual(floor.update(0,9),9) && nearlyEqual(floor.update(1000,4),4) && nearlyEqual(floor.update(2000,7),4),
            "package idle floor is the lowest recent package power");
        check(nearlyEqual(floor.update(601500,8),7), "package idle floor forgets readings outside its window");
        CpuAttributor pooled; CpuEnergyReport allCores;
        setAggregatePool(allCores,EnergySource::coreTotal,6,2,.8,1.25); allCores.intervalSeconds=2;
        a.cpu=25; a.fixedCoreBusy={0,1}; b.cpu=25; b.fixedCoreBusy={};
        energyRows={a,b}; pooled.apply(energyRows,allCores,1000,4);
        check(nearlyEqual(energyRows[0].cpuWatts,3) && nearlyEqual(energyRows[1].cpuWatts,3),
            "without per-core energy, core placement hints are ignored and the pool is split by CPU time");
        check(nearlyEqual(energyRows[0].cpuWattsLow,2.4) && nearlyEqual(energyRows[0].cpuWattsHigh,3.75),
            "aggregate core energy carries the documented range factors");
        CpuAttributor packaged; CpuEnergyReport package;
        setAggregatePool(package,EnergySource::package,4,2,1,2); package.intervalSeconds=2;
        energyRows={a,b}; packaged.apply(energyRows,package,1000,4);
        check(nearlyEqual(energyRows[0].cpuWatts,2) && nearlyEqual(energyRows[0].cpuWattsHigh,4) && nearlyEqual(package.unassignedCpuWatts,0),
            "package-only energy attributes the share above the idle floor, with the floor as the upper range");
        // Updates and installer handoff.
        check(update::isNewer(L"v0.12.0", L"0.11.0") && update::isNewer(L"0.11.1", L"0.11.0") && update::isNewer(L"1.0", L"0.99.9")
            && !update::isNewer(L"v0.11.0", L"0.11.0") && !update::isNewer(L"0.10.9", L"0.11.0") && !update::isNewer(L"0.11.0.0", L"0.11.0"),
            "release versions compare numerically, not as text");
        check(!update::isNewer(L"v0.12.0-beta", L"0.11.0") && !update::isNewer(L"", L"0.11.0") && !update::isNewer(L"latest", L"0.11.0"),
            "unparsable release tags never count as updates");
        const std::string feed = R"({"html_url":"https://github.com/rio10255254/BatteryScope/releases/tag/v0.12.0","tag_name":"v0.12.0",
            "assets":[{"name":"BatteryScope-0.12.0-win64.zip","browser_download_url":"https://github.com/rio10255254/BatteryScope/releases/download/v0.12.0/BatteryScope-0.12.0-win64.zip"},
            {"name":"BatteryScope-0.12.0-setup.exe","browser_download_url":"https://github.com/rio10255254/BatteryScope/releases/download/v0.12.0/BatteryScope-0.12.0-setup.exe"},
            {"name":"BatteryScope-0.12.0-setup.exe.sha256","browser_download_url":"https://github.com/rio10255254/BatteryScope/releases/download/v0.12.0/BatteryScope-0.12.0-setup.exe.sha256"}]})";
        auto release = update::parseRelease(feed);
        check(release && release->version == L"0.12.0" && release->setupUrl.size() > 20
            && release->setupHashUrl.substr(release->setupHashUrl.size() - 7) == L".sha256" && release->page.find(L"v0.12.0") != std::wstring::npos,
            "release feed yields the version, page and setup assets");
        auto foreign = update::parseRelease(R"({"tag_name":"v0.12.0","html_url":"http://example.com/x","assets":[{"browser_download_url":"https://evil.example/BatteryScope-0.12.0-setup.exe"}]})");
        check(foreign && foreign->setupUrl.empty() && foreign->page == update::ReleasesPage,
            "setup downloads and pages are only taken from github.com over HTTPS");
        check(!update::parseRelease(R"({"message":"Not Found"})"), "a feed without a release tag is ignored");
        const std::string digest(64, 'a');
        check(update::parseSha256File(digest + "  BatteryScope-0.12.0-setup.exe\n") == digest && update::parseSha256File(std::string(64, 'A')) == digest
            && update::parseSha256File("abc").empty() && update::parseSha256File(std::string(63, 'a') + "g").empty() && update::parseSha256File(digest + "a").empty(),
            "checksum files must start with exactly 64 hex digits");
        // Power lab: activity, schedule, blocks and paired comparison.
        lab::Signals signals; signals.idleSeconds = 300;
        check(lab::classify(signals) == lab::Context::idle, "no input for minutes is idle");
        signals.audio = true;
        check(lab::classify(signals) == lab::Context::media, "sound without input is watching or listening");
        signals.call = true;
        check(lab::classify(signals) == lab::Context::call, "an active microphone or camera is a call, ahead of media");
        lab::Signals working; working.idleSeconds = 2; working.systemCpu = 40.0;
        check(lab::classify(working) == lab::Context::heavy, "sustained CPU load is heavy work");
        working.systemCpu = 5.0;
        check(lab::classify(working) == lab::Context::interactive, "recent input with light load is interactive");
        working.fullscreenGame = true;
        check(lab::classify(working) == lab::Context::game, "a full-screen Direct3D app is a game");
        lab::Schedule schedule(12345);
        bool balanced = true, bothOrders[2] = {false, false};
        for (int pair = 0; pair < 40; ++pair) {
            const int a = schedule.armFor(pair * 2), b = schedule.armFor(pair * 2 + 1);
            balanced &= a != b && (a == 0 || a == 1);
            bothOrders[a] = true;
        }
        check(balanced && bothOrders[0] && bothOrders[1] && schedule.armFor(7) == lab::Schedule(12345).armFor(7),
            "every pair runs both arms once, in a reproducible random order");
        auto labSample = [](uint64_t ms, double battery, double package, double core, lab::Context context) {
            lab::Sample s; s.ms = ms; s.onBattery = true; s.batteryWatts = battery; s.packageWatts = package; s.coreWatts = core;
            s.systemCpu = 10.0; s.context = context; s.framesDropped = 1; s.framesComposed = 60; return s;
        };
        lab::BlockRecorder recorder; recorder.begin(3, 1, 100000);
        check(!recorder.add(labSample(110000, 50, 5, 2, lab::Context::interactive), 2), "the first 20 seconds of a block are warm-up");
        for (uint64_t t = 120000; t < 220000; t += 2000) recorder.add(labSample(t, 8, 3, 1, t < 170000 ? lab::Context::interactive : lab::Context::idle), 2);
        lab::Block block;
        check(recorder.finish(block) && block.pair == 3 && block.arm == 1 && std::abs(block.battery - 8) < 1e-9 && std::abs(block.rest - 5) < 1e-9
            && std::abs(block.dropsPerMinute - 30) < 1e-9 && std::abs(block.contextShare[size_t(lab::Context::idle)] - .5) < 1e-9,
            "a block averages power after warm-up and records drops and activity mix");
        lab::Sample pluggedSample = labSample(230000, 8, 3, 1, lab::Context::idle); pluggedSample.onBattery = false;
        check(!recorder.add(pluggedSample, 2), "plugged-in samples never count");
        recorder.invalidate(); lab::Block rejected;
        check(!recorder.finish(rejected), "a block changed by another program is discarded");
        lab::BlockRecorder brief; brief.begin(0, 0, 0);
        for (uint64_t t = 20000; t < 60000; t += 2000) brief.add(labSample(t, 8, 3, 1, lab::Context::idle), 2);
        check(!brief.finish(rejected), "a block needs a minute of battery data");
        block.experiment = L"cpu-boost"; block.run = 77; block.armName = L"Boost 關"; block.started = L"2026-11-10 10:00:00";
        lab::Block parsed;
        check(lab::Block::parse(block.csv(), parsed) && parsed.armName == L"Boost 關" && parsed.run == 77 && std::abs(parsed.battery - 8) < 1e-6
            && !lab::Block::parse(lab::Block::csvHeader(), parsed), "blocks round-trip through their CSV file, Chinese names included");
        auto comparison = lab::comparePairs({{10, 9}, {11, 10}, {9.5, 8.5}, {10.5, 9.5}});
        check(std::abs(comparison.difference + 1) < 1e-9 && comparison.halfWidth < 1e-9 && comparison.significant(),
            "identical savings in every pair are a certain difference");
        comparison = lab::comparePairs({{10, 9}, {10, 11}, {10, 9.5}, {10, 10.5}});
        check(std::abs(comparison.difference) < 1e-9 && !comparison.significant(), "savings that cancel out are not a difference");
        comparison = lab::comparePairs({{10, 9}, {10, 9.4}, {10, 8.8}});
        check(std::abs(comparison.difference + 2.8 / 3) < 1e-9 && std::abs(comparison.halfWidth - 4.303 * .3055050463 / std::sqrt(3.0)) < 1e-4,
            "the interval uses Student's t for the number of pairs");
        check(!lab::comparePairs({{10, 9}}).significant(), "one pair is never enough");
        std::vector<lab::Block> blocks;
        for (int pair = 0; pair < 3; ++pair) for (int arm = 0; arm < 2; ++arm) {
            lab::Block b; b.experiment = L"refresh"; b.run = 1; b.pair = pair; b.arm = arm; b.armName = arm ? L"60 Hz" : L"120 Hz";
            b.battery = arm ? 7 : 8; b.seconds = 100; blocks.push_back(b);
        }
        lab::Block orphan = blocks.front(); orphan.pair = 9; blocks.push_back(orphan);
        auto results = lab::analyze(blocks);
        check(results.size() == 1 && results[0].battery.pairs == 3 && std::abs(results[0].battery.difference + 1) < 1e-9
            && results[0].arm1 == L"60 Hz", "results pair blocks by run and pair, skipping incomplete pairs");
        for (int pair = 0; pair < 3; ++pair) for (int arm = 0; arm < 2; ++arm) {
            lab::Block b; b.experiment = L"refresh"; b.run = 2; b.pair = pair; b.arm = arm; b.armName = arm ? L"60 Hz" : L"90 Hz";
            b.battery = arm ? 7 : 7.5; b.seconds = 100; blocks.push_back(b);
        }
        results = lab::analyze(blocks);
        check(results.size() == 2 && results[0].battery.pairs == 3 && results[1].battery.pairs == 3,
            "runs that compared different settings are not pooled into one result");
        lab::Breakdown breakdown;
        auto onBattery = labSample(0, 9, 4, 1.5, lab::Context::interactive); onBattery.dgpuState = 0; onBattery.dgpuApps = L"game.exe;chrome.exe";
        breakdown.add(onBattery, 10); breakdown.add(pluggedSample, 10);
        check(std::abs(breakdown.totals(lab::Context::interactive).rest / breakdown.totals(lab::Context::interactive).restSeconds - 5) < 1e-9
            && breakdown.batterySeconds() == 10 && breakdown.dgpuAwakeSeconds() == 10 && breakdown.dgpuApps(5).size() == 2,
            "the breakdown splits battery power on battery only and tracks the discrete GPU");
        const std::string row = lab::csvRow(onBattery);
        check(lab::csvHeader().find("rest_w") != std::string::npos && row.find("game.exe;chrome.exe") != std::string::npos && row.find("5.000") != std::string::npos,
            "trace rows carry the derived rest-of-system power and GPU apps");
        // The experiment controller against a fake setting: alternation, pause on AC, outside changes, restore.
        struct FakeActuator : lab::Actuator {
            DWORD value = 2, applied = 0; bool remembered = false;
            lab::KnobPlan plan(lab::Knob) override {
                lab::KnobPlan p; p.available = true; p.original = value; p.values = {2, 0}; p.names = {L"Boost 開", L"Boost 關"}; return p;
            }
            DWORD apply(lab::Knob, DWORD v) override { value = v; ++applied; return ERROR_SUCCESS; }
            DWORD read(lab::Knob, DWORD& v) override { v = value; return ERROR_SUCCESS; }
            void remember(lab::Knob, DWORD) override { remembered = true; }
            void forget() override { remembered = false; }
        };
        auto fake = std::make_shared<FakeActuator>();
        lab::Controller controller(L"", fake);
        check(controller.start(lab::Knob::cpuBoost) == ERROR_SUCCESS && fake->remembered && controller.active(),
            "starting an experiment records the original setting for crash recovery");
        uint64_t clock = 1000;
        auto feedLab = [&](bool battery, double seconds) {
            for (double elapsed = 0; elapsed < seconds; elapsed += 2) {
                lab::Sample s = labSample(clock, 8, 3, 1, lab::Context::interactive); s.onBattery = battery;
                if (!battery) s.batteryWatts.reset();
                controller.onSample(s, 2); clock += 2000;
            }
        };
        feedLab(true, 2);
        const DWORD firstArm = fake->value;
        feedLab(true, 124);
        check(fake->value != firstArm && controller.status().pairsDone == 0 && controller.status().block == 1,
            "after a two-minute block the other setting is applied");
        feedLab(false, 10);
        check(fake->value == 2 && controller.status().paused, "plugging in pauses and restores the original setting");
        feedLab(true, 2);
        check(fake->value != firstArm, "unplugging redoes the interrupted block with the same setting");
        fake->value = fake->value ? 0 : 2; // another program flips it
        feedLab(true, 2);
        check(controller.status().externalChanges == 1 && fake->value != firstArm, "an outside change is counted and the setting is put back");
        feedLab(true, 130);
        check(controller.status().block == 1, "a block changed from outside is repeated, not counted");
        for (int i = 0; i < 20 && controller.status().running; ++i) feedLab(true, 126);
        check(!controller.status().running && controller.status().finished && controller.status().pairsDone == lab::Controller::TargetPairs
            && fake->value == 2 && !fake->remembered, "eight complete pairs finish the run and restore the original");
        auto labResults = controller.status().results;
        check(labResults.size() == 1 && labResults[0].battery.pairs == lab::Controller::TargetPairs && std::abs(labResults[0].battery.difference) < 1e-9,
            "the finished run is analyzed as paired blocks");
        lab::Controller stopped(L"", fake);
        stopped.start(lab::Knob::cpuBoost);
        fake->value = 2; clock += 1000;
        lab::Sample s1 = labSample(clock, 8, 3, 1, lab::Context::idle); stopped.onSample(s1, 2);
        stopped.stop();
        check(fake->value == 2 && !stopped.active() && !fake->remembered, "stopping restores the original at once");
        // Automatic test plan and its preparation phase.
        const auto browserPlan = lab::automaticPlan(true), builtInPlan = lab::automaticPlan(false);
        auto noScreenOff = [](const std::vector<lab::Step>& plan) { return std::none_of(plan.begin(), plan.end(), [](const lab::Step& step) { return step.knob == lab::Knob::display; }); };
        check(browserPlan.size() == 19 && noScreenOff(browserPlan) && noScreenOff(builtInPlan)
            && std::any_of(browserPlan.begin(), browserPlan.end(), [](const lab::Step& step) { return step.knob == lab::Knob::powerMode && step.workload == lab::Workload::sustained; })
            && std::all_of(browserPlan.begin(), browserPlan.end(), [](const lab::Step& step) { return step.pairs == 3 && step.blockSeconds == 45; }),
            "the automatic test covers every setting it can measure, without screen-off (standby would end it)");
        {
            const auto quick = lab::quickPlan(true);
            double seconds = 0; for (const auto& step : quick) seconds += step.prepareSeconds + step.pairs * 2 * step.blockSeconds;
            check(quick.size() == 9 && seconds <= 30 * 60 && noScreenOff(quick)
                && std::any_of(quick.begin(), quick.end(), [](const lab::Step& step) { return step.workload == lab::Workload::video; })
                && std::any_of(quick.begin(), quick.end(), [](const lab::Step& step) { return step.knob == lab::Knob::darkMode; }),
                "the quick test takes under half an hour and still covers every activity and dark mode");
        }
        {
            // ASUS firmware answers: bit 16 marks a present device, the value is the low 16 bits.
            check(asus::decode(0x00010002).present && asus::decode(0x00010002).value() == 2 && !asus::decode(0xFFFFFFFE).present && !asus::decode(0x2).present,
                "ASUS firmware readings are decoded and absent devices recognised");
            using Bytes = std::vector<uint8_t>;
            check(asus::packet::keyboardBrightness(0x5A, 2) == Bytes{0x5A, 0xBA, 0xC5, 0xC4, 0x02} && asus::packet::keyboardBrightness(0x5A, 9).back() == 3,
                "the keyboard backlight packet is G-Helper's, clamped to level 3");
            check(asus::packet::fnLock(true) == Bytes{0x5A, 0xD0, 0x4E, 0x00} && asus::packet::fnLock(false) == Bytes{0x5A, 0xD0, 0x4E, 0x01}
                && asus::packet::touchpad() == Bytes{0x5A, 0xF4, 0x6B}, "Fn lock and touchpad packets match G-Helper's");
            const Bytes greeting = asus::packet::init(0x5A);
            check(greeting.size() == 15 && greeting[0] == 0x5A && std::string(greeting.begin() + 1, greeting.end()) == "ASUS Tech.Inc.", "the keyboard greeting is the report id and \"ASUS Tech.Inc.\"");
            asus::Slash slashOn; slashOn.mode = asus::SlashMode::bounce; slashOn.brightness = 2; slashOn.interval = 1; slashOn.offOnBattery = true;
            const auto lit = asus::packet::slash(0x5E, slashOn);
            check(lit.size() == 10 && lit[0] == asus::packet::init(0x5E) && lit[1] == Bytes{0x5E, 0xC2} && lit[2] == Bytes{0x5E, 0xD1, 0x01, 0x00, 0x01}
                && lit[3] == Bytes{0x5E, 0xD8, 0x02, 0x00, 0x01, 0x00} && lit[4] == Bytes{0x5E, 0xD8, 0x01, 0x00, 0x01, 0x80}
                && lit[5] == Bytes{0x5E, 0xD7, 0x00, 0x00, 0x01, 0xAC} && lit[6] == Bytes{0x5E, 0xD2, 0x02, 0x01, 0x08, 0xAB}
                && lit[7] == Bytes{0x5E, 0xD3, 0x04, 0x00, 0x0C, 0x01, 0x10, 0x02, 0x42, 0x03, 0x13, 0x04, 0x11, 0x05, 0x12, 0x06, 0x13}
                && lit[8] == Bytes{0x5E, 0xD3, 0x03, 0x01, 0x08, 0xAB, 0xFF, 0x01, 0x01, 0x06, 170, 0xFF, 0x01} && lit[9] == Bytes{0x5E, 0xD4, 0x00, 0x00, 0x01, 0xAB},
                "Slash lighting: wake, on, battery flag, set up, mode, options and save, byte for byte as G-Helper sends them");
            asus::Slash slashOff; slashOff.on = false;
            const auto dark = asus::packet::slash(0x5D, slashOff);
            check(dark.size() == 5 && dark[3] == Bytes{0x5D, 0xD8, 0x02, 0x00, 0x01, 0x80} && dark[4] == Bytes{0x5D, 0xD8, 0x01, 0x00, 0x01, 0x00},
                "turning the Slash off only wakes it and switches it off");
            check(asus::slashModes().size() == 16 && std::none_of(asus::slashModes().begin(), asus::slashModes().end(), [](asus::SlashMode m) { return std::wstring(asus::slashModeName(m)) == L"?"; }),
                "every Slash mode has a name");
            // Fan curves: 8 temperatures then 8 duties, as the firmware hands them out; power presets stay at or below balanced.
            check(asus::validFanCurve({30, 40, 50, 60, 70, 80, 90, 100, 0, 0, 10, 20, 35, 50, 70, 90})
                && !asus::validFanCurve(Bytes(16, 0)) && !asus::validFanCurve({30, 40, 50}) && !asus::validFanCurve({30, 40, 50, 60, 70, 80, 90, 200, 0, 0, 10, 20, 35, 50, 70, 90})
                && !asus::validFanCurve({30, 40, 50, 60, 70, 80, 90, 100, 0, 0, 10, 20, 35, 50, 70, 150}), "only plausible fan curves are written");
            const auto saver = asus::powerLimits(asus::PowerPreset::saver), standard = asus::powerLimits(asus::PowerPreset::standard);
            check(asus::powerLimits(asus::PowerPreset::factory).spl == 0 && saver.spl < saver.sppt && saver.sppt < saver.fppt
                && standard.spl <= 45 && standard.fppt <= 65 && asus::powerLimits(asus::PowerPreset::light).spl > saver.spl,
                "CPU power presets rise in order and stay within the balanced range");
            check(asus::nextMode(asus::Mode::silent) == asus::Mode::balanced && asus::nextMode(asus::Mode::balanced) == asus::Mode::turbo && asus::nextMode(asus::Mode::turbo) == asus::Mode::silent,
                "Fn+F5 cycles silent, balanced, turbo");
            check(asus::key::optimization(asus::key::BrightnessUp) && asus::key::optimization(asus::key::Touchpad) && !asus::key::optimization(asus::key::Mode)
                && !asus::key::optimization(asus::key::Rog), "brightness and touchpad keys belong to ASUS Optimization, Fn+F5 and ROG to Armoury Crate");
            for (unsigned code : {asus::key::BrightnessDown, asus::key::BrightnessUp, asus::key::KeyboardUp, asus::key::KeyboardDown, asus::key::KeyboardCycle, asus::key::Mode,
                    asus::key::Rog, asus::key::MicMute, asus::key::Touchpad, asus::key::Sleep, asus::key::Airplane, asus::key::FnLock, asus::key::FnC, asus::key::Aura, asus::key::Emoji})
                check(asus::keyName(code) != nullptr, "every handled ASUS key has a name");
            check(lab::knobKey(lab::Knob::asusMode) == std::wstring(L"asus-mode") && gov::knobFor(gov::Setting::firmwareMode) == lab::Knob::asusMode,
                "the ASUS performance mode is both an experiment and a dynamic saving setting");
            gov::Governor fw;
            check(fw.profileFor(gov::Activity::reading, 0).values[size_t(gov::Setting::firmwareMode)] == DWORD(2)
                && fw.profileFor(gov::Activity::heavy, 0).values[size_t(gov::Setting::firmwareMode)] == DWORD(0)
                && !fw.profileFor(gov::Activity::game, 0).values[size_t(gov::Setting::firmwareMode)],
                "silent firmware mode for light use, balanced for heavy work, untouched in games");
            lab::ExperimentResult silent; silent.experiment = L"asus-mode@browse"; silent.arm0 = L"平衡"; silent.arm1 = L"靜音";
            silent.battery = lab::comparePairs({{14, 12.5}, {14.2, 12.6}, {13.9, 12.4}});
            const auto silentEvidence = gov::learn({silent});
            check(!silentEvidence.empty() && silentEvidence[0].setting == gov::Setting::firmwareMode && silentEvidence[0].savingValue == 2 && silentEvidence[0].approved,
                "a measured silent-mode saving becomes evidence for the firmware setting");
        }
        // ---- Stability monitor.
        {
            using namespace bs::stability;
            SYSTEMTIME st{2026, 11, 4, 12, 8, 9, 31, 500}; FILETIME ft{}; SystemTimeToFileTime(&st, &ft);
            const uint64_t expected = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
            check(parseIsoUtc(L"2026-11-12T08:09:31.5000000Z") == expected && parseIsoUtc(L"1601-01-01T00:00:00Z") == 0ull && !parseIsoUtc(L"garbage"),
                "event times parse to FILETIME ticks");
            check(std::wstring(bugcheckName(0x154)) == L"UNEXPECTED_STORE_EXCEPTION" && bugcheckArea(0x154) == Area::storage &&
                normalizeBugcheck(0x1000007E) == 0x7E && bugcheckArea(0xEF) == Area::system && bugcheckArea(0x116) == Area::graphics && !bugcheckName(0x12345),
                "stop codes are named and grouped");
            Record r; r.seq = 42; r.time = expected; r.boot = expected - 100; r.pid = 1234; r.text = L"hb ac=0 pct=74 act=瀏覽 fg=msedge.exe";
            const std::string slot = encodeSlot(r);
            const auto back = decodeSlot(slot.data(), slot.size());
            check(slot.size() == SlotBytes && back && back->seq == 42 && back->time == expected && back->pid == 1234 && back->text == r.text,
                "black box slots round-trip, Chinese included");
            std::string torn = slot; torn[60] = torn[60] == 'x' ? 'y' : 'x';
            Record longRecord = r; longRecord.text = std::wstring(900, L'字');
            const std::string longSlot = encodeSlot(longRecord);
            const auto longBack = decodeSlot(longSlot.data(), longSlot.size());
            check(!decodeSlot(torn.data(), torn.size()) && longSlot.size() == SlotBytes && longBack && !longBack->text.empty() && longBack->text.size() < 900,
                "torn slots are rejected and long lines are cut on a character boundary");
            const auto f = fields(L"hb ac=0 pct=74 w=9.8 fg=a_b.exe");
            check(f.at(L"ac") == L"0" && f.at(L"pct") == L"74" && f.at(L"fg") == L"a_b.exe" && !f.count(L"hb"), "heartbeat fields parse");

            wchar_t temp[MAX_PATH]; GetTempPathW(MAX_PATH, temp);
            const std::wstring boxPath = std::wstring(temp) + L"bs-blackbox-test.bin";
            DeleteFileW(boxPath.c_str());
            {
                BlackBox box; check(box.open(boxPath), "black box opens");
                for (int i = 0; i < int(BlackBox::Slots) + 10; ++i) box.write(L"hb n=" + std::to_wstring(i));
                const auto all = box.records();
                check(all.size() == BlackBox::Slots && all.front().text == L"hb n=10" && all.back().text == L"hb n=" + std::to_wstring(BlackBox::Slots + 9),
                    "the black box keeps the newest records in order when it wraps");
                box.close();
                BlackBox again; again.open(boxPath); again.write(L"start");
                const auto more = again.records();
                check(more.back().text == L"start" && more.back().seq == all.back().seq + 1, "a reopened black box continues the sequence");
                again.close();
            }
            DeleteFileW(boxPath.c_str());

            auto rec = [](uint64_t seq, uint64_t time, const wchar_t* text) { Record x; x.seq = seq; x.time = time; x.boot = 5; x.pid = 7; x.text = text; return x; };
            const uint64_t t0 = 100000 * Second;
            std::vector<Record> session{rec(1, t0, L"start v=1"), rec(2, t0 + 10 * Second, L"hb ac=0 pct=55 w=9.0"), rec(3, t0 + 20 * Second, L"ev acpi 0x00110024=1E")};
            check(classify(session, t0 + 3600 * Second).ending == Ending::systemDown && classify(session, t0 - 3600 * Second).ending == Ending::appEnded,
                "a later boot means the system went down; the same boot means the app ended");
            auto clean = session; clean.push_back(rec(4, t0 + 30 * Second, L"exit quit"));
            auto asleep = session; asleep.push_back(rec(4, t0 + 30 * Second, L"ev suspend"));
            auto empty = session; empty.push_back(rec(4, t0 + 30 * Second, L"hb ac=0 pct=2 w=7.0"));
            auto hung = session; hung.push_back(rec(4, t0 + 30 * Second, L"ev hang ui 31s"));
            const auto previous = classify(session, t0 + 3600 * Second);
            check(classify(clean, t0 + 3600 * Second).ending == Ending::clean && classify(asleep, t0 + 3600 * Second).ending == Ending::systemDownAsleep &&
                classify(empty, t0 + 3600 * Second).ending == Ending::batteryEmpty && classify(hung, t0 - 3600 * Second).ending == Ending::appHung &&
                previous.lastHeartbeat == L"ac=0 pct=55 w=9.0" && previous.tail.size() == 3,
                "the previous session's ending: clean, asleep, battery empty, hung");

            const std::wstring kp41 = L"<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'><System><Provider Name='Microsoft-Windows-Kernel-Power' Guid='{331c3b3a}'/>"
                L"<EventID>41</EventID><Version>9</Version><Level>1</Level><TimeCreated SystemTime='2026-11-12T08:09:31.5000000Z'/><Channel>System</Channel></System>"
                L"<EventData><Data Name='BugcheckCode'>340</Data><Data Name='BugcheckParameter1'>0xffffb10c7a5e8000</Data><Data Name='BugcheckParameter2'>0x1</Data>"
                L"<Data Name='BugcheckParameter3'>0x2</Data><Data Name='BugcheckParameter4'>0x0</Data><Data Name='SleepInProgress'>0</Data><Data Name='PowerButtonTimestamp'>0</Data>"
                L"<Data Name='ConnectedStandbyInProgress'>false</Data><Data Name='CsEntryScenarioInstanceId'/><Data Name='LongPowerButtonPressDetected'>false</Data></EventData></Event>";
            const auto e41 = parseEventXml(kp41);
            check(e41 && e41->provider == L"Microsoft-Windows-Kernel-Power" && e41->id == 41 && e41->level == 1 && e41->channel == L"System" && e41->time == expected &&
                e41->value(L"BugcheckCode") == L"340" && e41->value(L"CsEntryScenarioInstanceId").empty() && e41->value(L"LongPowerButtonPressDetected") == L"false",
                "a Kernel-Power 41 record parses");
            auto event = [](const wchar_t* provider, uint32_t id, uint64_t time, std::vector<std::pair<std::wstring, std::wstring>> data) {
                LogEvent e; e.provider = provider; e.id = id; e.time = time; e.data = std::move(data); return e;
            };
            const auto bugcheck = event(L"Microsoft-Windows-WER-SystemErrorReporting", 1001, expected + 40 * Second,
                {{L"param1", L"0x00000154 (0xffffb10c7a5e8000, 0x1, 0x2, 0x0)"}, {L"param2", L"C:\\Windows\\MEMORY.DMP"}});
            const auto unexpected = event(L"EventLog", 6008, expected + 2 * Second, {{L"", L"16:08:55"}});
            const auto one = incidentsFromEvents({*e41, bugcheck, unexpected});
            check(one.size() == 1 && one[0].kind == Kind::blueScreen && one[0].code == 0x154 && one[0].time == expected,
                "41, BugCheck 1001 and 6008 of one crash become one blue screen");
            auto heldOff = *e41; heldOff.data = {{L"BugcheckCode", L"0"}, {L"LongPowerButtonPressDetected", L"true"}};
            auto cut = *e41; cut.data = {{L"BugcheckCode", L"0"}, {L"PowerButtonTimestamp", L"0"}}; cut.time = expected + 86400 * Second;
            auto held = incidentsFromEvents({heldOff}), lost = incidentsFromEvents({cut});
            check(held.size() == 1 && held[0].kind == Kind::forcedOff && lost.size() == 1 && lost[0].kind == Kind::powerLoss,
                "no stop code: a held power button is a forced shutdown, otherwise a power loss");
            const std::wstring wer = L"<Event><System><Provider Name='Windows Error Reporting'/><EventID Qualifiers='0'>1001</EventID><Level>4</Level>"
                L"<TimeCreated SystemTime='2026-11-12T09:00:00.0000000Z'/><Channel>Application</Channel></System><EventData><Data>0</Data><Data></Data>"
                L"<Data>LiveKernelEvent</Data><Data>Not available</Data><Data>0</Data><Data>141</Data><Data>ffff8a8f</Data></EventData></Event>";
            const auto live = parseEventXml(wer);
            const auto display = event(L"Display", 4101, expected + 3 * 86400 * Second, {{L"", L"amdkmdag"}, {L"", L""}});
            const auto other = event(L"Application Error", 1000, expected + 4 * 86400 * Second, {{L"AppName", L"game.exe"}, {L"ExceptionCode", L"c0000005"}});
            const auto mine = event(L"Application Error", 1000, expected + 5 * 86400 * Second, {{L"", L"BatteryScope.exe"}, {L"", L"0.23"}, {L"", L"0"}, {L"", L"x"}, {L"", L"0"}, {L"", L"0"}, {L"", L"c0000005"}});
            const auto mixed = incidentsFromEvents({*live, display, other, mine});
            check(live && live->data.size() == 7 && mixed.size() == 3 && mixed[0].kind == Kind::graphicsReset && mixed[0].detail == L"LiveKernelEvent 141" &&
                mixed[1].kind == Kind::graphicsReset && mixed[1].detail == L"amdkmdag 4101" && mixed[2].kind == Kind::appCrash && mixed[2].detail == L"exception c0000005",
                "live kernel dumps and display resets are graphics resets; only BatteryScope's own crashes count");
            std::vector<LogEvent> whea;
            for (int i = 0; i < 30; ++i) whea.push_back(event(L"Microsoft-Windows-WHEA-Logger", 17, expected + uint64_t(i) * 10 * Second, {{L"ErrorSource", L"4"}}));
            check(incidentsFromEvents(whea).size() == 1, "a burst of corrected hardware errors is one incident");

            Incident fromBox; fromBox.kind = Kind::powerLoss; fromBox.time = expected - 30 * Second; fromBox.context = L"16:08:55 ac=0";
            std::vector<Incident> added;
            const auto merged = merge({}, {fromBox, one[0]}, &added);
            check(merged.size() == 1 && merged[0].kind == Kind::blueScreen && merged[0].code == 0x154 && merged[0].context == L"16:08:55 ac=0" &&
                added.size() == 1 && added[0].kind == Kind::blueScreen, "the black box's view and Windows' record of one crash merge");
            check(merge(merged, {one[0]}).size() == 1, "a crash already known is not added again");
            const auto line = saveLine(merged[0]);
            const auto loaded = loadLine(line);
            check(loaded && loaded->kind == Kind::blueScreen && loaded->code == 0x154 && loaded->time == merged[0].time && loaded->context == merged[0].context && !loadLine(L"x\ty"),
                "incidents save and load");

            std::vector<Record> box{rec(1, expected - 50 * Second, L"hb ac=0 pct=60 mode=silent"), rec(2, expected - 45 * Second, L"ev acpi 0x00110024=1E28"),
                rec(3, expected - 44 * Second, L"ev display off"), rec(4, expected - 40 * Second, L"hb ac=0 pct=60 mode=silent fanp=1")};
            Incident crash = one[0]; crash.context.clear();
            attachContext(crash, box);
            check(crash.context.find(L"fanp=1") != std::wstring::npos && crash.recentWrites.find(L"acpi 0x00110024=1E28") != std::wstring::npos &&
                crash.recentWrites.find(L"display") == std::wstring::npos, "a crash gets the last heartbeat and the hardware writes before it");

            StallMeter meter;
            check(!meter.feed(1000, 1000, true, 60000) && !meter.feed(3000, 1000, true, 60000) && meter.feed(4500, 1000, true, 60000) == 3.5 &&
                !meter.feed(9000, 1000, false, 60000) && !meter.feed(9000, 1000, true, 5000), "freezes count only with the screen on and away from power changes");
            // The G14's own report (2026-11-12): two 0xEF crashes eight minutes apart, each with its
            // 6008 and a failed dump (volmgr 161 logged just before the 41), stay two incidents.
            auto at = [&](int hour, int minute, int second) {
                SYSTEMTIME local{2026, 11, 4, 12, WORD(hour), WORD(minute), WORD(second), 0}, utc{}; FILETIME f{};
                TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc); SystemTimeToFileTime(&utc, &f);
                return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime;
            };
            auto crash41 = [&](uint64_t time, const wchar_t* code) { return event(L"Microsoft-Windows-Kernel-Power", 41, time, {{L"BugcheckCode", code}}); };
            const std::vector<LogEvent> day{
                event(L"volmgr", 161, at(17, 9, 28), {{L"", L"\\Device\\HarddiskVolume3"}}), crash41(at(17, 9, 29), L"239"),
                event(L"EventLog", 6008, at(17, 9, 44), {{L"", L"下午 05:08:31"}, {L"", L"\u200E2026/\u200E11/\u200E12"}}),
                event(L"volmgr", 161, at(17, 17, 7), {{L"", L"\\Device\\HarddiskVolume3"}}), crash41(at(17, 17, 9), L"239"),
                event(L"EventLog", 6008, at(17, 17, 20), {{L"", L"下午 05:09:44"}, {L"", L"\u200E2026/\u200E11/\u200E12"}}),
                event(L"Microsoft-Windows-WER-SystemErrorReporting", 1001, at(17, 18, 30), {{L"param1", L"0x000000ef (0xffff848a92ee9140, 0x0, 0x0, 0x0)"}})};
            const auto two = incidentsFromEvents(day);
            check(two.size() == 2 && two[0].kind == Kind::blueScreen && two[1].kind == Kind::blueScreen && two[0].code == 0xEF && two[1].code == 0xEF &&
                two[0].dumpFailed && two[1].dumpFailed && two[0].wentDown == at(17, 8, 31) && two[1].wentDown == at(17, 9, 44) && two[1].when() == at(17, 9, 44),
                "two crashes minutes apart stay two, each with its shutdown time and failed dump");
            check(merge(two, two).size() == 2, "re-reading the same records adds nothing");
            check(parseShutdownTime(L"6:25:20 PM", L"11/12/2026", at(18, 26, 40)) == at(18, 25, 20) &&
                parseShutdownTime(L"18:25:20", L"12.11.2026", at(18, 26, 40)) == at(18, 25, 20) &&
                parseShutdownTime(L"上午 12:05:00", L"2026/11/12", at(18, 26, 40)) == at(0, 5, 0) && !parseShutdownTime(L"x", L"y", at(18, 26, 40)),
                "6008's shutdown time reads in Chinese, US and European formats");
            Incident oldFormat; oldFormat.kind = Kind::blueScreen; oldFormat.time = expected; oldFormat.code = 0x154;
            std::wstring oldLine = saveLine(oldFormat);
            oldLine = oldLine.substr(0, oldLine.rfind(L'\t')); oldLine = oldLine.substr(0, oldLine.rfind(L'\t')); // as 0.23.0 wrote it
            const auto fromOld = loadLine(oldLine);
            Incident full = oldFormat; full.wentDown = expected - 90 * Second; full.dumpFailed = true;
            const auto fromNew = loadLine(saveLine(full));
            check(fromOld && fromOld->code == 0x154 && !fromOld->wentDown && fromNew && fromNew->wentDown == full.wentDown && fromNew->dumpFailed,
                "incidents saved by 0.23.0 still load; the shutdown time and failed dump are kept");
            {
                std::string dump(0x3000, '\0');
                std::memcpy(dump.data(), "PAGEDU64", 8);
                const uint32_t code9f = 0x9F; const uint64_t params[4] = {3, 0xffffd5860f5ec360ull, 0xfffff20d9e287a00ull, 0xffffd5860fcf1aa0ull};
                std::memcpy(dump.data() + 0x38, &code9f, 4); std::memcpy(dump.data() + 0x40, params, sizeof(params));
                size_t offset = 0x1000;
                for (const wchar_t* name : {L"\\Driver\\pci", L"\\Driver\\ACPI", L"\\Driver\\nipbcfk", L"\\Driver\\nipcibrd", L"\\Driver\\pci"}) {
                    const size_t bytes = wcslen(name) * 2;
                    std::memcpy(dump.data() + offset, name, bytes); offset += bytes + 40;
                }
                const std::wstring dumpPath = std::wstring(temp) + L"bs-dump-test.dmp";
                { std::ofstream out(std::filesystem::path(dumpPath), std::ios::binary); out << dump; }
                const auto text = summarizeKernelDump(dumpPath);
                DeleteFileW(dumpPath.c_str());
                check(text.find(L"0x9F DRIVER_POWER_STATE_FAILURE (0x3,") == 0 && text.find(L"pci ACPI nipbcfk nipcibrd") != std::wstring::npos,
                    "a kernel minidump gives its stop code and the drivers in its crash data");
            }
            const auto query = eventQueryXml(expected, Channel::system);
            const auto appQuery = eventQueryXml(expected, Channel::application);
            check(query.find(L"<Select Path='System'>") != std::wstring::npos && appQuery.find(L"<Select Path='Application'>") != std::wstring::npos &&
                query.find(L"Path='Application'") == std::wstring::npos && eventQueryXml(expected, Channel::installs).find(L"MsiInstaller") != std::wstring::npos &&
                query.find(L"@SystemTime&gt;='2026-11-12T08:09:31.000Z'") != std::wstring::npos && query.find(L">=") == std::wstring::npos, "the event query is valid XML");
        }
        check(std::any_of(browserPlan.begin(), browserPlan.end(), [](const lab::Step& step) { return step.knob == lab::Knob::darkMode && step.workload == lab::Workload::browse; })
            && lab::knobKey(lab::Knob::darkMode) == std::wstring(L"dark-mode"), "dark mode is measured on a browsed page");
        check(std::any_of(browserPlan.begin(), browserPlan.end(), [](const lab::Step& step) { return step.workload == lab::Workload::video && step.prepareSeconds >= 15; })
            && std::none_of(builtInPlan.begin(), builtInPlan.end(), [](const lab::Step& step) { return step.workload == lab::Workload::video; }),
            "real video runs only with a browser, after time to record the clip");
        check(lab::experimentKey(lab::Knob::cpuBoost, lab::Workload::browse) == L"cpu-boost@browse" && lab::experimentKey(lab::Knob::refreshRate, lab::Workload::none) == L"refresh",
            "experiment keys name the setting and the activity");
        auto planned = std::make_shared<FakeActuator>();
        lab::Controller sequence(L"", planned);
        lab::Step first; first.knob = lab::Knob::cpuBoost; first.workload = lab::Workload::browse; first.pairs = 1; first.blockSeconds = 40;
        first.warmupSeconds = 5; first.minimumSeconds = 20; first.prepareSeconds = 10;
        lab::Step second = first; second.workload = lab::Workload::sustained; second.prepareSeconds = 0;
        check(sequence.startPlan({first, second}) == ERROR_SUCCESS && sequence.workload() == lab::Workload::browse, "a plan starts with its first activity");
        planned->value = 2; const DWORD appliedBefore = planned->applied;
        uint64_t at = 50000;
        auto feedPlan = [&](double secondsToFeed) {
            for (double elapsed = 0; elapsed < secondsToFeed; elapsed += 2) {
                lab::Sample s = labSample(at, 8, 3, 1, lab::Context::interactive);
                s.workload.bursts = 1; s.workload.burstMs = 7;
                sequence.onSample(s, 2); at += 2000;
            }
        };
        feedPlan(8);
        check(planned->applied == appliedBefore && sequence.status().arm == -1, "nothing is switched while the activity starts up");
        feedPlan(4);
        check(planned->applied > appliedBefore && sequence.status().arm >= 0, "blocks begin after the preparation time");
        feedPlan(90);
        check(sequence.workload() == lab::Workload::sustained && sequence.status().step == 1, "after its pairs, the next step's activity starts");
        feedPlan(90);
        const auto planStatus = sequence.status();
        check(!planStatus.running && planStatus.finished && planned->value == 2 && !planned->remembered, "the plan ends with every setting restored");
        bool burstsRecorded = false;
        for (const auto& r : planStatus.results) burstsRecorded |= r.experiment == L"cpu-boost@browse" && r.bursts && std::abs(r.burstMs.mean0 - 7) < 1e-9;
        check(burstsRecorded, "blocks keep the activity's response times");
        // Dynamic power governor.
        {
            using gov::Setting;
            gov::Governor governor;
            gov::Input in; in.onBattery = true; in.context = lab::Context::interactive; in.idleSeconds = 1;
            in.actual = {DWORD(2), DWORD(60), DWORD(0), DWORD(120)}; // best performance, EPP 60, boost off, 120 Hz
            uint64_t now = 1000000;
            auto step = [&](double seconds, lab::Context context, double idle) {
                gov::Decision last;
                for (double t = 0; t < seconds; t += 2) {
                    in.ms = now; in.context = context; in.idleSeconds = idle + t;
                    last = governor.update(in);
                    for (size_t s = 0; s < last.apply.size(); ++s) if (last.apply[s]) in.actual[s] = *last.apply[s]; // the actuator obeys
                    now += 2000;
                }
                return last;
            };
            step(2, lab::Context::interactive, 1);
            check(governor.activity() == gov::Activity::responsive && in.actual[size_t(Setting::powerMode)] == DWORD(0) && in.actual[size_t(Setting::energyPreference)] == DWORD(50)
                && in.actual[size_t(Setting::cpuBoost)] == DWORD(0),
                "right after input the governor gives a responsive profile (efficiency, which cost only milliseconds in the real test)");
            check(in.actual[size_t(Setting::refreshRate)] == DWORD(120) && governor.originals()[size_t(Setting::powerMode)] == DWORD(2),
                "refresh switching is off by default and the originals are kept");
            step(4, lab::Context::interactive, 10);
            check(governor.activity() == gov::Activity::responsive, "a move toward saving waits until the activity has settled");
            step(10, lab::Context::interactive, 14);
            check(governor.activity() == gov::Activity::reading && in.actual[size_t(Setting::powerMode)] == DWORD(0) && in.actual[size_t(Setting::energyPreference)] == DWORD(80),
                "after settling, reading uses efficiency");
            step(2, lab::Context::heavy, 0);
            check(governor.activity() == gov::Activity::heavy && in.actual[size_t(Setting::powerMode)] == DWORD(1), "a move toward performance is immediate");
            step(2, lab::Context::idle, 300);
            check(governor.activity() == gov::Activity::heavy, "a short idle moment does not drop performance at once");
            step(20, lab::Context::idle, 302);
            check(governor.activity() == gov::Activity::idle && in.actual[size_t(Setting::energyPreference)] == DWORD(90), "a settled idle uses the most saving profile");
            in.actual[size_t(Setting::energyPreference)] = 33; // G-Helper or the user changes it
            step(2, lab::Context::idle, 330);
            check(!governor.managing(Setting::energyPreference, now) && in.actual[size_t(Setting::energyPreference)] == DWORD(33),
                "a setting changed by someone else is left alone for a while");
            step(60, lab::Context::interactive, 1);
            check(in.actual[size_t(Setting::energyPreference)] == DWORD(33) && in.actual[size_t(Setting::powerMode)] == DWORD(0),
                "other settings keep being managed meanwhile");
            in.onBattery = false;
            const auto unplugged = governor.update(in);
            check(unplugged.restore && unplugged.apply[size_t(Setting::powerMode)] == DWORD(2) && !unplugged.apply[size_t(Setting::energyPreference)],
                "plugging in restores what the governor changed, and only that");
            // Background apps.
            gov::Governor apps; in.onBattery = true; in.context = lab::Context::interactive; in.idleSeconds = 30;
            gov::BackgroundApp busy; busy.pid = 7; busy.created = 70; busy.name = L"sync.exe"; busy.cpu = 4;
            gov::BackgroundApp front = busy; front.pid = 8; front.created = 80; front.foreground = true;
            in.apps = {busy, front};
            std::vector<std::pair<uint32_t, uint64_t>> throttledNow;
            for (int i = 0; i < 20; ++i) { in.ms = now; now += 2000; auto d = apps.update(in); throttledNow.insert(throttledNow.end(), d.ecoOn.begin(), d.ecoOn.end()); }
            check(throttledNow.size() == 1 && throttledNow[0].first == 7, "a background app busy for 30 seconds gets efficiency mode, the foreground app never");
            in.apps[0].foreground = true; in.ms = now;
            auto released = apps.update(in);
            check(released.ecoOff.size() == 1 && released.ecoOff[0].first == 7, "an app brought to the front is released at once");
            // Battery target and lab evidence.
            gov::Options options; options.targetHours = 5.0;
            gov::Governor target; target.setOptions(options);
            check(target.profileFor(gov::Activity::interactive, true).values[size_t(Setting::energyPreference)] == DWORD(80),
                "when the battery will not last to the target, activities use the next more saving profile");
            lab::ExperimentResult saving; saving.experiment = L"cpu-boost@browse"; saving.arm0 = L"Boost 開"; saving.arm1 = L"Boost 關";
            saving.battery = lab::comparePairs({{12, 11}, {12.2, 11.1}, {11.9, 10.9}}); saving.frames = true;
            saving.drops = lab::comparePairs({{0, .2}, {0, 0}, {.1, .2}});
            lab::ExperimentResult costly = saving; costly.experiment = L"epp@browse"; costly.arm0 = L"能源偏好 60"; costly.arm1 = L"能源偏好 85";
            costly.drops = lab::comparePairs({{0, 3}, {0, 4}, {.5, 3}});
            auto evidence = gov::learn({saving, costly});
            check(evidence.size() == 6 && evidence[0].approved && evidence[0].savingValue == 0 && !evidence[3].approved,
                "lab results become evidence: savings without cost are approved, savings with late frames are not");
            gov::Governor learned; learned.setEvidence(evidence);
            const auto learnedProfile = learned.profileFor(gov::Activity::interactive, false);
            check(learnedProfile.values[size_t(Setting::cpuBoost)] == DWORD(0) && !learnedProfile.values[size_t(Setting::energyPreference)],
                "an approved saving is used and a costly one is left to the user");
            // From the first real automatic test: efficiency mode saved 2-3 W of CPU power in every
            // activity, while bursts took 7 -> 10 ms and the battery reading was noisy.
            lab::ExperimentResult mode; mode.experiment = L"power-mode@office"; mode.arm0 = L"效能"; mode.arm1 = L"效率";
            mode.battery = lab::comparePairs({{14.2, 12.4}, {13.2, 11.7}, {12.8, 12.1}});
            mode.core = lab::comparePairs({{4.3, 1.9}, {4.4, 2.0}, {4.2, 1.9}});
            mode.bursts = true; mode.burstMs = lab::comparePairs({{7.2, 10.2}, {7.1, 10.1}, {7.3, 10.3}});
            auto modeEvidence = gov::learn({mode});
            check(!mode.battery.significant() && modeEvidence.size() == 2 && modeEvidence[0].approved && modeEvidence[0].savingValue == 0
                && nearlyEqual(modeEvidence[0].wattsSaved, mode.core.difference < 0 ? -mode.core.difference : mode.core.difference),
                "a clear CPU core saving counts when the battery reading is noisy, and 3 ms slower bursts are no cost");
            mode.burstMs = lab::comparePairs({{20, 30}, {21, 31}, {19, 29}});
            check(!gov::learn({mode})[0].approved, "10 ms slower responses are a cost");
        }
        // Activity detector v2.
        {
            using gov::Activity;
            gov::Detector detector;
            auto observe = [](double idle, std::optional<double> active, gov::InputKind kind) {
                gov::Observation o; o.ms = 5000000; o.idleSeconds = idle; o.activeSeconds = active; o.lastInput = kind;
                o.foregroundExe = L"chrome.exe"; o.foregroundPid = 40; o.systemCpu = 5; o.framesPerSecond = 0; o.gpu.known = true;
                return o;
            };
            auto o = observe(1, 1, gov::InputKind::typing);
            check(detector.assess(o).activity == Activity::responsive, "typing makes the detector responsive at once");
            o = observe(1, 40, gov::InputKind::pointer);
            auto pointer = detector.assess(o);
            check(pointer.activity == Activity::interactive && pointer.reason == L"只移動游標", "moving the pointer alone does not trigger the input boost");
            o = observe(1, std::nullopt, gov::InputKind::none);
            check(detector.assess(o).activity == Activity::responsive, "without raw input, any recent input counts as active");
            o = observe(30, 30, gov::InputKind::scroll);
            auto reading = detector.assess(o);
            check(reading.activity == Activity::reading && reading.confidence >= .8 && reading.category == gov::Category::browser,
                "a pause in a browser is confidently reading");
            o = observe(300, 300, gov::InputKind::none);
            check(detector.assess(o).activity == Activity::idle, "a long pause with a still screen is idle");
            o.framesPerSecond = 40;
            check(detector.assess(o).activity == Activity::reading, "a long pause while the screen keeps moving is watching, not idle");
            o = observe(300, 300, gov::InputKind::none); o.framesPerSecond.reset(); o.compositorGpu = 9;
            check(detector.assess(o).activity == Activity::reading, "the compositor's GPU use shows a moving screen when the frame counter does not work");
            o.compositorGpu = .5;
            auto still = detector.assess(o);
            check(still.activity == Activity::idle && still.confidence >= .9, "and a still one");
            o = observe(30, 30, gov::InputKind::none); o.gpu.total.decode = 8;
            auto muted = detector.assess(o);
            check(muted.activity == Activity::media && muted.reason.find(L"靜音") != std::wstring::npos, "muted video is found through the video decoder");
            o = observe(1, 1, gov::InputKind::click); o.foregroundExe = L"cyberpunk2077.exe"; o.foregroundCoversScreen = true;
            o.gpu.processes[40].engine3d = 70;
            auto game = detector.assess(o);
            check(game.activity == Activity::game && game.category == gov::Category::game, "a borderless full-screen game is recognised");
            o.foregroundExe = L"unknown.exe"; o.gpu.processes[40].engine3d = 45;
            check(detector.assess(o).activity == Activity::game, "an unknown program filling the screen with heavy 3D is treated as a game");
            o.call = true;
            check(detector.assess(o).activity == Activity::game, "voice chat while gaming stays a game");
            o = observe(1, 1, gov::InputKind::click); o.foregroundCoversScreen = true; o.gpu.total.decode = 5; o.gpu.processes[40] = {30, 5, 0};
            check(detector.assess(o).activity != Activity::game, "a full-screen browser video is not a game");
            o = observe(0, 0, gov::InputKind::typing); o.locked = true;
            check(detector.assess(o).activity == Activity::idle, "a locked screen is idle whatever else happens");
            gov::Detector busy;
            o = observe(30, 30, gov::InputKind::none); o.systemCpu = 60; o.ms = 1000;
            check(busy.assess(o).activity == Activity::reading, "a short CPU spike is not heavy yet");
            o.ms = 6000;
            check(busy.assess(o).activity == Activity::heavy, "sustained CPU load is heavy");
            gov::Detector ruled;
            ruled.setRules({{L"chrome.exe", gov::AppRule::performance}}, {{L"word.exe", gov::AppRule::saving}});
            o = observe(30, 30, gov::InputKind::none);
            auto ruledResult = ruled.assess(o);
            check(ruledResult.activity == Activity::heavy && ruledResult.rule == gov::AppRule::performance && !ruledResult.learnedRule,
                "a performance rule lifts the program's activity");
            bool learnedFlag = false;
            check(ruled.ruleFor(L"WORD.EXE", &learnedFlag) == gov::AppRule::saving && learnedFlag, "learned rules match regardless of case");
            check(gov::categoryOf(L"Hogwarts-Win64-Shipping.exe") == gov::Category::game && gov::categoryOf(L"code.exe") == gov::Category::code,
                "program categories come from the executable name");
            auto gpuSummary = gov::summarizeGpu({{L"pid_40_luid_0x0_0x1_phys_0_eng_0_engtype_3D", 30}, {L"pid_41_luid_0x0_0x1_phys_0_eng_0_engtype_3D", 20},
                {L"pid_40_luid_0x0_0x1_phys_0_eng_5_engtype_VideoDecode", 6}, {L"pid_9_luid_0x0_0x2_phys_0_eng_0_engtype_3D", 15}});
            check(nearlyEqual(gpuSummary.total.engine3d, 50) && nearlyEqual(gpuSummary.total.decode, 6) && nearlyEqual(gpuSummary.processes[40].engine3d, 30),
                "GPU engines are summed per adapter and per process");
            // From the 2026-11-12 full test on the G14 2024 (Radeon 780M, OLED at 120 Hz).
            auto amd = gov::summarizeGpu({{L"pid_40_luid_0x0_0x1_phys_0_eng_4_engtype_Video Codec 0", 7}, {L"pid_40_luid_0x0_0x1_phys_0_eng_0_engtype_3D", 8},
                {L"pid_41_luid_0x0_0x1_phys_0_eng_1_engtype_Compute_1", 0}});
            check(nearlyEqual(amd.total.decode, 7) && gov::decodeEngine(L"videodecode") && !gov::decodeEngine(L"videoencode") && !gov::decodeEngine(L"3d")
                && std::find(amd.engineTypes.begin(), amd.engineTypes.end(), L"video codec") != amd.engineTypes.end()
                && std::find(amd.engineTypes.begin(), amd.engineTypes.end(), L"compute") != amd.engineTypes.end(),
                "AMD's \"Video Codec\" engine counts as video decoding, and every engine type is listed");
            gov::Detector twoThreads;
            o = observe(300, 300, gov::InputKind::none); o.systemCpu = 17; o.busiestCores = 2.0; o.ms = 1000;
            twoThreads.assess(o); o.ms = 6000;
            check(twoThreads.assess(o).activity == Activity::heavy, "one program keeping 2 of 16 threads busy is heavy work although the whole CPU is only 17% busy");
            o.busiestCores = 1.0; o.ms = 12000;
            check(twoThreads.assess(o).activity != Activity::heavy, "one busy thread is not");
            o = observe(300, 300, gov::InputKind::none); o.compositorGpu = 0; o.framesPerSecond = 1; o.foregroundGpu.engine3d = 12;
            check(detector.assess(o).activity == Activity::reading, "a browser drawing on its own (the compositor shows nothing on an overlay) is a moving screen, not idle");
            o.foregroundGpu.engine3d = 0;
            check(detector.assess(o).activity == Activity::idle, "and a page that stopped drawing is idle");
            // Detection is scored against the automatic test's known activities.
            gov::DetectionTally tally;
            tally.add(L"video", Activity::media, 90); tally.add(L"video", Activity::reading, 10); tally.add(L"sustained", Activity::heavy, 100);
            check(nearlyEqual(*tally.accuracy(L"video"), .9) && nearlyEqual(*tally.overall(), 190.0 / 200) && tally.summary().find(L"閱讀") != std::wstring::npos,
                "detection accuracy is measured per activity and names the most common mistake");
            // The governor uses the detection and its confidence.
            using gov::Setting;
            gov::Governor fast; gov::Input fi; fi.onBattery = true; fi.actual = {DWORD(1), DWORD(60), DWORD(0), DWORD(120)};
            uint64_t t = 9000000;
            auto feed = [&](Activity a, double confidence) {
                fi.ms = t; fi.detected = a; fi.confidence = confidence;
                auto d = fast.update(fi);
                for (size_t i = 0; i < d.apply.size(); ++i) if (d.apply[i]) fi.actual[i] = *d.apply[i];
                t += 1000; return d;
            };
            for (int i = 0; i < 8; ++i) feed(Activity::reading, 1);
            check(fi.actual[size_t(Setting::energyPreference)] == DWORD(80), "the governor follows the detector");
            feed(Activity::responsive, 1);
            check(fi.actual[size_t(Setting::energyPreference)] == DWORD(50), "a boost is applied within one sample even right after a saving change");
            for (int i = 0; i < 7; ++i) feed(Activity::reading, .6);
            check(fast.activity() == Activity::responsive, "an unsure detection waits longer before saving power");
            for (int i = 0; i < 4; ++i) feed(Activity::reading, .6);
            check(fast.activity() == Activity::reading, "and saves once it has held");
            fi.actual[size_t(Setting::powerMode)] = 2; // the user picks best performance
            auto overridden = feed(Activity::reading, 1);
            // A value the user chose in BatteryScope is held for the battery session, then handed back.
            {
                gov::Governor h; gov::Input hi; hi.onBattery = true; hi.actual = {DWORD(2), DWORD(60), DWORD(0), DWORD(120)}; hi.detected = Activity::reading; hi.ms = 1000;
                auto apply = [&](const gov::Decision& d) { for (size_t i = 0; i < d.apply.size(); ++i) if (d.apply[i]) hi.actual[i] = *d.apply[i]; };
                apply(h.update(hi));
                check(hi.actual[size_t(Setting::powerMode)] == DWORD(0), "reading saves power");
                hi.actual[size_t(Setting::powerMode)] = 1; h.hold(Setting::powerMode); // the user picks balanced
                for (int i = 0; i < 30; ++i) { hi.ms += 60000; apply(h.update(hi)); }
                check(hi.actual[size_t(Setting::powerMode)] == DWORD(1) && h.held(Setting::powerMode), "a held setting is left alone, even after half an hour");
                h.release(Setting::powerMode); hi.ms += 60000; apply(h.update(hi));
                check(hi.actual[size_t(Setting::powerMode)] == DWORD(0), "handed back, it is managed again at once");
                h.hold(Setting::powerMode); hi.actual[size_t(Setting::powerMode)] = 1;
                hi.onBattery = false; hi.ms += 1000; auto plugged = h.update(hi);
                check(!plugged.apply[size_t(Setting::powerMode)] && !h.held(Setting::powerMode), "plugging in keeps the held value and ends the hold");
            }
            // Bursts, low battery and a vetoed efficiency mode.
            {
                gov::Detector burstDetector; gov::Observation bo; bo.ms = 1000; bo.idleSeconds = 0; bo.activeSeconds = 0.5; bo.lastInput = gov::InputKind::click;
                bo.systemCpu = 55; bo.foregroundExe = L"chrome.exe";
                check(burstDetector.assess(bo).activity == Activity::burst, "a click with a CPU spike is a burst (a program or page loading)");
                bo.systemCpu = 8;
                check(burstDetector.assess(bo).activity == Activity::responsive, "a click without one is plain input");
                gov::Governor g; gov::Options o; g.setOptions(o);
                check(g.profileFor(Activity::burst, 0).values[size_t(Setting::powerMode)] == DWORD(1) && g.profileFor(Activity::burst, 1).values[size_t(Setting::powerMode)] == DWORD(0),
                    "a burst gets balanced, and efficiency when saving harder");
                gov::Input li; li.onBattery = true; li.actual = {DWORD(1), DWORD(60), DWORD(0), DWORD(120)}; li.detected = Activity::interactive; li.ms = 100;
                li.batteryPercent = 50.0; g.update(li);
                check(g.stretchLevel() == 0, "no extra saving with half a battery");
                li.ms += 1000; li.batteryPercent = 18.0; g.update(li);
                check(g.stretchLevel() == 1, "one level more saving at the low-battery level");
                li.ms += 1000; li.batteryPercent = 9.0; auto low = g.update(li);
                check(g.stretchLevel() == 2 && low.reason.find(L"最大省電") != std::wstring::npos, "two levels when the battery is very low");
                gov::Governor vetoed; vetoed.setEvidence({{Setting::powerMode, Activity::interactive, 0, 2, false}});
                check(vetoed.profileFor(Activity::interactive, 0).values[size_t(Setting::powerMode)] == DWORD(1), "a proven cost of efficiency falls back to balanced, not to the user's mode");
            }
            check(overridden.overrides.size() == 1 && overridden.overrides[0].first == Setting::powerMode && overridden.overrides[0].second == 1,
                "a user override toward performance is reported for learning");
        }
        CpuAttributor slowEstimator;
        report.cores = heterogeneous; report.coreWatts = 5; report.ready = true; report.intervalSeconds = 20;
        a.cpu = 25; a.fixedCoreBusy.clear(); b.cpu = 25; energyRows = {a,b};
        slowEstimator.apply(energyRows, report, 1000, 4);
        auto initialEnergy = energyRows.front().cpuMilliWh;
        slowEstimator.apply(energyRows, report, 21000, 4);
        check(initialEnergy && nearlyEqual(energyRows.front().cpuMilliWh, *initialEnergy + 2.5 * 20 / 3.6),
            "twenty second adaptive sampling integrates elapsed energy correctly");
        check(nearlyEqual(energyRows[0].cpuWatts.value_or(0) + energyRows[1].cpuWatts.value_or(0) + report.unassignedCpuWatts.value_or(0), 5),
            "slow cadence conserves the measured core energy budget");
        {
            EnergyLedger ledger; CpuEnergyReport ledgerReport; ledgerReport.ready = true; ledgerReport.intervalSeconds = 2; ledgerReport.coreWatts = 6;
            ProcessRow tab; tab.pid = 30; tab.created = 1; tab.name = L"Browser.exe"; tab.cpuWatts = 1.8;
            ProcessRow tab2 = tab; tab2.pid = 31; tab2.name = L"browser.EXE"; tab2.cpuWatts = .9;
            ProcessRow idle = tab; idle.pid = 32; idle.name = L"Idle.exe"; idle.cpuWatts.reset();
            ledger.update({tab, tab2, idle}, ledgerReport, 1000);
            ledger.update({tab, tab2, idle}, ledgerReport, 1000);
            auto ranking = ledger.top(10);
            check(ranking.size() == 1 && ranking[0].processes == 2 && nearlyEqual(ranking[0].milliWh, 2.7 * 2 / 3.6) && ranking[0].running,
                "session ledger groups an app's processes by name and ignores replayed samples");
            check(nearlyEqual(ledger.coreMilliWh(), 6 * 2 / 3.6) && ledger.tracked() == 2, "ledger tracks core energy and skips processes without estimates");
            ledger.update({tab2}, ledgerReport, 3000);
            ranking = ledger.top(10);
            check(ranking.size() == 1 && nearlyEqual(ranking[0].milliWh, (2.7 + .9) * 2 / 3.6), "exited processes keep their session energy");
            ledgerReport.timingMismatch = true; ledger.update({tab2}, ledgerReport, 5000);
            check(nearlyEqual(ledger.top(10)[0].milliWh, (2.7 + .9) * 2 / 3.6), "misaligned samples add no energy");
            ledgerReport.timingMismatch = false;
            for (uint32_t i = 0; i < 700; ++i) {
                ProcessRow brief = tab; brief.pid = 1000 + i; brief.name = L"Brief" + std::to_wstring(i) + L".exe"; brief.cpuWatts = .01 * (i + 1);
                ledger.update({brief}, ledgerReport, 7000 + i * 2000);
            }
            check(ledger.tracked() <= 600 && ledger.top(1)[0].name == L"Brief699.exe", "ledger bounds memory and keeps the largest exited entries");
        }
        {
            LowBatteryMonitor monitor;
            auto battery = [](double percent, bool ac) { PowerSample p; p.present = p.acKnown = true; p.onAc = p.charging = ac; p.percent = percent; return p; };
            check(!monitor.update(battery(50, false), 20) && monitor.update(battery(20, false), 20), "low battery alert fires at the threshold while discharging");
            check(!monitor.update(battery(19, false), 20) && !monitor.update(battery(22, false), 20) && !monitor.update(battery(20, false), 20),
                "low battery alert fires once until it recovers five points");
            check(!monitor.update(battery(18, true), 20) && monitor.update(battery(17, false), 20), "external power re-arms the alert");
            check(!monitor.update(battery(5, false), 0), "threshold zero disables the alert");
            auto unknown = battery(10, false); unknown.percent.reset();
            check(!monitor.update(unknown, 20), "unknown percentage never alerts");
        }
        check(csvField(L"plain") == L"plain" && csvField(L"a,b") == L"\"a,b\"" && csvField(L"say \"hi\"") == L"\"say \"\"hi\"\"\"",
            "CSV fields are quoted only when needed");
        check(csvField(L"=cmd|' /C calc'!A0") == L"'=cmd|' /C calc'!A0" && csvField(L"-x") == L"'-x" && csvField(L"line\nbreak") == L"\"line\nbreak\"",
            "CSV export neutralizes spreadsheet formulas from process names");
        {
            PowerSample worn = sample(0); worn.fullWh = 45; worn.designWh = 60;
            check(nearlyEqual(batteryHealth(worn), 75), "battery health is full-charge over design capacity");
            worn.designWh.reset(); check(!batteryHealth(worn), "unknown design capacity gives no health");
            worn.designWh = 0; check(!batteryHealth(worn), "zero design capacity never divides");
            worn.designWh = 40; check(nearlyEqual(batteryHealth(worn), 112.5), "a new battery may report above 100 percent");
        }
        {
            // Remaining-time range: 30 minutes at 8 W, then the last 4 minutes at 16 W.
            std::deque<PowerSample> mixed;
            for (uint64_t t = 0; t <= 1800000; t += 10000) {
                auto p = sample(t, t >= 1560000 ? 16.0 : 8.0); p.remainingWh = 40; appendPower(mixed, p);
            }
            auto range = calculatePower(mixed);
            check(range.hoursRemainingLow && range.hoursRemainingHigh && *range.hoursRemainingLow < *range.hoursRemainingHigh &&
                nearlyEqual(range.hoursRemainingHigh, 40 / *range.longAverageWatts) && *range.longAverageWatts < *range.averageWatts,
                "remaining time range spans the short and long discharge means");
            std::deque<PowerSample> steady;
            for (uint64_t t = 0; t <= 600000; t += 10000) { auto p = sample(t, 10.0); p.remainingWh = 40; appendPower(steady, p); }
            range = calculatePower(steady);
            check(nearlyEqual(range.hoursRemainingLow, 4) && nearlyEqual(range.hoursRemainingHigh, 4), "steady load gives a zero-width range");
            std::deque<PowerSample> brief;
            for (uint64_t t = 0; t <= 60000; t += 10000) { auto p = sample(t, 10.0); p.remainingWh = 40; appendPower(brief, p); }
            check(!calculatePower(brief).hoursRemainingLow, "range needs two minutes of discharge");
        }
        {
            auto held = [](uint64_t t, double percent) { PowerSample p; p.ms = t; p.present = p.acKnown = p.onAc = true; p.percent = percent; return p; };
            std::deque<PowerSample> limit;
            for (uint64_t t = 0; t <= 200000; t += 10000) limit.push_back(held(t, 80 + (t % 20000 ? .3 : 0)));
            check(detectChargeLimit(limit) == 80, "steady plugged-in level below full is reported as a charge limit");
            limit.pop_back(); limit.pop_back(); limit.pop_back();
            check(!detectChargeLimit(limit), "charge limit needs three steady minutes");
            std::deque<PowerSample> full;
            for (uint64_t t = 0; t <= 200000; t += 10000) full.push_back(held(t, 99));
            check(!detectChargeLimit(full), "a full battery is not a charge limit");
            std::deque<PowerSample> charging;
            for (uint64_t t = 0; t <= 200000; t += 10000) { auto p = held(t, 80); p.charging = t < 100000; charging.push_back(p); }
            check(!detectChargeLimit(charging), "charging inside the window resets limit detection");
        }
        {
            ChargeCurve curve;
            check(!curve.hoursTo(50, 100, 50, 20), "an empty charge curve gives no estimate");
            for (int band = 0; band < ChargeCurve::Bands; ++band)
                for (int i = 0; i < 8; ++i) curve.add(band * 5 + 2, band < 16 ? 20 : 5); // fast to 80%, slow taper after
            const auto taper = curve.hoursTo(70, 100, 50, 20);
            // 70-80%: 5 Wh at 20 W = .25 h; 80-100%: 10 Wh at 5 W = 2 h.
            check(nearlyEqual(taper, 2.25), "charge curve follows the learned taper above 80 percent");
            check(nearlyEqual(curve.hoursTo(70, 80, 50, 20), .25), "charge curve can stop at a charge limit");
            check(nearlyEqual(curve.hoursTo(70, 80, 50, 10), .5), "a slower live rate scales the learned curve");
            ChargeCurve copy;
            check(copy.load(curve.save()) && nearlyEqual(copy.hoursTo(70, 100, 50, 20), 2.25) && !copy.load({1, 2, 3}),
                "charge curve round-trips through storage and rejects malformed data");
        }
        {
            CpuAttributor cycleAttributor; CpuEnergyReport cycleReport; cycleReport.ready = true; cycleReport.coreWatts = 8;
            cycleReport.cores = {{4, 1}, {4, 1}}; cycleReport.intervalSeconds = 2;
            ProcessRow fast; fast.pid = 40; fast.created = 1; fast.measured = true; fast.cpu = 25; fast.cycleRate = 3e9;
            ProcessRow slow = fast; slow.pid = 41; slow.cycleRate = 1e9;
            std::vector<ProcessRow> cycleRows{fast, slow};
            cycleAttributor.apply(cycleRows, cycleReport, 1000, 4);
            check(cycleReport.cycleWeighted && cycleRows[0].pid == 40 && nearlyEqual(cycleRows[0].cpuWatts, 6) && nearlyEqual(cycleRows[1].cpuWatts, 2),
                "equal CPU time at three times the cycles gets three times the energy");
            check(nearlyEqual(cycleReport.unassignedCpuWatts, 0), "cycle weighting conserves the core energy budget");
            CpuAttributor extreme; slow.cycleRate = 1e6; cycleRows = {fast, slow};
            extreme.apply(cycleRows, cycleReport, 1000, 4);
            check(*cycleRows[0].cpuWatts / *cycleRows[1].cpuWatts <= 9.0001, "cycle weighting is bounded against counter glitches");
            CpuAttributor partial; slow.cycleRate.reset(); cycleRows = {fast, slow};
            partial.apply(cycleRows, cycleReport, 1000, 4);
            check(!cycleReport.cycleWeighted && nearlyEqual(cycleRows[0].cpuWatts, 4), "a single process with cycles keeps time-based shares");
        }
        check(roundedMinutes(1.5) == 90 && roundedMinutes(.001) == 1 && roundedMinutes(-1) == 0, "durations round up to whole minutes");
        Snapshot opportunity; opportunity.energy.ready = true; opportunity.energy.observedCpuCoverage = .9;
        ProcessRow foreground; foreground.pid = 20; foreground.created = 10; foreground.window = true;
        ProcessRow helper; helper.pid = 21; helper.parentPid = 20; helper.created = 20; helper.actionable = helper.measured = true;
        helper.path = L"C:\\Apps\\Helper.exe"; helper.busySeconds = 90; helper.averageCpuWatts = 1.5;
        ProcessRow independent = helper; independent.pid = 22; independent.parentPid = 0; independent.averageCpuWatts = .8;
        opportunity.processes = {foreground, helper, independent};
        auto suggestions = reviewCandidates(opportunity);
        check(suggestions.size() == 1 && suggestions.front() == 2,
            "review candidates exclude helpers of visible apps");
        opportunity.processes[0].created = 30;
        check(reviewCandidates(opportunity).size() == 2, "reused parent PID does not hide independent background work");
        opportunity.processes[0].created = 10;
        opportunity.processes[2].services.push_back({L"test", L"Test", 22, false, false});
        check(reviewCandidates(opportunity).empty(), "services are never suggested for background review");
        opportunity.processes[2].services.clear(); opportunity.energy.observedCpuCoverage = .5;
        check(reviewCandidates(opportunity).empty(), "low observation coverage suppresses suggestions");
        {
            Snapshot drain; drain.energy.ready = true; drain.energy.observedCpuCoverage = .9;
            PowerSample onBattery = sample(1000); drain.history.push_back(onBattery);
            ProcessRow worker; worker.pid = 50; worker.created = 5; worker.actionable = worker.measured = true; worker.path = L"C:\\Apps\\Worker.exe";
            worker.busySeconds = 700; worker.averageCpuWatts = 2.4;
            ProcessRow light = worker; light.pid = 51; light.averageCpuWatts = 1.0;
            ProcessRow young = worker; young.pid = 52; young.busySeconds = 120; young.averageCpuWatts = 5;
            drain.processes = {light, young, worker};
            std::set<std::pair<uint32_t,uint64_t>> notified;
            auto alert = drainAlertCandidate(drain, notified);
            check(alert && drain.processes[*alert].pid == 50, "drain alert picks sustained high-power background work only");
            notified.insert({50, 5});
            check(!drainAlertCandidate(drain, notified), "each process is reported once");
            notified.clear(); drain.history.back().onAc = true;
            check(!drainAlertCandidate(drain, notified), "no drain alert while plugged in");
        }
        {
            uint32_t pid = 0; std::wstring engine;
            check(parseGpuInstance(L"pid_1234_luid_0x00000000_0x0000D1A5_phys_0_eng_0_engtype_3D", pid, engine) && pid == 1234 && engine == L"3D" &&
                !parseGpuInstance(L"luid_0x0_engtype_3D", pid, engine) && !parseGpuInstance(L"pid_12_luid_0x0", pid, engine),
                "GPU engine instance names give the process and engine type");
            auto gpu = aggregateGpu({{L"pid_7_luid_0x0_0x1_phys_0_eng_0_engtype_3D", 30}, {L"pid_7_luid_0x0_0x1_phys_0_eng_1_engtype_3D", 15},
                {L"pid_7_luid_0x0_0x1_phys_0_eng_2_engtype_VideoDecode", 60}, {L"pid_9_luid_0x0_0x1_phys_0_eng_0_engtype_Copy", 140},
                {L"_Total", 99}});
            check(nearlyEqual(gpu[7], 60) && nearlyEqual(gpu[9], 100) && gpu.size() == 2, "GPU use is the busiest engine type per process, capped at 100");
        }
        {
            DailyHistory history;
            for (uint64_t t = 0; t <= 600000; t += 10000) history.addPower(sample(t, 12.0), 20261102);
            auto plugged = sample(610000, -20); plugged.onAc = plugged.charging = true; history.addPower(plugged, 20261102);
            for (uint64_t t = 620000; t <= 3620000; t += 10000) { auto p = sample(t, -20); p.onAc = p.charging = true; history.addPower(p, 20261102); }
            auto days = history.recent(7);
            check(days.size() == 1 && nearlyEqual(days[0].batteryMinutes, 10) && nearlyEqual(days[0].dischargedWh, 2) && nearlyEqual(days[0].chargedWh, 20 * 3010 / 3600.0),
                "daily history integrates time on battery, discharge, and charge energy");
            CpuEnergyReport valid; valid.ready = true; valid.intervalSeconds = 2;
            ProcessRow app; app.pid = 1; app.created = 1; app.name = L"Editor.exe"; app.cpuWatts = 3.6;
            history.addApps({app}, valid, 1000, 20261102); history.addApps({app}, valid, 1000, 20261102);
            history.addApps({app}, valid, 3000, 20261103);
            days = history.recent(7);
            check(days.size() == 2 && days[0].apps.size() == 1 && nearlyEqual(days[0].apps[0].second, 2) && nearlyEqual(days[1].apps[0].second, 2),
                "per-day app energy rolls over at midnight and ignores replayed samples");
            DailyHistory restored;
            check(restored.load(history.save()) && restored.recent(7).size() == 2 && nearlyEqual(restored.recent(7)[0].dischargedWh, 2) &&
                restored.recent(7)[1].apps[0].first == L"Editor.exe", "daily history round-trips through its file format");
            check(!restored.load(L"garbage\n1,2\n") && restored.recent(7).empty(), "malformed history files are rejected safely");
            DailyHistory long_;
            for (int d = 1; d <= 70; ++d) long_.addPower(sample(uint64_t(d) * 100000000), 20260000 + d);
            check(long_.recent(100).size() == DailyHistory::KeepDays, "history keeps a bounded number of days");
        }
        {
            auto level = [](uint64_t t, double percent, bool ac) { PowerSample p; p.ms = t; p.present = p.acKnown = true; p.onAc = ac; p.percent = percent; return p; };
            auto sleep = evaluateSleep(level(0, 85, false), level(7 * 3600000ull, 79, false));
            check(sleep && nearlyEqual(sleep->hours(), 7) && nearlyEqual(sleep->drop(), 6) && !unusualSleepDrain(*sleep),
                "a seven hour sleep losing six points is recorded and normal");
            auto heavy = evaluateSleep(level(0, 80, false), level(4 * 3600000ull, 70, false));
            check(heavy && unusualSleepDrain(*heavy), "losing ten points in four hours of sleep is unusual");
            check(!evaluateSleep(level(0, 80, false), level(300000, 79, false)), "gaps under ten minutes are not sleeps");
            check(!evaluateSleep(level(0, 80, true), level(3600000, 90, true)), "sleep while plugged in is not a battery sleep");
            PowerMetrics drainNow; drainNow.averageWatts = 10;
            PowerSample battery = level(0, 50, false); battery.remainingWh = 30;
            check(nearlyEqual(extraMinutesIfStopped(drainNow, battery, 2), (30.0 / 8 - 30.0 / 10) * 60), "stopping two of ten watts adds the computed minutes");
            check(!extraMinutesIfStopped(drainNow, battery, 9.5) && !extraMinutesIfStopped(drainNow, battery, .001), "implausible or negligible gains are hidden");
            battery.onAc = true;
            check(!extraMinutesIfStopped(drainNow, battery, 2), "no battery gain is shown while plugged in");
            ChargeReminder reminder;
            check(!reminder.update(level(0, 70, true), 80) && reminder.update(level(1, 80, true), 80) && !reminder.update(level(2, 82, true), 80),
                "charge reminder fires once at the target");
            check(!reminder.update(level(3, 81, false), 80) && reminder.update(level(4, 81, true), 80), "unplugging re-arms the charge reminder");
            check(!reminder.update(level(5, 90, true), 0), "a zero target disables the charge reminder");
        }
        {
            DailyHistory record;
            auto worn = sample(0); worn.fullWh = 45; worn.designWh = 50; worn.cycleCount = 300u;
            record.addPower(worn, 20261102);
            SleepRecord night; night.startMs = 0; night.endMs = 6 * 3600000ull; night.before = 90; night.after = 85;
            record.addSleep(night, 20261102);
            auto day = record.recent(1).front();
            check(nearlyEqual(day.healthPercent, 90) && day.cycles == 300u && nearlyEqual(day.sleepMinutes, 360) && nearlyEqual(day.sleepDropPercent, 5),
                "daily history keeps battery health, cycles, and sleep");
            DailyHistory restored;
            restored.load(record.save());
            auto back = restored.recent(1).front();
            check(nearlyEqual(back.healthPercent, 90) && back.cycles == 300u && nearlyEqual(back.sleepMinutes, 360), "new history columns round-trip");
            check(restored.load(L"date,battery_minutes,discharged_wh,charged_wh,top_apps\n20261101,30.0,4.000,1.000,Editor.exe=12.0\n") &&
                restored.recent(1).front().apps.size() == 1 && !restored.recent(1).front().healthPercent,
                "history files from the previous version still load");
        }
        {
            auto battery = [](double percent, bool ac) { PowerSample p; p.present = p.acKnown = true; p.onAc = ac; p.percent = percent; return p; };
            check(decideAutoMode(AutoMode::none, false, battery(25, false), 30) == AutoMode::saver &&
                decideAutoMode(AutoMode::saver, false, battery(35, false), 30) == AutoMode::saver &&
                decideAutoMode(AutoMode::saver, false, battery(41, false), 30) == AutoMode::none &&
                decideAutoMode(AutoMode::none, false, battery(35, false), 30) == AutoMode::none,
                "low-battery saver engages at the threshold and releases ten points above it");
            check(decideAutoMode(AutoMode::saver, false, battery(25, true), 30) == AutoMode::none, "external power releases the saver rule");
            check(decideAutoMode(AutoMode::saver, true, battery(15, false), 30) == AutoMode::performance &&
                decideAutoMode(AutoMode::none, false, battery(15, false), 0) == AutoMode::none,
                "a listed app takes precedence; threshold zero disables the saver rule");
        }
        {
            auto day = [](int date, double minutes, double wh) { DayStats d; d.date = date; d.batteryMinutes = minutes; d.dischargedWh = wh; return d; };
            std::vector<DayStats> days{day(20261101, 60, 10), day(20261102, 60, 12), day(20261103, 60, 8), day(20261104, 60, 7.5)};
            auto grade = gradeDay(days, 20261104); // 7.5 W against a median of 10 W
            check(grade && grade->letter == L'S' && nearlyEqual(grade->ratio, .75), "a day 25 percent below the usual drain earns an S");
            days.back().dischargedWh = 14;
            check(gradeDay(days, 20261104)->letter == L'D', "a day 40 percent above usual earns a D");
            check(!gradeDay(days, 20261102), "grades need three earlier days");
            days.back().batteryMinutes = 10;
            check(!gradeDay(days, 20261104), "grades need 30 minutes on battery that day");
            std::vector<DayStats> streak;
            for (int i = 1; i <= 6; ++i) streak.push_back(day(20261100 + i, i == 1 ? 300 : 60, i == 1 ? 50 : i <= 3 ? 10 : 7)); // 10 W, then 7 W
            streak[1].sleepMinutes = 420; streak[1].sleepDropPercent = 2;
            auto earned = achievements(streak, false);
            check(earned.size() == AchievementCount && earned[0] && earned[1] && earned[2] && earned[3] && !earned[4] && !earned[5],
                "achievements follow the recorded days");
            check(achievements(streak, true)[4] && !achievements({}, false)[0], "battery care and empty history are reported correctly");
        }
        Collector cachedCollector;
        size_t total = 0, inaccessible = 0;
        auto warmRows = cachedCollector.collect(total, inaccessible);
        auto firstWork = cachedCollector.work();
        auto cachedRows = cachedCollector.collect(total, inaccessible);
        auto own = std::find_if(cachedRows.begin(), cachedRows.end(), [](const auto& row) { return row.pid == GetCurrentProcessId(); });
        auto system = std::find_if(cachedRows.begin(), cachedRows.end(), [](const auto& row) { return row.pid == 4; });
        check(own != cachedRows.end() && !own->path.empty() && !own->actionable, "cached metadata retains identity and self protection");
        check(cachedCollector.systemSnapshot() && system != cachedRows.end() && system->kind == L"Windows 核心",
            "single system snapshot includes protected kernel activity");
        check(cachedCollector.work().paths < firstWork.paths && cachedCollector.work().processOpens < firstWork.processOpens,
            "warm metadata cache removes repeated queries");
        cachedCollector.reset(); cachedCollector.collect(total, inaccessible);
        check(cachedCollector.work().paths > 0 && cachedCollector.work().processOpens > 0, "resume reset invalidates metadata cache");
        Collector fallbackCollector(true, true);
        auto fallbackRows = fallbackCollector.collect(total, inaccessible);
        std::set<DWORD> fallbackPids;
        for (const auto& row : fallbackRows) fallbackPids.insert(row.pid);
        check(!fallbackCollector.systemSnapshot() && fallbackPids.size() == fallbackRows.size() &&
            fallbackPids.count(GetCurrentProcessId()) && fallbackPids.count(4),
            "fallback snapshot retains distinct running processes including protected PID 4");
        check(total >= fallbackRows.size(), "fallback never loses a counted process except idle PID 0");
        check(!safeProcess(GetCurrentProcess(), GetCurrentProcessId()), "refuse to terminate self");
        check(stopService(L"RpcSs", 1, 1) == ERROR_ACCESS_DISABLED_BY_POLICY, "critical service blocked before SCM access");
        check(stopService(L"", 0, 0) == ERROR_INVALID_PARAMETER, "malformed service command rejected");
        {
            OwnedChild fixture(1);
            auto wrong = fixture.row; ++wrong.created;
            check(closeProcess(wrong, true) == ERROR_NOT_FOUND && WaitForSingleObject(fixture.process, 0) == WAIT_TIMEOUT, "stale PID identity cannot terminate a replacement");
            check(closeProcess(fixture.row, true) == ERROR_SUCCESS && WaitForSingleObject(fixture.process, 0) == WAIT_OBJECT_0, "force termination of owned test child only");
        }
        {
            OwnedChild fixture(3);
            DWORD original = 0;
            const DWORD before = GetPriorityClass(fixture.process);
            check(setProcessEfficiency(fixture.row, true, original) == ERROR_SUCCESS && GetPriorityClass(fixture.process) == IDLE_PRIORITY_CLASS &&
                processEfficiencyOn(fixture.row.pid) && original == before, "efficiency mode throttles and lowers priority of an owned test child");
            check(setProcessEfficiency(fixture.row, false, original) == ERROR_SUCCESS && GetPriorityClass(fixture.process) == before &&
                !processEfficiencyOn(fixture.row.pid), "turning efficiency mode off restores priority and hands throttling back to Windows");
            auto stale = fixture.row; ++stale.created;
            check(setProcessEfficiency(stale, true, original) == ERROR_NOT_FOUND, "efficiency mode revalidates process identity");
            ProcessRow self; self.pid = GetCurrentProcessId(); FILETIME c{}, e{}, k{}, u{};
            GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u); self.created = fileTime(c);
            check(setProcessEfficiency(self, true, original) == ERROR_ACCESS_DENIED, "efficiency mode refuses the monitor itself");
        }
        {
            OwnedChild fixture(2);
            check(closeProcess(fixture.row, false) == ERROR_SUCCESS, "normal WM_CLOSE sent to owned test window");
            check(WaitForSingleObject(fixture.process, 5000) == WAIT_OBJECT_0, "normal close child exits");
        }
        std::cout << passed << " tests passed\n"; return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
