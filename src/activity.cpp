#include "activity.h"
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <cwctype>

namespace bs::gov {
namespace {
std::wstring lower(std::wstring text) {
    for (auto& c : text) c = wchar_t(std::towlower(c));
    return text;
}
bool decodeBusy(const EngineUse& e) { return e.decode >= 1; }
}

const wchar_t* activityKey(Activity a) {
    switch (a) {
    case Activity::idle: return L"閒置";
    case Activity::reading: return L"閱讀";
    case Activity::responsive: return L"操作中";
    case Activity::burst: return L"開啟／載入中";
    case Activity::interactive: return L"一般操作";
    case Activity::media: return L"看影片／聽音樂";
    case Activity::call: return L"視訊／通話";
    case Activity::heavy: return L"高負載";
    case Activity::game: return L"全螢幕遊戲";
    case Activity::exempt: return L"此程式不管理";
    default: return L"";
    }
}
int rank(Activity a) {
    switch (a) {
    case Activity::idle: return 0;
    case Activity::reading: case Activity::media: return 1;
    case Activity::interactive: case Activity::call: return 2;
    case Activity::responsive: return 3;
    case Activity::burst: return 4;
    case Activity::heavy: return 5;
    case Activity::game: case Activity::exempt: return 6;
    default: return 2;
    }
}

Category categoryOf(const std::wstring& exe) {
    static const std::map<std::wstring, Category> known = [] {
        std::map<std::wstring, Category> m;
        auto add = [&](Category c, std::initializer_list<const wchar_t*> names) { for (auto n : names) m[n] = c; };
        add(Category::browser, {L"msedge.exe", L"chrome.exe", L"firefox.exe", L"brave.exe", L"opera.exe", L"opera_gx.exe", L"vivaldi.exe",
            L"arc.exe", L"zen.exe", L"librewolf.exe", L"thorium.exe", L"iexplore.exe"});
        add(Category::document, {L"winword.exe", L"excel.exe", L"powerpnt.exe", L"onenote.exe", L"outlook.exe", L"olk.exe", L"notepad.exe",
            L"notepad++.exe", L"obsidian.exe", L"notion.exe", L"acrobat.exe", L"acrord32.exe", L"sumatrapdf.exe", L"wps.exe", L"et.exe",
            L"wpp.exe", L"soffice.bin", L"typora.exe", L"logseq.exe", L"evernote.exe", L"explorer.exe", L"line.exe", L"telegram.exe",
            L"whatsapp.exe", L"thunderbird.exe"});
        add(Category::code, {L"code.exe", L"cursor.exe", L"devenv.exe", L"idea64.exe", L"pycharm64.exe", L"clion64.exe", L"rider64.exe",
            L"webstorm64.exe", L"goland64.exe", L"studio64.exe", L"windowsterminal.exe", L"wt.exe", L"cmd.exe", L"powershell.exe", L"pwsh.exe",
            L"sublime_text.exe", L"zed.exe", L"conhost.exe"});
        add(Category::call, {L"teams.exe", L"ms-teams.exe", L"zoom.exe", L"discord.exe", L"slack.exe", L"webex.exe", L"skype.exe",
            L"lync.exe", L"googlemeet.exe"});
        add(Category::video, {L"vlc.exe", L"mpc-hc64.exe", L"mpc-hc.exe", L"mpc-be64.exe", L"potplayermini64.exe", L"potplayermini.exe",
            L"wmplayer.exe", L"video.ui.exe", L"microsoft.media.player.exe", L"mpv.exe", L"kmplayer64x.exe", L"netflix.exe",
            L"spotify.exe", L"applemusic.exe", L"music.ui.exe", L"plex.exe", L"kodi.exe"});
        add(Category::creative, {L"adobe premiere pro.exe", L"afterfx.exe", L"photoshop.exe", L"lightroom.exe", L"resolve.exe", L"blender.exe",
            L"handbrake.exe", L"ffmpeg.exe", L"obs64.exe", L"capcut.exe", L"davinci resolve.exe", L"unity.exe", L"unrealeditor.exe",
            L"illustrator.exe", L"audition.exe", L"fl64.exe", L"ableton live 12 suite.exe", L"reaper.exe", L"matlab.exe", L"msbuild.exe",
            L"cl.exe", L"rustc.exe", L"cargo.exe", L"gradle.exe", L"java.exe", L"python.exe", L"node.exe"});
        add(Category::launcher, {L"steam.exe", L"steamwebhelper.exe", L"epicgameslauncher.exe", L"battle.net.exe", L"eadesktop.exe",
            L"ubisoftconnect.exe", L"galaxyclient.exe", L"riotclientux.exe", L"xboxpcapp.exe"});
        add(Category::game, {L"minecraft.exe", L"javaw.exe", L"leagueclient.exe", L"league of legends.exe", L"valorant-win64-shipping.exe",
            L"genshinimpact.exe", L"yuanshen.exe", L"starrail.exe", L"zenlesszonezero.exe", L"cs2.exe", L"dota2.exe", L"overwatch.exe",
            L"fortniteclient-win64-shipping.exe", L"r5apex.exe", L"eldenring.exe", L"cyberpunk2077.exe", L"gta5.exe", L"rocketleague.exe",
            L"osu!.exe", L"robloxplayerbeta.exe"});
        return m;
    }();
    const std::wstring name = lower(exe);
    if (auto it = known.find(name); it != known.end()) return it->second;
    // Most Unreal games ship as "<name>-win64-shipping.exe".
    if (name.find(L"-win64-shipping") != std::wstring::npos || name.find(L"-wingdk-shipping") != std::wstring::npos) return Category::game;
    return Category::other;
}
const wchar_t* categoryKey(Category c) {
    switch (c) {
    case Category::browser: return L"瀏覽器";
    case Category::document: return L"文書／閱讀";
    case Category::code: return L"程式開發";
    case Category::call: return L"通訊／會議";
    case Category::video: return L"影音播放";
    case Category::creative: return L"創作／運算";
    case Category::game: return L"遊戲";
    case Category::launcher: return L"遊戲平台";
    default: return L"其他";
    }
}
const wchar_t* ruleKey(AppRule r) {
    switch (r) {
    case AppRule::saving: return L"一律省電";
    case AppRule::balanced: return L"固定平衡";
    case AppRule::performance: return L"效能優先";
    case AppRule::exempt: return L"不要管理";
    default: return L"自動判斷";
    }
}
const wchar_t* inputKey(InputKind k) {
    switch (k) {
    case InputKind::pointer: return L"移動游標";
    case InputKind::click: return L"點擊";
    case InputKind::scroll: return L"捲動";
    case InputKind::typing: return L"打字";
    default: return L"操作";
    }
}

GpuActivity summarizeGpu(const std::vector<std::pair<std::wstring, double>>& instances) {
    GpuActivity g; g.known = true;
    // Engines are summed per adapter and type, then the busiest adapter counts.
    std::map<std::pair<std::wstring, std::wstring>, double> perAdapter;
    for (const auto& [name, percent] : instances) {
        uint32_t pid = 0; std::wstring engine;
        if (!std::isfinite(percent) || percent <= 0 || !parseGpuInstance(name, pid, engine)) continue;
        const size_t luid = name.find(L"luid_"), phys = name.find(L"_phys_");
        const std::wstring adapter = luid != std::wstring::npos && phys != std::wstring::npos && phys > luid ? name.substr(luid, phys - luid) : L"";
        const std::wstring type = lower(engine);
        auto& use = g.processes[pid];
        if (type == L"3d") { use.engine3d = std::min(100.0, use.engine3d + percent); perAdapter[{adapter, L"3d"}] += percent; }
        else if (type.rfind(L"videodecode", 0) == 0) { use.decode = std::min(100.0, use.decode + percent); perAdapter[{adapter, L"decode"}] += percent; }
        else if (type.rfind(L"compute", 0) == 0) { use.compute = std::min(100.0, use.compute + percent); perAdapter[{adapter, L"compute"}] += percent; }
    }
    for (const auto& [key, sum] : perAdapter) {
        const double v = std::min(100.0, sum);
        if (key.second == L"3d") g.total.engine3d = std::max(g.total.engine3d, v);
        else if (key.second == L"decode") g.total.decode = std::max(g.total.decode, v);
        else g.total.compute = std::max(g.total.compute, v);
    }
    for (auto it = g.processes.begin(); it != g.processes.end();)
        it = it->second.engine3d + it->second.decode + it->second.compute <= 0 ? g.processes.erase(it) : std::next(it);
    return g;
}

AppRule Detector::ruleFor(const std::wstring& exe, bool* learned) const {
    if (learned) *learned = false;
    const std::wstring name = lower(exe);
    if (auto it = rules_.find(name); it != rules_.end() && it->second != AppRule::automatic) return it->second;
    if (auto it = learned_.find(name); it != learned_.end() && it->second != AppRule::automatic) { if (learned) *learned = true; return it->second; }
    return AppRule::automatic;
}

Assessment Detector::assess(const Observation& o) {
    Assessment a;
    a.category = categoryOf(o.foregroundExe);
    a.rule = ruleFor(o.foregroundExe, &a.learnedRule);
    auto& c = a.candidates;
    const EngineUse fg = [&] { auto it = o.gpu.processes.find(o.foregroundPid); return it != o.gpu.processes.end() ? it->second : EngineUse{}; }();
    const bool decoding = o.gpu.known && decodeBusy(o.gpu.total);
    const double cpu = o.systemCpu.value_or(0);
    // "Active" input: a key, click or wheel. Without raw input, any input counts.
    const double active = o.activeSeconds.value_or(o.idleSeconds);
    // Whether anything on screen is moving: the compositor's GPU use when known (the frame
    // counter stays near 0.5 per second on some PCs whatever is shown), else its frame count.
    const bool screenStatic = o.compositorGpu ? *o.compositorGpu < 1.5 : o.framesPerSecond && *o.framesPerSecond < 5;
    const bool screenMoving = o.compositorGpu ? *o.compositorGpu >= 4 : o.framesPerSecond && *o.framesPerSecond >= 10;

    // Sustained busy time, so a short spike (opening a program) is not "heavy".
    if (cpu >= 25) { if (!busySince_) busySince_ = o.ms ? o.ms : 1; } else busySince_ = 0;
    if (o.gpu.known && std::max(o.gpu.total.engine3d, o.gpu.total.compute) >= 40 && !o.foregroundCoversScreen) { if (!gpuBusySince_) gpuBusySince_ = o.ms ? o.ms : 1; }
    else if (o.gpu.known) gpuBusySince_ = 0;
    const double busyFor = busySince_ ? (o.ms - busySince_) / 1000.0 : 0, gpuBusyFor = gpuBusySince_ ? (o.ms - gpuBusySince_) / 1000.0 : 0;

    if (o.locked || o.displayOff) c.push_back({Activity::idle, 1, o.locked ? L"已鎖定螢幕" : L"螢幕已關閉"});
    if (o.call) c.push_back({Activity::call, a.category == Category::call ? 1 : .9, L"麥克風或攝影機使用中"});
    if (o.exclusiveFullscreen) c.push_back({Activity::game, 1, L"獨佔全螢幕的 3D 程式"});
    if (o.foregroundCoversScreen && !decoding && a.category != Category::browser && a.category != Category::video) {
        if (fg.engine3d >= 20 || (a.category == Category::game && fg.engine3d >= 5))
            c.push_back({Activity::game, a.category == Category::game ? .95 : .85, L"全螢幕且大量使用顯示卡 3D"});
    }
    if (a.category == Category::game && fg.engine3d >= 30 && !o.foregroundCoversScreen) c.push_back({Activity::game, .8, L"視窗模式遊戲"});
    if (active < ResponsiveSeconds) c.push_back({Activity::responsive, 1, inputKey(o.lastInput)});
    else if (o.foregroundSwitchSeconds < 2) c.push_back({Activity::responsive, .9, L"剛切換程式"});
    // Input or a switch with the CPU jumping: a program opening or a page loading, worth a short boost.
    if ((active < ResponsiveSeconds || o.foregroundSwitchSeconds < ResponsiveSeconds) && cpu >= 30)
        c.push_back({Activity::burst, .95, L"剛開啟或載入（CPU 尖峰）"});
    if (busyFor >= HeavyAfterSeconds) c.push_back({Activity::heavy, std::min(.95, .65 + (cpu - 25) / 100), L"CPU 持續忙碌"});
    if (gpuBusyFor >= HeavyAfterSeconds) c.push_back({Activity::heavy, .75, L"顯示卡持續運算"});
    if (a.category == Category::creative && cpu >= 15) c.push_back({Activity::heavy, .75, L"創作／運算程式忙碌中"});
    if (decoding && (o.idleSeconds >= 5 || o.foregroundCoversScreen))
        c.push_back({Activity::media, o.foregroundCoversScreen ? .95 : .9, o.audio ? L"正在播放影片" : L"正在播放影片（靜音）"});
    else if (o.audio && o.idleSeconds >= 20)
        c.push_back({Activity::media, a.category == Category::video ? .9 : .75, L"正在播放聲音"});
    const bool idle = o.idleSeconds >= IdleAfterSeconds && !o.audio && !decoding && cpu < 15 && !screenMoving;
    if (idle) c.push_back({Activity::idle, screenStatic ? .9 : .7, L"超過 2 分鐘沒操作、畫面靜止"});
    else if (o.idleSeconds >= ReadingAfterSeconds) {
        const bool readable = a.category == Category::browser || a.category == Category::document || a.category == Category::code;
        if (o.idleSeconds >= IdleAfterSeconds && screenMoving) c.push_back({Activity::reading, .75, L"沒操作但畫面在動"});
        else c.push_back({Activity::reading, readable ? .8 : .65, L"一段時間沒操作"});
    }
    if (o.idleSeconds < ReadingAfterSeconds) c.push_back({Activity::interactive, .7, o.lastInput == InputKind::pointer ? L"只移動游標" : L"最近有操作"});

    // The most demanding activity that is confidently present wins; ties go to confidence.
    std::stable_sort(c.begin(), c.end(), [](const Candidate& x, const Candidate& y) { return x.confidence > y.confidence; });
    static const Candidate unsure{Activity::interactive, .5, L"無法確定，維持一般"};
    const Candidate* best = nullptr;
    const bool lockedOrOff = o.locked || o.displayOff;
    for (const auto& k : c) {
        if (k.confidence < MinConfidence) continue;
        if (lockedOrOff && k.activity != Activity::idle && k.activity != Activity::call) continue; // nobody is looking
        if (!best || rank(k.activity) > rank(best->activity)) best = &k;
    }
    if (!best) best = &unsure;
    a.activity = best->activity; a.confidence = best->confidence; a.reason = best->reason;

    // The program's rule, if any, adjusts the result (never during a call or a game).
    if (a.activity != Activity::game && a.activity != Activity::call && !lockedOrOff) {
        switch (a.rule) {
        case AppRule::exempt: a.activity = Activity::exempt; a.confidence = 1; break;
        case AppRule::performance: if (rank(a.activity) < rank(Activity::heavy)) { a.activity = Activity::heavy; a.confidence = 1; } break;
        case AppRule::balanced: a.activity = Activity::interactive; a.confidence = 1; break;
        case AppRule::saving: if (a.activity != Activity::idle) { a.activity = Activity::reading; a.confidence = 1; } break;
        default: break;
        }
        if (a.rule != AppRule::automatic) a.reason += std::wstring(L" · 程式規則：") + ruleKey(a.rule);
    }
    return a;
}

void DetectionTally::add(const std::wstring& workload, Activity detected, double seconds) {
    if (workload.empty() || seconds <= 0 || detected == Activity::count) return;
    auto& row = seconds_[workload];
    row[size_t(detected)] += seconds;
}
std::vector<Activity> DetectionTally::expected(const std::wstring& w) {
    if (w == L"idle") return {Activity::idle, Activity::reading};
    if (w == L"browse" || w == L"scroll") return {Activity::reading, Activity::interactive};
    if (w == L"office" || w == L"bursts") return {Activity::reading, Activity::interactive, Activity::idle};
    if (w == L"video") return {Activity::media};
    if (w == L"sustained") return {Activity::heavy};
    return {};
}
Value DetectionTally::accuracy(const std::wstring& workload) const {
    auto it = seconds_.find(workload);
    const auto want = expected(workload);
    if (it == seconds_.end() || want.empty()) return std::nullopt;
    double total = 0, right = 0;
    for (size_t i = 0; i < it->second.size(); ++i) {
        total += it->second[i];
        if (std::find(want.begin(), want.end(), Activity(i)) != want.end()) right += it->second[i];
    }
    if (total < 1) return std::nullopt;
    return right / total;
}
Value DetectionTally::overall() const {
    double total = 0, right = 0;
    for (const auto& [workload, row] : seconds_) {
        const auto want = expected(workload);
        if (want.empty()) continue;
        for (size_t i = 0; i < row.size(); ++i) {
            total += row[i];
            if (std::find(want.begin(), want.end(), Activity(i)) != want.end()) right += row[i];
        }
    }
    if (total < 1) return std::nullopt;
    return right / total;
}
std::wstring DetectionTally::csv() const {
    std::wstring out = L"workload,activity,seconds,expected\n";
    for (const auto& [workload, row] : seconds_) {
        const auto want = expected(workload);
        for (size_t i = 0; i < row.size(); ++i) {
            if (row[i] <= 0) continue;
            wchar_t seconds[32]; swprintf(seconds, 32, L"%.0f", row[i]);
            const bool ok = std::find(want.begin(), want.end(), Activity(i)) != want.end();
            out += workload + L"," + activityKey(Activity(i)) + L"," + seconds + L"," + (want.empty() ? L"" : ok ? L"yes" : L"no") + L"\n";
        }
    }
    return out;
}
std::wstring DetectionTally::summary() const {
    std::wstring out;
    for (const auto& [workload, row] : seconds_) {
        const auto score = accuracy(workload);
        if (!score) continue;
        // The most common wrong answer, to show what it was confused with.
        const auto want = expected(workload);
        size_t wrong = size_t(Activity::count); double wrongSeconds = 0;
        for (size_t i = 0; i < row.size(); ++i)
            if (std::find(want.begin(), want.end(), Activity(i)) == want.end() && row[i] > wrongSeconds) { wrong = i; wrongSeconds = row[i]; }
        wchar_t percent[16]; swprintf(percent, 16, L"%.0f%%", *score * 100);
        out += L"\n" + workload + L"：" + percent;
        if (wrong < size_t(Activity::count)) out += std::wstring(L"（誤判最多：") + activityKey(Activity(wrong)) + L"）";
    }
    if (auto all = overall()) { wchar_t percent[16]; swprintf(percent, 16, L"%.0f%%", *all * 100); out = std::wstring(L"\n整體：") + percent + out; }
    return out;
}
}
