#include "../src/platform.h"
#include "../src/energy.h"
#include "../src/motion.h"
#include "../src/i18n.h"
#include <shellapi.h>
#include <iostream>
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
    if (argc == 2 && std::wstring(argv[1]) == L"--saver-cycle") {
        // Opt-in local integration check; regular CI tests never change system settings.
        const auto before = querySaverMode();
        if (before.error || before.efficient) return 2;
        const DWORD enable = toggleSaverMode();
        const auto enabled = querySaverMode();
        const DWORD restore = enable ? enable : toggleSaverMode();
        const auto restored = querySaverMode();
        std::cout << "enable=" << enable << " enabled=" << enabled.efficient << " restorable=" << enabled.restorable
            << " restore=" << restore << " restored=" << !restored.efficient << '\n';
        return !enable && enabled.efficient && enabled.restorable && !restore && !restored.error && !restored.efficient ? 0 : 1;
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
        CpuAttributor estimator; CpuEnergyReport report; report.ready=true; report.coreWatts=5;
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
            OwnedChild fixture(2);
            check(closeProcess(fixture.row, false) == ERROR_SUCCESS, "normal WM_CLOSE sent to owned test window");
            check(WaitForSingleObject(fixture.process, 5000) == WAIT_OBJECT_0, "normal close child exits");
        }
        std::cout << passed << " tests passed\n"; return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
