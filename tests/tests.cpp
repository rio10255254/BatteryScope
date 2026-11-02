#include "../src/platform.h"
#include "../src/energy.h"
#include "../src/motion.h"
#include "../src/i18n.h"
#include <shellapi.h>
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
