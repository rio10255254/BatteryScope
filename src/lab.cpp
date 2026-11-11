#include "lab.h"
#include <tuple>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <set>
#include <sstream>

namespace bs::lab {
namespace {
std::string utf8(const std::wstring& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        uint32_t c = text[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < text.size()) c = 0x10000 + ((c - 0xD800) << 10) + (text[++i] - 0xDC00);
        if (c < 0x80) out += char(c);
        else if (c < 0x800) { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 63)); }
        else if (c < 0x10000) { out += char(0xE0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 63)); out += char(0x80 | (c & 63)); }
        else { out += char(0xF0 | (c >> 18)); out += char(0x80 | ((c >> 12) & 63)); out += char(0x80 | ((c >> 6) & 63)); out += char(0x80 | (c & 63)); }
    }
    return out;
}
std::wstring wide(const std::string& text) {
    std::wstring out;
    for (size_t i = 0; i < text.size();) {
        unsigned char c = text[i];
        uint32_t code = c; size_t extra = 0;
        if (c >= 0xF0) { code = c & 7; extra = 3; } else if (c >= 0xE0) { code = c & 15; extra = 2; } else if (c >= 0xC0) { code = c & 31; extra = 1; }
        ++i;
        for (size_t k = 0; k < extra && i < text.size(); ++k, ++i) code = (code << 6) | (static_cast<unsigned char>(text[i]) & 63);
        if (code >= 0x10000) { code -= 0x10000; out += wchar_t(0xD800 + (code >> 10)); out += wchar_t(0xDC00 + (code & 0x3FF)); }
        else out += wchar_t(code);
    }
    return out;
}
// Quotes a CSV field when needed; a leading formula character is neutralized.
std::string field(const std::wstring& text) {
    std::string value = utf8(text);
    if (!value.empty() && (value[0] == '=' || value[0] == '+' || value[0] == '-' || value[0] == '@')) value = "'" + value;
    if (value.find_first_of(",\"\r\n") == std::string::npos) return value;
    std::string quoted = "\"";
    for (char c : value) { if (c == '"') quoted += '"'; quoted += c; }
    return quoted + "\"";
}
std::string number(Value value, int digits = 3) {
    if (!value || !std::isfinite(*value)) return {};
    std::ostringstream out; out << std::fixed << std::setprecision(digits) << *value;
    return out.str();
}
std::string number(double value, int digits = 3) { return number(Value(value), digits); }
std::vector<std::string> splitCsv(const std::string& line) {
    std::vector<std::string> fields; std::string current; bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted) {
            if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') { current += '"'; ++i; }
            else if (c == '"') quoted = false;
            else current += c;
        } else if (c == '"') quoted = true;
        else if (c == ',') { fields.push_back(current); current.clear(); }
        else if (c != '\r' && c != '\n') current += c;
    }
    fields.push_back(current);
    return fields;
}
// Two-sided 95% critical values of Student's t for 1..30 degrees of freedom.
double tCritical(int degrees) {
    static const double table[] = {12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228,
        2.201, 2.179, 2.160, 2.145, 2.131, 2.120, 2.110, 2.101, 2.093, 2.086, 2.080, 2.074, 2.069, 2.064, 2.060,
        2.056, 2.052, 2.048, 2.045, 2.042};
    if (degrees < 1) return INFINITY;
    return degrees <= 30 ? table[degrees - 1] : 1.96 + 2.4 / degrees;
}
}

Context classify(const Signals& s) {
    if (s.call) return Context::call;
    if (s.fullscreenGame) return Context::game;
    if (s.audio && s.idleSeconds >= 20) return Context::media;
    if (s.systemCpu && *s.systemCpu >= 25) return Context::heavy;
    if (s.idleSeconds >= 120 && !s.audio) return Context::idle;
    return Context::interactive;
}
const wchar_t* contextKey(Context context) {
    switch (context) {
    case Context::idle: return L"閒置";
    case Context::interactive: return L"一般操作";
    case Context::media: return L"看影片／聽音樂";
    case Context::call: return L"視訊／通話";
    case Context::heavy: return L"高負載";
    case Context::game: return L"全螢幕遊戲";
    default: return L"";
    }
}
const wchar_t* workloadKey(Workload workload) {
    switch (workload) {
    case Workload::idle: return L"idle";
    case Workload::scroll: return L"scroll";
    case Workload::bursts: return L"bursts";
    case Workload::sustained: return L"sustained";
    case Workload::browse: return L"browse";
    case Workload::video: return L"video";
    case Workload::office: return L"office";
    default: return L"";
    }
}
const wchar_t* workloadName(Workload workload) {
    switch (workload) {
    case Workload::idle: return L"靜止畫面";
    case Workload::scroll: return L"捲動頁面";
    case Workload::bursts: return L"短暫運算";
    case Workload::sustained: return L"持續運算";
    case Workload::browse: return L"瀏覽網頁";
    case Workload::video: return L"看影片";
    case Workload::office: return L"打字文書";
    default: return L"";
    }
}
Value Sample::restWatts() const {
    if (!batteryWatts || !packageWatts || *batteryWatts < *packageWatts) return {};
    return *batteryWatts - *packageWatts;
}
std::string csvHeader() {
    return "time,on_battery,battery_w,package_w,core_w,rest_w,system_cpu,idle_s,audio,call,fullscreen_game,context,"
           "foreground,refresh_hz,dgpu_percent,igpu_percent,dgpu_state,dgpu_apps,frames_composed,frames_dropped,refreshes,"
           "boost_dc,epp_dc,ghelper,experiment,arm,block,counted,top_processes,test_frames,test_late_frames,test_bursts,test_burst_ms,test_work\r\n";
}
std::string csvRow(const Sample& s) {
    static const char* contexts[] = {"idle", "interactive", "media", "call", "heavy", "game"};
    std::ostringstream out;
    out << field(s.time) << ',' << s.onBattery << ',' << number(s.batteryWatts) << ',' << number(s.packageWatts) << ','
        << number(s.coreWatts) << ',' << number(s.restWatts()) << ',' << number(s.systemCpu, 1) << ',' << number(s.signals.idleSeconds, 0) << ','
        << s.signals.audio << ',' << s.signals.call << ',' << s.signals.fullscreenGame << ',' << contexts[size_t(s.context)] << ','
        << field(s.foreground) << ',' << s.refreshHz << ',' << number(s.dgpuPercent, 1) << ',' << number(s.igpuPercent, 1) << ','
        << s.dgpuState << ',' << field(s.dgpuApps) << ',' << number(s.framesComposed, 0) << ',' << number(s.framesDropped, 0) << ','
        << number(s.refreshes, 0) << ',' << s.boostDc << ',' << s.eppDc << ',' << s.ghelper << ',' << field(s.experiment) << ','
        << s.arm << ',' << s.block << ',' << s.counted << ',' << field(s.topProcesses) << ',' << number(s.workload.frames, 0) << ','
        << number(s.workload.lateFrames, 0) << ',' << number(s.workload.bursts, 0) << ',' << number(s.workload.burstMs, 1) << ','
        << number(s.workload.work, 0) << "\r\n";
    return out.str();
}

void Breakdown::add(const Sample& s, double seconds) {
    if (!s.onBattery || !s.batteryWatts || seconds <= 0 || seconds > 30) return;
    auto& t = totals_[size_t(s.context)];
    t.seconds += seconds;
    t.battery += *s.batteryWatts * seconds;
    if (s.packageWatts) t.package += *s.packageWatts * seconds;
    if (s.coreWatts) t.core += *s.coreWatts * seconds;
    if (auto rest = s.restWatts()) { t.rest += *rest * seconds; t.restSeconds += seconds; }
    batterySeconds_ += seconds;
    if (s.dgpuState == 0) dgpuAwake_ += seconds;
    size_t at = 0;
    while (at < s.dgpuApps.size()) {
        size_t end = s.dgpuApps.find(L';', at);
        if (end == std::wstring::npos) end = s.dgpuApps.size();
        if (end > at) dgpuApps_[s.dgpuApps.substr(at, end - at)] += seconds;
        at = end + 1;
    }
}
std::vector<std::pair<std::wstring, double>> Breakdown::dgpuApps(size_t limit) const {
    std::vector<std::pair<std::wstring, double>> apps(dgpuApps_.begin(), dgpuApps_.end());
    std::sort(apps.begin(), apps.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    if (apps.size() > limit) apps.resize(limit);
    return apps;
}

int Schedule::armFor(int block) const {
    // splitmix64 of (seed, pair) decides whether the pair runs AB or BA.
    uint64_t z = seed_ + uint64_t(block / 2) * 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; z ^= z >> 31;
    const int first = int(z & 1);
    return block % 2 == 0 ? first : 1 - first;
}

void BlockRecorder::begin(int pair, int arm, uint64_t ms, double warmup, double minimum) {
    *this = BlockRecorder{};
    pair_ = pair; arm_ = arm; start_ = ms; warmup_ = warmup; minimum_ = minimum;
}
bool BlockRecorder::add(const Sample& s, double seconds) {
    if (invalid_ || seconds <= 0 || seconds > 30 || s.ms < start_) return false;
    if ((s.ms - start_) / 1000.0 < warmup_) return false; // the setting is still settling
    if (!s.onBattery || !s.batteryWatts) return false;
    seconds_ += seconds;
    battery_ += *s.batteryWatts * seconds;
    if (s.packageWatts) package_ += *s.packageWatts * seconds;
    if (s.coreWatts) core_ += *s.coreWatts * seconds;
    if (auto rest = s.restWatts()) { rest_ += *rest * seconds; restSeconds_ += seconds; }
    if (s.systemCpu) { cpu_ += *s.systemCpu * seconds; cpuSeconds_ += seconds; }
    drops_ += s.framesDropped; composed_ += s.framesComposed;
    workload_.frames += s.workload.frames; workload_.lateFrames += s.workload.lateFrames;
    workload_.bursts += s.workload.bursts; workload_.burstMs += s.workload.burstMs; workload_.work += s.workload.work;
    contexts_[size_t(s.context)] += seconds;
    return true;
}
bool BlockRecorder::finish(Block& b) const {
    if (invalid_ || seconds_ < minimum_) return false;
    b.pair = pair_; b.arm = arm_; b.seconds = seconds_;
    b.battery = battery_ / seconds_; b.package = package_ / seconds_; b.core = core_ / seconds_;
    b.rest = restSeconds_ > 0 ? rest_ / restSeconds_ : 0;
    b.cpu = cpuSeconds_ > 0 ? cpu_ / cpuSeconds_ : 0;
    b.dropsPerMinute = drops_ / (seconds_ / 60); b.composedPerSecond = composed_ / seconds_;
    // The test window's own late frames are a better smoothness measure than the compositor's counters.
    if (workload_.frames > 0) { b.dropsPerMinute = workload_.lateFrames / (seconds_ / 60); b.framesPerSecond = workload_.frames / seconds_; }
    b.burstMs = workload_.bursts > 0 ? workload_.burstMs / workload_.bursts : 0;
    b.workPerSecond = workload_.work / seconds_;
    for (size_t i = 0; i < contexts_.size(); ++i) b.contextShare[i] = contexts_[i] / seconds_;
    return true;
}
std::string Block::csvHeader() {
    return "experiment,run,pair,arm,arm_name,started,seconds,battery_w,package_w,core_w,rest_w,cpu_percent,"
           "drops_per_min,composed_per_s,idle,interactive,media,call,heavy,game,burst_ms,work_per_s,frames_per_s\r\n";
}
std::string Block::csv() const {
    std::ostringstream out;
    out << field(experiment) << ',' << run << ',' << pair << ',' << arm << ',' << field(armName) << ',' << field(started) << ','
        << number(seconds, 0) << ',' << number(battery) << ',' << number(package) << ',' << number(core) << ',' << number(rest) << ','
        << number(cpu, 1) << ',' << number(dropsPerMinute, 2) << ',' << number(composedPerSecond, 2);
    for (double share : contextShare) out << ',' << number(share, 3);
    out << ',' << number(burstMs, 3) << ',' << number(workPerSecond, 1) << ',' << number(framesPerSecond, 2) << "\r\n";
    return out.str();
}
bool Block::parse(const std::string& line, Block& b) {
    const auto f = splitCsv(line);
    if (f.size() < 20 || f[0] == "experiment") return false;
    try {
        b.experiment = wide(f[0]); b.run = std::stoull(f[1]); b.pair = std::stoi(f[2]); b.arm = std::stoi(f[3]);
        b.armName = wide(f[4]); b.started = wide(f[5]); b.seconds = std::stod(f[6]);
        b.battery = std::stod(f[7]); b.package = std::stod(f[8]); b.core = std::stod(f[9]); b.rest = std::stod(f[10]);
        b.cpu = std::stod(f[11]); b.dropsPerMinute = std::stod(f[12]); b.composedPerSecond = std::stod(f[13]);
        for (size_t i = 0; i < b.contextShare.size(); ++i) b.contextShare[i] = std::stod(f[14 + i]);
        if (f.size() >= 23) { b.burstMs = std::stod(f[20]); b.workPerSecond = std::stod(f[21]); b.framesPerSecond = std::stod(f[22]); }
    } catch (...) { return false; }
    return (b.arm == 0 || b.arm == 1) && b.seconds > 0 && !b.experiment.empty();
}

Comparison comparePairs(const std::vector<std::pair<double, double>>& pairs) {
    Comparison c;
    c.pairs = int(pairs.size());
    if (pairs.empty()) return c;
    double sum0 = 0, sum1 = 0, sumDiff = 0;
    for (const auto& p : pairs) { sum0 += p.first; sum1 += p.second; sumDiff += p.second - p.first; }
    const double n = double(pairs.size());
    c.mean0 = sum0 / n; c.mean1 = sum1 / n; c.difference = sumDiff / n;
    if (pairs.size() < 2) { c.halfWidth = INFINITY; return c; }
    double squares = 0;
    for (const auto& p : pairs) { const double d = p.second - p.first - c.difference; squares += d * d; }
    c.halfWidth = tCritical(int(pairs.size()) - 1) * std::sqrt(squares / (n - 1)) / std::sqrt(n);
    return c;
}
std::vector<ExperimentResult> analyze(const std::vector<Block>& blocks) {
    // Runs of the same experiment are pooled only when they compared the same two
    // settings: with a different original (e.g. after switching the power mode) a
    // run compares something else and becomes its own result.
    std::map<std::pair<std::wstring, uint64_t>, std::array<std::wstring, 2>> runNames;
    for (const auto& b : blocks) runNames[{b.experiment, b.run}][size_t(b.arm)] = b.armName;
    std::map<std::tuple<std::wstring, std::wstring, std::wstring>, std::map<std::pair<uint64_t, int>, std::array<const Block*, 2>>> grouped;
    for (const auto& b : blocks) {
        const auto& n = runNames[{b.experiment, b.run}];
        grouped[{b.experiment, n[0], n[1]}][{b.run, b.pair}][size_t(b.arm)] = &b;
    }
    std::vector<ExperimentResult> results;
    for (const auto& [key, pairs] : grouped) {
        ExperimentResult r; r.experiment = std::get<0>(key);
        r.arm0 = std::get<1>(key); r.arm1 = std::get<2>(key);
        std::vector<std::pair<double, double>> battery, package, core, rest, cpu, drops, burst, work, perWork;
        for (const auto& item : pairs) {
            const Block* a = item.second[0]; const Block* b = item.second[1];
            if (!a || !b) continue; // only complete pairs
            battery.emplace_back(a->battery, b->battery); package.emplace_back(a->package, b->package);
            core.emplace_back(a->core, b->core); rest.emplace_back(a->rest, b->rest);
            cpu.emplace_back(a->cpu, b->cpu); drops.emplace_back(a->dropsPerMinute, b->dropsPerMinute);
            r.frames |= a->framesPerSecond > 0 && b->framesPerSecond > 0;
            if (a->burstMs > 0 && b->burstMs > 0) { burst.emplace_back(a->burstMs, b->burstMs); r.bursts = true; }
            if (a->workPerSecond > 0 && b->workPerSecond > 0) {
                work.emplace_back(a->workPerSecond, b->workPerSecond);
                // Energy per unit of work: watts divided by work per second.
                perWork.emplace_back(a->battery / a->workPerSecond * 1e6, b->battery / b->workPerSecond * 1e6);
                r.work = true;
            }
        }
        r.battery = comparePairs(battery); r.package = comparePairs(package); r.core = comparePairs(core);
        r.rest = comparePairs(rest); r.cpu = comparePairs(cpu); r.drops = comparePairs(drops);
        r.burstMs = comparePairs(burst); r.workRate = comparePairs(work); r.energyPerWork = comparePairs(perWork);
        results.push_back(std::move(r));
    }
    return results;
}
}
