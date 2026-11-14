#include "stability.h"
#include "version.h"

#include <shlobj.h>
#include <winevt.h>
#include <dbghelp.h>

#include <algorithm>
#include <array>
#include <iterator>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace bs::stability {
namespace {

std::string toUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(std::max(bytes, 0)), '\0');
    if (bytes > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), out.data(), bytes, nullptr, nullptr);
    return out;
}
std::wstring fromUtf8(const std::string& text) {
    if (text.empty()) return {};
    const int chars = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring out(size_t(std::max(chars, 0)), L'\0');
    if (chars > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), chars);
    return out;
}
uint32_t fnv1a(const std::string& text) {
    uint32_t h = 2166136261u;
    for (unsigned char c : text) { h ^= c; h *= 16777619u; }
    return h;
}
uint64_t fileTimeValue(const FILETIME& t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; }
bool startsWith(const std::wstring& text, const wchar_t* prefix) { return text.rfind(prefix, 0) == 0; }
std::wstring oneLine(std::wstring text) {
    for (auto& c : text) if (c == L'\t' || c == L'\r' || c == L'\n') c = L' ';
    return text;
}
std::optional<uint64_t> parseNumber(const std::wstring& text) {
    if (text.empty()) return std::nullopt;
    wchar_t* end = nullptr;
    const bool hex = text.size() > 2 && text[0] == L'0' && (text[1] == L'x' || text[1] == L'X');
    const unsigned long long v = std::wcstoull(text.c_str(), &end, hex ? 16 : 10);
    if (end == text.c_str()) return std::nullopt;
    return uint64_t(v);
}

} // namespace

// ---- Time.
uint64_t nowUtc() { FILETIME t{}; GetSystemTimeAsFileTime(&t); return fileTimeValue(t); }
uint64_t bootUtc() { return nowUtc() - GetTickCount64() * 10000ull; }
std::optional<uint64_t> parseIsoUtc(const std::wstring& text) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (text.size() < 19 || swscanf(text.c_str(), L"%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s) != 6) return std::nullopt;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60) return std::nullopt;
    uint64_t fraction = 0;
    if (text.size() > 20 && text[19] == L'.') {
        int digits = 0;
        for (size_t i = 20; i < text.size() && iswdigit(text[i]) && digits < 7; ++i, ++digits) fraction = fraction * 10 + uint64_t(text[i] - L'0');
        for (; digits < 7; ++digits) fraction *= 10;
    }
    // Days from the civil date (Howard Hinnant's algorithm).
    const int yy = y - (mo <= 2);
    const int era = (yy >= 0 ? yy : yy - 399) / 400;
    const unsigned yoe = unsigned(yy - era * 400);
    const unsigned doy = unsigned((153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = int64_t(era) * 146097 + int64_t(doe) - 719468; // since 1970-01-01
    const int64_t seconds = days * 86400 + h * 3600 + mi * 60 + s + 11644473600ll;
    if (seconds < 0) return std::nullopt;
    return uint64_t(seconds) * Second + fraction;
}
std::wstring localTimeText(uint64_t utc, bool seconds) {
    FILETIME ft{DWORD(utc & 0xFFFFFFFF), DWORD(utc >> 32)};
    SYSTEMTIME u{}, l{};
    if (!FileTimeToSystemTime(&ft, &u) || !SystemTimeToTzSpecificLocalTime(nullptr, &u, &l)) return L"?";
    wchar_t text[40];
    if (seconds) swprintf(text, 40, L"%04u-%02u-%02u %02u:%02u:%02u", l.wYear, l.wMonth, l.wDay, l.wHour, l.wMinute, l.wSecond);
    else swprintf(text, 40, L"%04u-%02u-%02u %02u:%02u", l.wYear, l.wMonth, l.wDay, l.wHour, l.wMinute);
    return text;
}

// ---- Blue screen codes.
uint32_t normalizeBugcheck(uint32_t code) {
    // The 0x1000xxxx forms are the same stop codes with parameters in another layout.
    if ((code & 0xF0000000u) == 0x10000000u) code &= 0x0FFFFFFFu;
    return code;
}
namespace {
struct StopCode { uint32_t code; const wchar_t* name; Area area; };
constexpr StopCode StopCodes[] = {
    {0x0A, L"IRQL_NOT_LESS_OR_EQUAL", Area::driver},
    {0x19, L"BAD_POOL_HEADER", Area::memory},
    {0x1A, L"MEMORY_MANAGEMENT", Area::memory},
    {0x1E, L"KMODE_EXCEPTION_NOT_HANDLED", Area::driver},
    {0x3B, L"SYSTEM_SERVICE_EXCEPTION", Area::driver},
    {0x50, L"PAGE_FAULT_IN_NONPAGED_AREA", Area::memory},
    {0x7A, L"KERNEL_DATA_INPAGE_ERROR", Area::storage},
    {0x7E, L"SYSTEM_THREAD_EXCEPTION_NOT_HANDLED", Area::driver},
    {0x7F, L"UNEXPECTED_KERNEL_MODE_TRAP", Area::processor},
    {0x9C, L"MACHINE_CHECK_EXCEPTION", Area::processor},
    {0x9F, L"DRIVER_POWER_STATE_FAILURE", Area::power},
    {0xA0, L"INTERNAL_POWER_ERROR", Area::power},
    {0xC2, L"BAD_POOL_CALLER", Area::driver},
    {0xD1, L"DRIVER_IRQL_NOT_LESS_OR_EQUAL", Area::driver},
    {0xE2, L"MANUALLY_INITIATED_CRASH", Area::freeze},
    {0xEF, L"CRITICAL_PROCESS_DIED", Area::system},
    {0xF4, L"CRITICAL_OBJECT_TERMINATION", Area::storage},
    {0x101, L"CLOCK_WATCHDOG_TIMEOUT", Area::processor},
    {0x109, L"CRITICAL_STRUCTURE_CORRUPTION", Area::memory},
    {0x10D, L"WDF_VIOLATION", Area::driver},
    {0x113, L"VIDEO_DXGKRNL_FATAL_ERROR", Area::graphics},
    {0x116, L"VIDEO_TDR_FAILURE", Area::graphics},
    {0x117, L"VIDEO_TDR_TIMEOUT_DETECTED", Area::graphics},
    {0x119, L"VIDEO_SCHEDULER_INTERNAL_ERROR", Area::graphics},
    {0x124, L"WHEA_UNCORRECTABLE_ERROR", Area::processor},
    {0x12B, L"FAULTY_HARDWARE_CORRUPTED_PAGE", Area::memory},
    {0x133, L"DPC_WATCHDOG_VIOLATION", Area::driver},
    {0x139, L"KERNEL_SECURITY_CHECK_FAILURE", Area::driver},
    {0x13A, L"KERNEL_MODE_HEAP_CORRUPTION", Area::driver},
    {0x141, L"VIDEO_ENGINE_TIMEOUT_DETECTED", Area::graphics},
    {0x144, L"BUGCODE_USB3_DRIVER", Area::driver},
    {0x14C, L"FATAL_ABNORMAL_RESET_ERROR", Area::system},
    {0x14F, L"PDC_WATCHDOG_TIMEOUT", Area::power},
    {0x154, L"UNEXPECTED_STORE_EXCEPTION", Area::storage},
    {0x15F, L"CONNECTED_STANDBY_WATCHDOG_TIMEOUT_LIVEDUMP", Area::power},
    {0x193, L"VIDEO_DXGKRNL_LIVEDUMP", Area::graphics},
    {0x1C8, L"MANUALLY_INITIATED_POWER_BUTTON_HOLD", Area::freeze},
    {0xC000021A, L"STATUS_SYSTEM_PROCESS_TERMINATED", Area::system},
};
const StopCode* findStop(uint32_t code) {
    code = normalizeBugcheck(code);
    for (const auto& s : StopCodes) if (s.code == code) return &s;
    return nullptr;
}
} // namespace
const wchar_t* bugcheckName(uint32_t code) { const auto* s = findStop(code); return s ? s->name : nullptr; }
Area bugcheckArea(uint32_t code) { const auto* s = findStop(code); return s ? s->area : Area::unknown; }
const wchar_t* areaKey(Area area) {
    switch (area) {
    case Area::storage: return L"storage";
    case Area::memory: return L"memory";
    case Area::graphics: return L"graphics";
    case Area::power: return L"power";
    case Area::processor: return L"processor";
    case Area::driver: return L"driver";
    case Area::system: return L"system";
    case Area::freeze: return L"freeze";
    default: return L"unknown";
    }
}

// ---- Black box.
std::string encodeSlot(const Record& r) {
    std::string text = toUtf8(oneLine(r.text));
    char head[96];
    // Room for the header and the newline; cut on a UTF-8 character boundary.
    const size_t room = SlotBytes - 1 - 80;
    if (text.size() > room) {
        size_t cut = room;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
        text.resize(cut);
    }
    const int n = std::snprintf(head, sizeof(head), "BS1 %llu %llu %llu %lu %08x|", static_cast<unsigned long long>(r.seq),
        static_cast<unsigned long long>(r.time), static_cast<unsigned long long>(r.boot), static_cast<unsigned long>(r.pid), fnv1a(text));
    std::string slot(head, size_t(std::max(n, 0)));
    slot += text;
    slot.resize(SlotBytes - 1, ' ');
    slot += '\n';
    return slot;
}
std::optional<Record> decodeSlot(const char* data, size_t size) {
    if (size < 8 || std::memcmp(data, "BS1 ", 4) != 0) return std::nullopt;
    std::string slot(data, std::min(size, SlotBytes));
    const size_t bar = slot.find('|');
    if (bar == std::string::npos) return std::nullopt;
    unsigned long long seq = 0, time = 0, boot = 0; unsigned long pid = 0; unsigned hash = 0;
    if (std::sscanf(slot.c_str(), "BS1 %llu %llu %llu %lu %8x|", &seq, &time, &boot, &pid, &hash) != 5) return std::nullopt;
    std::string text = slot.substr(bar + 1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\n' || text.back() == '\0')) text.pop_back();
    if (fnv1a(text) != hash) return std::nullopt; // torn or foreign
    Record r; r.seq = seq; r.time = time; r.boot = boot; r.pid = DWORD(pid); r.text = fromUtf8(text);
    return r;
}
std::map<std::wstring, std::wstring> fields(const std::wstring& text) {
    std::map<std::wstring, std::wstring> out;
    std::wistringstream in(text);
    std::wstring token;
    while (in >> token) {
        const size_t eq = token.find(L'=');
        if (eq != std::wstring::npos && eq > 0) out[token.substr(0, eq)] = token.substr(eq + 1);
    }
    return out;
}
bool BlackBox::open(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_ != INVALID_HANDLE_VALUE) return true;
    uint64_t last = 0;
    for (const auto& r : readFile(path)) last = std::max(last, r.seq);
    file_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file_, &size) && uint64_t(size.QuadPart) < Slots * SlotBytes) {
        // Fill with blank slots once so later writes never extend the file.
        const std::string blank(SlotBytes - 1, ' ');
        std::string all; all.reserve(Slots * SlotBytes);
        for (size_t i = 0; i < Slots; ++i) { all += blank; all += '\n'; }
        DWORD written = 0;
        SetFilePointer(file_, 0, nullptr, FILE_BEGIN);
        WriteFile(file_, all.data(), DWORD(all.size()), &written, nullptr);
    }
    seq_ = last + 1; boot_ = bootUtc(); path_ = path;
    return true;
}
void BlackBox::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    file_ = INVALID_HANDLE_VALUE;
}
bool BlackBox::isOpen() const { std::lock_guard<std::mutex> lock(mutex_); return file_ != INVALID_HANDLE_VALUE; }
void BlackBox::write(const std::wstring& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_ == INVALID_HANDLE_VALUE) return;
    Record r; r.seq = seq_++; r.time = nowUtc(); r.boot = boot_; r.pid = GetCurrentProcessId(); r.text = text;
    const std::string slot = encodeSlot(r);
    OVERLAPPED at{};
    const uint64_t offset = (r.seq % Slots) * SlotBytes;
    at.Offset = DWORD(offset & 0xFFFFFFFF); at.OffsetHigh = DWORD(offset >> 32);
    DWORD written = 0;
    WriteFile(file_, slot.data(), DWORD(slot.size()), &written, &at);
}
std::vector<Record> BlackBox::records() const {
    std::wstring path;
    { std::lock_guard<std::mutex> lock(mutex_); path = path_; }
    return path.empty() ? std::vector<Record>{} : readFile(path);
}
std::vector<Record> BlackBox::readFile(const std::wstring& path) {
    std::vector<Record> out;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return out;
    std::string data(Slots * SlotBytes, '\0');
    DWORD read = 0;
    ReadFile(file, data.data(), DWORD(data.size()), &read, nullptr);
    CloseHandle(file);
    for (size_t at = 0; at + 8 <= read; at += SlotBytes)
        if (auto r = decodeSlot(data.data() + at, std::min<size_t>(SlotBytes, read - at))) out.push_back(std::move(*r));
    std::sort(out.begin(), out.end(), [](const Record& a, const Record& b) { return a.seq < b.seq; });
    return out;
}
BlackBox& blackBox() { static BlackBox box; return box; }
void note(const std::wstring& text) { blackBox().write(L"ev " + text); }

// ---- The previous session.
PreviousSession classify(const std::vector<Record>& records, uint64_t currentBoot) {
    PreviousSession s;
    if (records.empty()) return s;
    const Record& last = records.back();
    s.lastTime = last.time; s.lastBoot = last.boot;
    // That session's records: same process, from its last start on.
    size_t first = records.size() - 1;
    while (first > 0 && records[first - 1].pid == last.pid && records[first - 1].boot == last.boot && !startsWith(records[first].text, L"start")) --first;
    bool asleep = false, hung = false;
    for (size_t i = first; i < records.size(); ++i) {
        const auto& t = records[i].text;
        if (startsWith(t, L"hb ")) s.lastHeartbeat = t.substr(3);
        else if (t == L"ev suspend") asleep = true;
        else if (t == L"ev resume") asleep = false;
        else if (startsWith(t, L"ev hang")) hung = true;
        else if (startsWith(t, L"ev responsive")) hung = false;
        if (records[i].time + 5 * 60 * Second >= last.time) s.tail.push_back(records[i]);
    }
    if (startsWith(last.text, L"exit")) { s.ending = Ending::clean; return s; }
    if (currentBoot > last.time) {
        const auto f = fields(s.lastHeartbeat);
        const auto ac = f.find(L"ac"), pct = f.find(L"pct");
        if (ac != f.end() && ac->second == L"0" && pct != f.end() && std::wcstod(pct->second.c_str(), nullptr) <= 4) s.ending = Ending::batteryEmpty;
        else s.ending = asleep ? Ending::systemDownAsleep : Ending::systemDown;
        return s;
    }
    s.ending = hung ? Ending::appHung : Ending::appEnded;
    return s;
}

// ---- Event log.
std::wstring LogEvent::value(const std::wstring& name) const {
    for (const auto& [k, v] : data) if (k == name) return v;
    return {};
}
namespace {
std::wstring unescape(const std::wstring& text) {
    std::wstring out; out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != L'&') { out += text[i]; continue; }
        const size_t semi = text.find(L';', i);
        if (semi == std::wstring::npos || semi - i > 10) { out += text[i]; continue; }
        const std::wstring entity = text.substr(i + 1, semi - i - 1);
        if (entity == L"amp") out += L'&';
        else if (entity == L"lt") out += L'<';
        else if (entity == L"gt") out += L'>';
        else if (entity == L"quot") out += L'"';
        else if (entity == L"apos") out += L'\'';
        else if (!entity.empty() && entity[0] == L'#') {
            const bool hex = entity.size() > 1 && (entity[1] == L'x' || entity[1] == L'X');
            out += wchar_t(std::wcstoul(entity.c_str() + (hex ? 2 : 1), nullptr, hex ? 16 : 10));
        } else { out += text.substr(i, semi - i + 1); }
        i = semi;
    }
    return out;
}
// The value of attr="..." or attr='...' inside the tag that starts at from.
std::wstring attribute(const std::wstring& xml, size_t from, const wchar_t* attr) {
    const size_t end = xml.find(L'>', from);
    const std::wstring key = std::wstring(attr) + L"=";
    size_t at = xml.find(key, from);
    if (at == std::wstring::npos || at > end) return {};
    at += key.size();
    if (at >= xml.size()) return {};
    const wchar_t quote = xml[at];
    if (quote != L'\'' && quote != L'"') return {};
    const size_t close = xml.find(quote, at + 1);
    return close == std::wstring::npos ? std::wstring() : unescape(xml.substr(at + 1, close - at - 1));
}
std::wstring element(const std::wstring& xml, const wchar_t* name) {
    const std::wstring open = std::wstring(L"<") + name;
    size_t at = xml.find(open);
    while (at != std::wstring::npos && at + open.size() < xml.size() && xml[at + open.size()] != L'>' && xml[at + open.size()] != L' ')
        at = xml.find(open, at + 1);
    if (at == std::wstring::npos) return {};
    const size_t gt = xml.find(L'>', at);
    if (gt == std::wstring::npos || xml[gt - 1] == L'/') return {};
    const size_t close = xml.find(L"</" + std::wstring(name) + L">", gt);
    return close == std::wstring::npos ? std::wstring() : unescape(xml.substr(gt + 1, close - gt - 1));
}
} // namespace
std::optional<LogEvent> parseEventXml(const std::wstring& xml) {
    LogEvent e;
    const size_t provider = xml.find(L"<Provider");
    if (provider == std::wstring::npos) return std::nullopt;
    e.provider = attribute(xml, provider, L"Name");
    const auto id = parseNumber(element(xml, L"EventID"));
    if (!id) return std::nullopt;
    e.id = uint32_t(*id & 0xFFFF);
    if (const auto level = parseNumber(element(xml, L"Level"))) e.level = int(*level);
    e.channel = element(xml, L"Channel");
    const size_t created = xml.find(L"<TimeCreated");
    if (created != std::wstring::npos) if (auto t = parseIsoUtc(attribute(xml, created, L"SystemTime"))) e.time = *t;
    // <EventData><Data Name='x'>v</Data>...; or <UserData><Something><Leaf>v</Leaf>...
    const size_t eventData = xml.find(L"<EventData");
    if (eventData != std::wstring::npos) {
        const size_t end = xml.find(L"</EventData>", eventData);
        size_t at = xml.find(L"<Data", eventData);
        while (at != std::wstring::npos && (end == std::wstring::npos || at < end)) {
            const size_t gt = xml.find(L'>', at);
            if (gt == std::wstring::npos) break;
            const std::wstring name = attribute(xml, at, L"Name");
            std::wstring value;
            if (xml[gt - 1] != L'/') {
                const size_t close = xml.find(L"</Data>", gt);
                if (close == std::wstring::npos) break;
                value = unescape(xml.substr(gt + 1, close - gt - 1));
                at = xml.find(L"<Data", close);
            } else at = xml.find(L"<Data", gt);
            e.data.emplace_back(name, value);
        }
    } else if (const size_t user = xml.find(L"<UserData>"); user != std::wstring::npos) {
        const size_t end = xml.find(L"</UserData>", user);
        size_t at = xml.find(L'<', user + 10);
        if (at != std::wstring::npos) at = xml.find(L'<', at + 1); // skip the wrapper element
        while (at != std::wstring::npos && at < end) {
            if (xml[at + 1] == L'/') { at = xml.find(L'<', at + 1); continue; }
            const size_t gt = xml.find(L'>', at);
            if (gt == std::wstring::npos) break;
            size_t nameEnd = at + 1;
            while (nameEnd < gt && xml[nameEnd] != L' ' && xml[nameEnd] != L'/' && xml[nameEnd] != L'>') ++nameEnd;
            const std::wstring name = xml.substr(at + 1, nameEnd - at - 1);
            const size_t next = xml.find(L'<', gt);
            if (xml[gt - 1] != L'/' && next != std::wstring::npos && xml.compare(next, 2 + name.size(), L"</" + name) == 0)
                e.data.emplace_back(name, unescape(xml.substr(gt + 1, next - gt - 1)));
            at = next;
        }
    }
    return e;
}
std::wstring eventQueryXml(uint64_t sinceUtc, Channel channel) {
    FILETIME ft{DWORD(sinceUtc & 0xFFFFFFFF), DWORD(sinceUtc >> 32)};
    SYSTEMTIME t{}; FileTimeToSystemTime(&ft, &t);
    wchar_t since[40];
    swprintf(since, 40, L"%04u-%02u-%02uT%02u:%02u:%02u.000Z", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    const std::wstring time = std::wstring(L" and TimeCreated[@SystemTime&gt;='") + since + L"']";
    // One small Select per source: Windows rejects event queries with too many terms, and
    // the results of all Selects come back together.
    static const std::vector<std::pair<const wchar_t*, const wchar_t*>> selects[] = {
        { // system
            {L"System", L"Provider[@Name='Microsoft-Windows-Kernel-Power'] and EventID=41"},
            {L"System", L"Provider[@Name='EventLog'] and EventID=6008"},
            {L"System", L"Provider[@Name='Microsoft-Windows-WER-SystemErrorReporting']"},
            {L"System", L"Provider[@Name='Microsoft-Windows-WHEA-Logger'] and Level&lt;=3"},
            {L"System", L"Provider[@Name='Display'] and EventID=4101"},
            {L"System", L"(Provider[@Name='nvlddmkm'] or Provider[@Name='amdkmdag'] or Provider[@Name='amdwddmg']) and Level&lt;=2"},
            {L"System", L"Provider[@Name='disk'] and (EventID=7 or EventID=11 or EventID=51 or EventID=153 or EventID=154)"},
            {L"System", L"(Provider[@Name='stornvme'] or Provider[@Name='storahci']) and (EventID=11 or EventID=129)"},
            {L"System", L"Provider[@Name='Ntfs'] and (EventID=55 or EventID=98 or EventID=140)"},
            {L"System", L"Provider[@Name='volmgr'] and (EventID=46 or EventID=161 or EventID=162)"},
            {L"System", L"Provider[@Name='Microsoft-Windows-Kernel-Processor-Power'] and EventID=37"},
        },
        { // application
            {L"Application", L"Provider[@Name='Application Error'] and EventID=1000"},
            {L"Application", L"Provider[@Name='Application Hang'] and EventID=1002"},
            {L"Application", L"Provider[@Name='Windows Error Reporting'] and EventID=1001"},
        },
        { // installs
            {L"System", L"Provider[@Name='Microsoft-Windows-WindowsUpdateClient'] and EventID=19"},
            {L"Application", L"Provider[@Name='MsiInstaller'] and (EventID=1033 or EventID=1034 or EventID=1035)"},
        },
    };
    std::wstring xml = L"<QueryList><Query Id='0'>";
    for (const auto& [path, condition] : selects[int(channel)])
        xml += std::wstring(L"<Select Path='") + path + L"'>*[System[" + condition + time + L"]]</Select>";
    return xml + L"</Query></QueryList>";
}
std::optional<uint64_t> parseShutdownTime(const std::wstring& timeText, const std::wstring& dateText, uint64_t loggedUtc) {
    auto numbers = [](const std::wstring& text) {
        std::vector<int> out; int value = -1;
        for (wchar_t c : text) {
            if (c >= L'0' && c <= L'9') value = (value < 0 ? 0 : value * 10) + (c - L'0');
            else if (value >= 0) { out.push_back(value); value = -1; }
        }
        if (value >= 0) out.push_back(value);
        return out;
    };
    const auto t = numbers(timeText), d = numbers(dateText);
    if (t.size() < 2 || d.size() != 3) return std::nullopt;
    int hour = t[0];
    const int minute = t[1], second = t.size() > 2 ? t[2] : 0;
    const bool pm = timeText.find(L"下午") != std::wstring::npos || timeText.find(L"午後") != std::wstring::npos || timeText.find(L"오후") != std::wstring::npos ||
        timeText.find(L"PM") != std::wstring::npos || timeText.find(L"pm") != std::wstring::npos;
    const bool am = timeText.find(L"上午") != std::wstring::npos || timeText.find(L"午前") != std::wstring::npos || timeText.find(L"오전") != std::wstring::npos ||
        timeText.find(L"AM") != std::wstring::npos || timeText.find(L"am") != std::wstring::npos;
    if (pm && hour < 12) hour += 12;
    if (am && hour == 12) hour = 0;
    // The year has four digits; the other two are month and day in the PC's order. Try both orders.
    std::vector<std::array<int, 3>> candidates; // year, month, day
    if (d[0] >= 1000) candidates.push_back({d[0], d[1], d[2]});
    else if (d[2] >= 1000) { candidates.push_back({d[2], d[0], d[1]}); candidates.push_back({d[2], d[1], d[0]}); }
    std::optional<uint64_t> best;
    for (const auto& c : candidates) {
        if (c[1] < 1 || c[1] > 12 || c[2] < 1 || c[2] > 31 || hour > 23 || minute > 59 || second > 60) continue;
        SYSTEMTIME local{}; local.wYear = WORD(c[0]); local.wMonth = WORD(c[1]); local.wDay = WORD(c[2]);
        local.wHour = WORD(hour); local.wMinute = WORD(minute); local.wSecond = WORD(second);
        SYSTEMTIME utc{}; FILETIME ft{};
        if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc) || !SystemTimeToFileTime(&utc, &ft)) continue;
        const uint64_t value = fileTimeValue(ft);
        // Before the record (it is written at the next start), and within a week of it.
        if (value > loggedUtc + 60 * Second || value + 7ull * 86400 * Second < loggedUtc) continue;
        if (!best || value > *best) best = value;
    }
    return best;
}
namespace {
// Windows Error Reporting files many reports; only crashes, hangs and live kernel dumps matter here.
bool interestingReport(const LogEvent& e) {
    if (e.provider != L"Windows Error Reporting") return true;
    if (e.data.size() < 3) return false;
    const std::wstring& name = e.data[2].second;
    return name == L"LiveKernelEvent" || name == L"BlueScreen" || name == L"APPCRASH" || name == L"AppHangB1" || name == L"AppHangXProcB1" || name == L"MoAppCrash";
}
// The program a crash or hang record is about, so a crash loop can be capped.
std::wstring crashingApp(const LogEvent& e) {
    if (e.provider == L"Windows Error Reporting") return e.data.size() > 5 ? e.data[2].second + L"/" + e.data[5].second : std::wstring();
    if (e.provider == L"Application Error" || e.provider == L"Application Hang") {
        std::wstring app = e.value(L"AppName");
        return app.empty() && !e.data.empty() ? e.data[0].second : app;
    }
    return {};
}
std::vector<LogEvent> runQuery(const std::wstring& query, bool withMessages, size_t limit) {
    std::vector<LogEvent> out;
    EVT_HANDLE results = EvtQuery(nullptr, nullptr, query.c_str(), EvtQueryChannelPath | EvtQueryReverseDirection | EvtQueryTolerateQueryErrors);
    if (!results) return out;
    std::unordered_map<std::wstring, EVT_HANDLE> publishers;
    std::unordered_map<std::wstring, int> perApp;
    std::vector<wchar_t> buffer(16384);
    EVT_HANDLE batch[32];
    DWORD returned = 0;
    while (out.size() < limit && EvtNext(results, 32, batch, 2000, 0, &returned)) {
        for (DWORD i = 0; i < returned; ++i) {
            DWORD used = 0, properties = 0;
            if (!EvtRender(nullptr, batch[i], EvtRenderEventXml, DWORD(buffer.size() * sizeof(wchar_t)), buffer.data(), &used, &properties)
                && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
                buffer.resize(used / sizeof(wchar_t) + 1);
                EvtRender(nullptr, batch[i], EvtRenderEventXml, DWORD(buffer.size() * sizeof(wchar_t)), buffer.data(), &used, &properties);
            }
            auto e = used ? parseEventXml(std::wstring(buffer.data())) : std::nullopt;
            bool keep = e && interestingReport(*e) && out.size() < limit;
            // One program crashing over and over: the newest 20 of each kind are enough.
            if (keep) if (const std::wstring app = crashingApp(*e); !app.empty() && ++perApp[e->provider + L"|" + app] > 20) keep = false;
            if (keep) {
                if (withMessages) {
                    auto found = publishers.find(e->provider);
                    if (found == publishers.end()) found = publishers.emplace(e->provider, EvtOpenPublisherMetadata(nullptr, e->provider.c_str(), nullptr, 0, 0)).first;
                    if (found->second) {
                        std::vector<wchar_t> message(4096);
                        DWORD size = 0;
                        if (!EvtFormatMessage(found->second, batch[i], 0, 0, nullptr, EvtFormatMessageEvent, DWORD(message.size()), message.data(), &size)
                            && GetLastError() == ERROR_INSUFFICIENT_BUFFER && size < 1 << 20) {
                            message.resize(size + 1);
                            EvtFormatMessage(found->second, batch[i], 0, 0, nullptr, EvtFormatMessageEvent, DWORD(message.size()), message.data(), &size);
                        }
                        if (size) e->message = std::wstring(message.data());
                    }
                }
                out.push_back(std::move(*e));
            }
            EvtClose(batch[i]);
        }
    }
    for (auto& [name, handle] : publishers) if (handle) EvtClose(handle);
    EvtClose(results);
    // Repeats beyond the cap, so a report still shows how often it happened.
    for (const auto& [key, count] : perApp) if (count > 20) {
        LogEvent note; note.provider = L"BatteryScope"; note.channel = L"Application";
        note.message = key + L": " + std::to_wstring(count) + L" records, the newest 20 kept";
        out.push_back(note);
    }
    return out;
}
} // namespace
std::vector<LogEvent> readEvents(uint64_t sinceUtc, bool withMessages, size_t limit) {
    // The system's own records first, each channel within its own limit.
    auto out = runQuery(eventQueryXml(sinceUtc, Channel::system), withMessages, limit);
    auto apps = runQuery(eventQueryXml(sinceUtc, Channel::application), withMessages, limit / 2);
    out.insert(out.end(), std::make_move_iterator(apps.begin()), std::make_move_iterator(apps.end()));
    std::stable_sort(out.begin(), out.end(), [](const LogEvent& a, const LogEvent& b) { return a.time > b.time; });
    return out;
}
std::vector<LogEvent> readInstalls(uint64_t sinceUtc) { return runQuery(eventQueryXml(sinceUtc, Channel::installs), false, 300); }
std::vector<std::pair<std::wstring, std::wstring>> knownTroubleDrivers() {
    // Kept to drivers with a documented pattern; each must be installed and not disabled.
    static const std::pair<const wchar_t*, const wchar_t*> known[] = {
        {L"nipcibrd", L"National Instruments PCI bridge driver (LabVIEW, PXI Platform Services): known DRIVER_POWER_STATE_FAILURE (0x9F) on sleep and wake"},
        {L"nipbcfk", L"National Instruments PCI bridge filter (PXI Platform Services), attached to PCI bridges"},
    };
    std::vector<std::pair<std::wstring, std::wstring>> out;
    for (const auto& [name, why] : known) {
        DWORD start = 0, bytes = sizeof(start);
        const std::wstring key = std::wstring(L"SYSTEM\\CurrentControlSet\\Services\\") + name;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"Start", RRF_RT_REG_DWORD, nullptr, &start, &bytes) == ERROR_SUCCESS && start != 4)
            out.emplace_back(std::wstring(name) + L".sys", why);
    }
    return out;
}
std::wstring summarizeKernelDump(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    std::string data(4u << 20, '\0');
    DWORD read = 0;
    ReadFile(file, data.data(), DWORD(data.size()), &read, nullptr);
    CloseHandle(file);
    data.resize(read);
    if (read < 0x60 || data.compare(0, 8, "PAGEDU64") != 0) return {};
    uint32_t code = 0; uint64_t p[4]{};
    std::memcpy(&code, data.data() + 0x38, 4);
    std::memcpy(p, data.data() + 0x40, sizeof(p));
    wchar_t head[200];
    swprintf(head, 200, L"0x%X %ls (0x%llX, 0x%llX, 0x%llX, 0x%llX)", code, bugcheckName(code) ? bugcheckName(code) : L"?",
        static_cast<unsigned long long>(p[0]), static_cast<unsigned long long>(p[1]), static_cast<unsigned long long>(p[2]), static_cast<unsigned long long>(p[3]));
    // Driver objects named in the crash data, in the order they appear: the first ones are
    // usually the device stack the stop code is about.
    std::vector<std::wstring> drivers;
    const std::string marker("\\\0D\0r\0i\0v\0e\0r\0\\\0", 16);
    for (size_t at = data.find(marker); at != std::string::npos && drivers.size() < 16; at = data.find(marker, at + 2)) {
        std::wstring name;
        for (size_t i = at + marker.size(); i + 1 < data.size() && name.size() < 32; i += 2) {
            const wchar_t c = wchar_t(uint8_t(data[i]) | (uint8_t(data[i + 1]) << 8));
            if (!(iswalnum(c) || c == L'_' || c == L'-') || c > 0x7F) break;
            name += c;
        }
        if (name.size() >= 2 && std::find(drivers.begin(), drivers.end(), name) == drivers.end()) drivers.push_back(name);
    }
    std::wstring text = head;
    if (!drivers.empty()) {
        text += L"\r\n  drivers in the crash data:";
        for (const auto& d : drivers) text += L" " + d;
    }
    return text;
}

// ---- Incidents.
const wchar_t* kindKey(Kind kind) {
    switch (kind) {
    case Kind::blueScreen: return L"blue-screen";
    case Kind::powerLoss: return L"power-loss";
    case Kind::forcedOff: return L"forced-off";
    case Kind::batteryEmpty: return L"battery-empty";
    case Kind::freeze: return L"freeze";
    case Kind::graphicsReset: return L"graphics-reset";
    case Kind::hardwareError: return L"hardware-error";
    case Kind::diskError: return L"disk-error";
    case Kind::appCrash: return L"app-crash";
    case Kind::appHang: return L"app-hang";
    case Kind::manual: return L"manual";
    default: return L"";
    }
}
std::optional<Kind> kindFromKey(const std::wstring& key) {
    for (int k = 0; k < int(Kind::count); ++k) if (key == kindKey(Kind(k))) return Kind(k);
    return std::nullopt;
}
bool serious(Kind kind) {
    return kind == Kind::blueScreen || kind == Kind::powerLoss || kind == Kind::forcedOff || kind == Kind::freeze ||
        kind == Kind::graphicsReset || kind == Kind::hardwareError || kind == Kind::appCrash;
}
std::wstring Incident::key() const { return std::wstring(kindKey(kind)) + L"@" + std::to_wstring(time / Second); }
namespace {
// Shutdown kinds describe one crash from different records; the more specific wins.
int shutdownRank(Kind k) {
    switch (k) {
    case Kind::blueScreen: return 4;
    case Kind::forcedOff: return 3;
    case Kind::batteryEmpty: return 2;
    case Kind::powerLoss: return 1;
    default: return 0;
    }
}
uint64_t distance(uint64_t a, uint64_t b) { return a > b ? a - b : b - a; }
bool sameIncident(const Incident& a, const Incident& b) {
    // One crash's records are written within two minutes of the next start; two crashes with
    // different stop codes are never the same one.
    if (shutdownRank(a.kind) && shutdownRank(b.kind))
        return distance(a.time, b.time) <= 2 * 60 * Second && (!a.code || !b.code || a.code == b.code);
    if (a.kind != b.kind) return false;
    if (a.kind == Kind::freeze && a.seconds > 0 && b.seconds > 0) return distance(a.time, b.time) <= 20 * Second;
    return distance(a.time, b.time) <= 3 * 60 * Second;
}
void absorb(Incident& into, const Incident& other) {
    if (shutdownRank(other.kind) > shutdownRank(into.kind)) { into.kind = other.kind; into.code = other.code; }
    if (!into.code && other.code) into.code = other.code;
    if (into.detail.empty()) into.detail = other.detail;
    else if (!other.detail.empty() && into.detail.find(other.detail) == std::wstring::npos && into.detail.size() < 300) into.detail += L"; " + other.detail;
    if (into.context.empty()) into.context = other.context;
    if (into.recentWrites.empty()) into.recentWrites = other.recentWrites;
    into.sleeping = into.sleeping || other.sleeping;
    into.dumpFailed = into.dumpFailed || other.dumpFailed;
    if (!into.wentDown) into.wentDown = other.wentDown;
    into.seconds = std::max(into.seconds, other.seconds);
    // Keep the earlier time: it is closer to when it happened.
    into.time = std::min(into.time, other.time);
}
bool isTrue(const std::wstring& v) { return v == L"true" || v == L"1"; }
} // namespace
std::vector<Incident> incidentsFromEvents(const std::vector<LogEvent>& source) {
    std::vector<LogEvent> events = source;
    std::sort(events.begin(), events.end(), [](const LogEvent& a, const LogEvent& b) { return a.time < b.time; });
    std::vector<Incident> out;
    // The crash a boot-time record belongs to: the one logged within two minutes before it.
    auto crashNear = [&](uint64_t time) -> Incident* {
        for (auto it = out.rbegin(); it != out.rend(); ++it)
            if (shutdownRank(it->kind) && it->time <= time + 30 * Second && time <= it->time + 2 * 60 * Second) return &*it;
        return nullptr;
    };
    auto add = [&](Incident incident) {
        // Warnings that come in bursts are one incident per ten minutes.
        if (incident.kind == Kind::hardwareError || incident.kind == Kind::diskError || incident.kind == Kind::graphicsReset)
            for (auto it = out.rbegin(); it != out.rend(); ++it)
                if (it->kind == incident.kind && distance(it->time, incident.time) <= 10 * 60 * Second) { absorb(*it, incident); return; }
        for (auto it = out.rbegin(); it != out.rend(); ++it)
            if (!shutdownRank(incident.kind) && sameIncident(*it, incident)) { absorb(*it, incident); return; }
        out.push_back(std::move(incident));
    };
    for (const auto& e : events) {
        Incident i; i.time = e.time;
        if (e.provider == L"Microsoft-Windows-Kernel-Power" && e.id == 41) {
            // Each 41 is a start after a crash: always its own incident.
            const uint32_t code = normalizeBugcheck(uint32_t(parseNumber(e.value(L"BugcheckCode")).value_or(0)));
            const bool held = isTrue(e.value(L"LongPowerButtonPressDetected"));
            const bool button = parseNumber(e.value(L"PowerButtonTimestamp")).value_or(0) != 0;
            i.sleeping = parseNumber(e.value(L"SleepInProgress")).value_or(0) != 0 || isTrue(e.value(L"ConnectedStandbyInProgress"));
            if (code == 0x1C8 || code == 0xE2) { i.kind = Kind::forcedOff; i.code = code; }
            else if (code) { i.kind = Kind::blueScreen; i.code = code; }
            else i.kind = held || button ? Kind::forcedOff : Kind::powerLoss;
            if (code) {
                wchar_t text[160];
                swprintf(text, 160, L"0x%X (%ls, %ls, %ls, %ls)", code, e.value(L"BugcheckParameter1").c_str(), e.value(L"BugcheckParameter2").c_str(),
                    e.value(L"BugcheckParameter3").c_str(), e.value(L"BugcheckParameter4").c_str());
                i.detail = text;
            }
            if (Incident* earlier = crashNear(e.time); earlier && !earlier->code && earlier->kind == Kind::powerLoss && earlier->time >= e.time - 30 * Second) {
                Incident merged = i; absorb(merged, *earlier); *earlier = merged; // a 6008 that came first
            } else out.push_back(i);
        } else if (e.provider == L"Microsoft-Windows-WER-SystemErrorReporting") {
            std::wstring p1 = e.value(L"param1");
            if (p1.empty() && !e.data.empty()) p1 = e.data[0].second;
            const uint32_t code = normalizeBugcheck(uint32_t(parseNumber(p1.substr(0, p1.find(L' '))).value_or(0)));
            if (!code) continue;
            i.kind = code == 0x1C8 || code == 0xE2 ? Kind::forcedOff : Kind::blueScreen; i.code = code; i.detail = p1;
            // Written after sign-in, up to some minutes after the start.
            Incident* crash = nullptr;
            for (auto it = out.rbegin(); it != out.rend() && !crash; ++it)
                if (shutdownRank(it->kind) && it->time <= e.time && e.time <= it->time + 15 * 60 * Second && (!it->code || it->code == code)) crash = &*it;
            if (crash) absorb(*crash, i); else out.push_back(i);
        } else if (e.provider == L"EventLog" && e.id == 6008) {
            i.kind = Kind::powerLoss;
            if (e.data.size() >= 2) if (auto down = parseShutdownTime(e.data[0].second, e.data[1].second, e.time)) i.wentDown = *down;
            if (Incident* crash = crashNear(e.time)) absorb(*crash, i); else out.push_back(i);
        } else if (e.provider == L"volmgr" && (e.id == 161 || e.id == 162 || e.id == 46)) {
            // The crash dump could not be written: part of the crash, not a disk error of its own.
            if (Incident* crash = crashNear(e.time)) { crash->dumpFailed = true; continue; }
            i.kind = Kind::diskError; i.dumpFailed = true;
            i.detail = L"volmgr " + std::to_wstring(e.id);
            out.push_back(i); // matched with the next start's 41 below, never merged with another

        } else if (e.provider == L"Display" || e.provider == L"nvlddmkm" || e.provider == L"amdkmdag" || e.provider == L"amdwddmg") {
            i.kind = Kind::graphicsReset;
            i.detail = e.provider + L" " + std::to_wstring(e.id);
            if (e.provider == L"Display" && !e.data.empty()) i.detail = e.data[0].second + L" 4101";
            add(i);
        } else if (e.provider == L"Windows Error Reporting" && e.data.size() > 5 && e.data[2].second == L"LiveKernelEvent") {
            const uint32_t code = uint32_t(std::wcstoul(e.data[5].second.c_str(), nullptr, 16));
            i.code = code;
            i.kind = bugcheckArea(code) == Area::graphics ? Kind::graphicsReset : Kind::freeze;
            i.detail = L"LiveKernelEvent " + e.data[5].second;
            add(i);
        } else if (e.provider == L"Microsoft-Windows-WHEA-Logger") {
            i.kind = Kind::hardwareError;
            i.detail = L"WHEA " + std::to_wstring(e.id);
            const std::wstring errorSource = e.value(L"ErrorSource");
            if (!errorSource.empty()) i.detail += L" source " + errorSource;
            add(i);
        } else if (e.provider == L"disk" || e.provider == L"stornvme" || e.provider == L"storahci" || e.provider == L"Ntfs") {
            i.kind = Kind::diskError;
            i.detail = e.provider + L" " + std::to_wstring(e.id);
            for (const auto& [name, value] : e.data)
                if (value.find(L"\\Device\\") != std::wstring::npos || value.find(L"Harddisk") != std::wstring::npos) { i.detail += L" " + value; break; }
            add(i);
        } else if (e.provider == L"Application Error" || e.provider == L"Application Hang") {
            if (e.data.empty()) continue;
            // Newer builds name the fields; older ones list them in order.
            std::wstring app = e.value(L"AppName");
            if (app.empty()) app = e.data[0].second;
            std::transform(app.begin(), app.end(), app.begin(), ::towlower);
            if (app != L"batteryscope.exe") continue;
            i.kind = e.provider == L"Application Error" ? Kind::appCrash : Kind::appHang;
            std::wstring exception = e.value(L"ExceptionCode");
            if (exception.empty() && e.data.size() > 6) exception = e.data[6].second;
            if (i.kind == Kind::appCrash && !exception.empty()) i.detail = L"exception " + exception;
            add(i);
        }
    }
    // volmgr 161 is logged a moment before the 41 of the same start: fold those in now.
    for (auto it = out.begin(); it != out.end();) {
        if (it->kind == Kind::diskError && it->dumpFailed && it->detail.rfind(L"volmgr", 0) == 0) {
            auto crash = std::find_if(out.begin(), out.end(), [&](const Incident& c) {
                return shutdownRank(c.kind) && c.time >= it->time && c.time <= it->time + 60 * Second; });
            if (crash != out.end()) { crash->dumpFailed = true; it = out.erase(it); continue; }
        }
        ++it;
    }
    return out;
}
std::vector<Incident> merge(std::vector<Incident> known, const std::vector<Incident>& found, std::vector<Incident>* added) {
    const size_t before = known.size();
    for (const auto& f : found) {
        bool matched = false;
        for (auto& k : known) if (sameIncident(k, f)) { absorb(k, f); matched = true; break; }
        if (!matched) known.push_back(f);
    }
    // The new ones as they ended up, after later records of the same incident were folded in.
    if (added) for (size_t i = before; i < known.size(); ++i) added->push_back(known[i]);
    std::sort(known.begin(), known.end(), [](const Incident& a, const Incident& b) { return a.time > b.time; });
    if (known.size() > 100) known.resize(100);
    return known;
}
std::wstring saveLine(const Incident& i) {
    wchar_t head[96];
    swprintf(head, 96, L"%llu\t%ls\t%lu\t%.1f\t%d\t", static_cast<unsigned long long>(i.time), kindKey(i.kind), static_cast<unsigned long>(i.code), i.seconds, int(i.sleeping));
    return head + oneLine(i.detail) + L"\t" + oneLine(i.context) + L"\t" + oneLine(i.recentWrites) + L"\t" + std::to_wstring(i.wentDown) + L"\t" + (i.dumpFailed ? L"1" : L"0");
}
std::optional<Incident> loadLine(const std::wstring& line) {
    std::vector<std::wstring> parts;
    size_t start = 0;
    for (;;) {
        const size_t tab = line.find(L'\t', start);
        parts.push_back(line.substr(start, tab == std::wstring::npos ? std::wstring::npos : tab - start));
        if (tab == std::wstring::npos) break;
        start = tab + 1;
    }
    if (parts.size() < 8) return std::nullopt;
    Incident i;
    const auto time = parseNumber(parts[0]); const auto kind = kindFromKey(parts[1]);
    if (!time || !kind) return std::nullopt;
    i.time = *time; i.kind = *kind;
    i.code = uint32_t(parseNumber(parts[2]).value_or(0));
    i.seconds = std::wcstod(parts[3].c_str(), nullptr);
    i.sleeping = parts[4] == L"1";
    i.detail = parts[5]; i.context = parts[6]; i.recentWrites = parts[7];
    if (parts.size() >= 10) { i.wentDown = parseNumber(parts[8]).value_or(0); i.dumpFailed = parts[9] == L"1"; } // 0.23.1 and later
    return i;
}
void attachContext(Incident& incident, const std::vector<Record>& records) {
    const Record* lastBefore = nullptr;
    const Record* heartbeat = nullptr;
    for (const auto& r : records) {
        if (r.time > incident.time) break;
        lastBefore = &r;
        if (startsWith(r.text, L"hb ")) heartbeat = &r;
    }
    if (!lastBefore) return;
    // Only when the black box ran up to the incident (within 30 minutes; a crash is reported at the next boot).
    if (incident.time - lastBefore->time > 30 * 60 * Second && !shutdownRank(incident.kind)) return;
    if (shutdownRank(incident.kind) && incident.time - lastBefore->time > 6 * 3600 * Second) return;
    if (heartbeat && incident.context.empty())
        incident.context = localTimeText(heartbeat->time, true) + L" " + heartbeat->text.substr(3);
    if (incident.recentWrites.empty()) {
        std::wstring writes;
        for (const auto& r : records) {
            if (r.time > lastBefore->time) break;
            if (r.time + 60 * Second < lastBefore->time || !startsWith(r.text, L"ev ")) continue;
            const std::wstring t = r.text.substr(3);
            if (!startsWith(t, L"acpi") && !startsWith(t, L"knob") && !startsWith(t, L"asus") && !startsWith(t, L"tuning")) continue;
            const double before = double(lastBefore->time - r.time) / Second;
            wchar_t ago[24]; swprintf(ago, 24, L"-%.0fs ", before);
            if (!writes.empty()) writes += L" | ";
            writes += ago + t;
            if (writes.size() > 400) break;
        }
        incident.recentWrites = writes;
    }
}

// ---- Freezes.
std::optional<double> StallMeter::feed(uint64_t gapMs, uint64_t intervalMs, bool screenOn, uint64_t msSinceScreenOrPowerChange) {
    if (!screenOn || msSinceScreenOrPowerChange < 30000 || gapMs <= intervalMs) return std::nullopt;
    const uint64_t stall = gapMs - intervalMs;
    if (stall < ThresholdMs) return std::nullopt;
    return double(stall) / 1000.0;
}

// ---- Files.
std::wstring folder() {
    PWSTR base = nullptr;
    std::wstring out;
    constexpr GUID LocalAppData = {0xF1B32785, 0x6FBA, 0x4FCF, {0x9D, 0x55, 0x7B, 0x8E, 0x7F, 0x15, 0x70, 0x91}};
    if (SUCCEEDED(SHGetKnownFolderPath(LocalAppData, KF_FLAG_CREATE, nullptr, &base)) && base)
        out = (std::filesystem::path(base) / L"BatteryScope" / L"stability").wstring();
    CoTaskMemFree(base);
    std::error_code error;
    if (!out.empty()) std::filesystem::create_directories(out, error);
    return out;
}
std::vector<Incident> loadIncidents() {
    std::vector<Incident> out;
    const std::wstring dir = folder();
    if (dir.empty()) return out;
    std::ifstream in(std::filesystem::path(dir) / L"incidents.tsv", std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (auto i = loadLine(fromUtf8(line))) out.push_back(std::move(*i));
    }
    return out;
}
void saveIncidents(const std::vector<Incident>& incidents) {
    const std::wstring dir = folder();
    if (dir.empty()) return;
    const auto path = std::filesystem::path(dir) / L"incidents.tsv";
    const auto temp = std::filesystem::path(dir) / L"incidents.tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        for (const auto& i : incidents) out << toUtf8(saveLine(i)) << "\n";
    }
    MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

namespace {
using MiniDumpWriteDumpFn = BOOL (WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
MiniDumpWriteDumpFn miniDumpWriteDump = nullptr;
wchar_t crashFolder[MAX_PATH]{};
LPTOP_LEVEL_EXCEPTION_FILTER previousFilter = nullptr;
void loadDbgHelp() {
    if (miniDumpWriteDump) return;
    if (HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        FARPROC symbol = GetProcAddress(dbghelp, "MiniDumpWriteDump");
        std::memcpy(&miniDumpWriteDump, &symbol, sizeof(miniDumpWriteDump));
    }
}
bool dumpTo(const wchar_t* path, EXCEPTION_POINTERS* exception, DWORD threadId) {
    if (!miniDumpWriteDump) return false;
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    MINIDUMP_EXCEPTION_INFORMATION info{};
    info.ThreadId = threadId ? threadId : GetCurrentThreadId(); info.ExceptionPointers = exception; info.ClientPointers = FALSE;
    const BOOL ok = miniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
        MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory), exception ? &info : nullptr, nullptr, nullptr);
    CloseHandle(file);
    return ok != FALSE;
}
// Written without locks or allocation where possible: the process is in an unknown state.
void writeCrashNote(const char* what, DWORD code, const void* address) {
    if (!crashFolder[0]) return;
    SYSTEMTIME t{}; GetLocalTime(&t);
    wchar_t base[MAX_PATH + 64];
    swprintf(base, MAX_PATH + 64, L"%ls\\crash-%04u%02u%02u-%02u%02u%02u", crashFolder, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    wchar_t textPath[MAX_PATH + 80]; swprintf(textPath, MAX_PATH + 80, L"%ls.txt", base);
    HMODULE module = nullptr; wchar_t moduleName[MAX_PATH] = L"?";
    if (address && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(address), &module))
        GetModuleFileNameW(module, moduleName, MAX_PATH);
    char line[1024];
    const int n = std::snprintf(line, sizeof(line), "BatteryScope %s crash: %s code=0x%08lX address=%p module=%ls+0x%llX thread=%lu\r\n",
        BS_VERSION_STRING, what, static_cast<unsigned long>(code), address, moduleName,
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module)), static_cast<unsigned long>(GetCurrentThreadId()));
    HANDLE file = CreateFileW(textPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file != INVALID_HANDLE_VALUE) { DWORD written = 0; WriteFile(file, line, DWORD(std::max(n, 0)), &written, nullptr); CloseHandle(file); }
}
LONG WINAPI crashFilter(EXCEPTION_POINTERS* exception) {
    static volatile LONG once = 0;
    if (InterlockedExchange(&once, 1) == 0 && crashFolder[0]) {
        const DWORD code = exception && exception->ExceptionRecord ? exception->ExceptionRecord->ExceptionCode : 0;
        const void* address = exception && exception->ExceptionRecord ? exception->ExceptionRecord->ExceptionAddress : nullptr;
        writeCrashNote("exception", code, address);
        SYSTEMTIME t{}; GetLocalTime(&t);
        wchar_t dump[MAX_PATH + 64];
        swprintf(dump, MAX_PATH + 64, L"%ls\\crash-%04u%02u%02u-%02u%02u%02u.dmp", crashFolder, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        dumpTo(dump, exception, 0);
    }
    return previousFilter ? previousFilter(exception) : EXCEPTION_CONTINUE_SEARCH;
}
void terminateHandler() {
    writeCrashNote("std::terminate", 0, nullptr);
    std::abort();
}
} // namespace
void installCrashHandler() {
    const std::wstring dir = folder();
    if (dir.empty() || dir.size() >= MAX_PATH) return;
    wcscpy_s(crashFolder, dir.c_str());
    loadDbgHelp();
    previousFilter = SetUnhandledExceptionFilter(crashFilter);
    std::set_terminate(terminateHandler);
}
bool writeOwnDump(const std::wstring& path, DWORD threadId) {
    loadDbgHelp();
    return dumpTo(path.c_str(), nullptr, threadId);
}
std::vector<std::pair<uint64_t, std::wstring>> ownCrashes(uint64_t sinceUtc) {
    std::vector<std::pair<uint64_t, std::wstring>> out;
    const std::wstring dir = folder();
    if (dir.empty()) return out;
    WIN32_FIND_DATAW found{};
    HANDLE find = FindFirstFileW((dir + L"\\crash-*.txt").c_str(), &found);
    if (find == INVALID_HANDLE_VALUE) return out;
    do {
        const uint64_t time = fileTimeValue(found.ftLastWriteTime);
        if (time <= sinceUtc) continue;
        std::ifstream in(std::filesystem::path(dir) / found.cFileName, std::ios::binary);
        std::string line; std::getline(in, line);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        out.emplace_back(time, fromUtf8(line));
    } while (FindNextFileW(find, &found));
    FindClose(find);
    std::sort(out.begin(), out.end());
    return out;
}

namespace {
struct DumpFile { std::wstring path; uint64_t time = 0, size = 0; };
std::vector<DumpFile> findDumps(bool& denied) {
    std::vector<DumpFile> out;
    wchar_t windows[MAX_PATH]{}; GetWindowsDirectoryW(windows, MAX_PATH);
    const std::wstring roots[] = {std::wstring(windows) + L"\\Minidump", std::wstring(windows) + L"\\LiveKernelReports"};
    for (const auto& root : roots) {
        std::error_code error;
        if (!std::filesystem::exists(root, error)) continue;
        std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, error);
        if (error) { if (error.value() == ERROR_ACCESS_DENIED || error == std::errc::permission_denied) denied = true; continue; }
        for (; it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
            if (error) { denied = true; break; }
            const auto& entry = *it;
            if (!entry.is_regular_file(error) || entry.path().extension() != L".dmp") continue;
            WIN32_FILE_ATTRIBUTE_DATA a{};
            if (!GetFileAttributesExW(entry.path().c_str(), GetFileExInfoStandard, &a)) continue;
            out.push_back({entry.path().wstring(), fileTimeValue(a.ftLastWriteTime), (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow});
        }
    }
    std::sort(out.begin(), out.end(), [](const DumpFile& a, const DumpFile& b) { return a.time > b.time; });
    return out;
}
} // namespace
std::wstring listSystemDumps(bool& accessDenied, int& count) {
    accessDenied = false;
    const auto dumps = findDumps(accessDenied);
    count = int(dumps.size());
    std::wstring text;
    for (const auto& d : dumps) {
        wchar_t line[64]; swprintf(line, 64, L"  %.1f MB  ", double(d.size) / (1024.0 * 1024.0));
        text += localTimeText(d.time) + line + d.path + L"\r\n";
    }
    return text;
}
int copySystemDumps(const std::wstring& target, std::wstring& listing) {
    bool denied = false;
    const auto dumps = findDumps(denied);
    std::error_code error;
    std::filesystem::create_directories(target, error);
    int copied = 0, minidumps = 0, liveDumps = 0;
    const uint64_t cutoff = nowUtc() - 30ull * 86400 * Second;
    for (const auto& d : dumps) {
        wchar_t line[64]; swprintf(line, 64, L"  %.1f MB  ", double(d.size) / (1024.0 * 1024.0));
        listing += localTimeText(d.time) + line + d.path;
        const bool isLive = d.path.find(L"LiveKernelReports") != std::wstring::npos;
        // Small dumps: the latest five; live kernel dumps: the latest two under 64 MB.
        const bool take = d.time >= cutoff && (isLive ? liveDumps < 2 && d.size <= 64ull << 20 : minidumps < 5 && d.size <= 128ull << 20);
        if (take) {
            const std::wstring name = (isLive ? L"live-" : L"") + std::filesystem::path(d.path).filename().wstring();
            if (CopyFileW(d.path.c_str(), (std::filesystem::path(target) / name).c_str(), FALSE)) { ++copied; ++(isLive ? liveDumps : minidumps); listing += L"  (copied)"; }
        }
        listing += L"\r\n";
    }
    if (denied) listing += L"(some folders could not be read)\r\n";
    return copied;
}

// ---- Write guard.
void WriteGuard::hold(Hold reason) { holds_ |= unsigned(reason); }
void WriteGuard::release(Hold reason, uint64_t nowMs, uint64_t settleMs) {
    holds_ &= ~unsigned(reason);
    settle(nowMs, settleMs);
}
void WriteGuard::settle(uint64_t nowMs, uint64_t ms) {
    uint64_t until = nowMs + ms, current = settleUntil_.load();
    while (until > current && !settleUntil_.compare_exchange_weak(current, until)) {}
}
bool WriteGuard::held() const { return holds_.load() != 0; }
bool WriteGuard::automaticBlocked(uint64_t nowMs) const { return held() || nowMs < settleUntil_.load(); }
unsigned WriteGuard::holds() const { return holds_.load(); }
uint64_t WriteGuard::blockedWrites() const { return blocked_.load(); }
void WriteGuard::countBlocked() { ++blocked_; }
WriteGuard& writeGuard() { static WriteGuard guard; return guard; }
bool refuseWrite(const std::wstring& what) {
    if (!writeGuard().held()) return false;
    writeGuard().countBlocked();
    note(L"blocked " + what);
    return true;
}

// ---- G-Helper.
GHelperSettings parseGHelperConfig(const std::string& json) {
    GHelperSettings g;
    // A flat object of "key": number pairs; anything else is skipped.
    size_t at = 0;
    while ((at = json.find('"', at)) != std::string::npos) {
        const size_t end = json.find('"', at + 1);
        if (end == std::string::npos) break;
        const std::string key = json.substr(at + 1, end - at - 1);
        size_t colon = end + 1;
        while (colon < json.size() && (json[colon] == ' ' || json[colon] == '\t' || json[colon] == '\r' || json[colon] == '\n')) ++colon;
        if (colon >= json.size() || json[colon] != ':') { at = end + 1; continue; }
        ++colon;
        while (colon < json.size() && (json[colon] == ' ' || json[colon] == '\t')) ++colon;
        char* stop = nullptr;
        const long value = std::strtol(json.c_str() + colon, &stop, 10);
        const bool number = stop != json.c_str() + colon;
        at = number ? size_t(stop - json.c_str()) : colon;
        if (!number) { if (colon < json.size() && json[colon] == '"') { const size_t close = json.find('"', colon + 1); if (close == std::string::npos) break; at = close + 1; } continue; }
        g.found = true;
        if (key == "auto_uv") g.autoUndervolt = value != 0;
        else if (key.rfind("cpu_uv", 0) == 0) g.cpuUndervolt = std::min(g.cpuUndervolt, int(value));
        else if (key.rfind("igpu_uv", 0) == 0) g.igpuUndervolt = std::min(g.igpuUndervolt, int(value));
        else if (key.rfind("gpu_core", 0) == 0) g.gpuCore = std::max(g.gpuCore, int(value));
        else if (key.rfind("gpu_memory", 0) == 0) g.gpuMemory = std::max(g.gpuMemory, int(value));
    }
    return g;
}
GHelperSettings readGHelperConfig() {
    PWSTR roaming = nullptr;
    constexpr GUID RoamingAppData = {0x3EB685DB, 0x65F9, 0x4CF6, {0xA0, 0x3A, 0xE3, 0xEF, 0x65, 0x72, 0x9F, 0x3D}};
    std::filesystem::path path;
    if (SUCCEEDED(SHGetKnownFolderPath(RoamingAppData, 0, nullptr, &roaming)) && roaming) path = std::filesystem::path(roaming) / L"GHelper" / L"config.json";
    CoTaskMemFree(roaming);
    if (path.empty()) return {};
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.size() > (1u << 20)) return {};
    return parseGHelperConfig(text);
}

// ---- Timeline.
bool tookSystemDown(Kind kind) { return kind == Kind::blueScreen || kind == Kind::powerLoss || kind == Kind::forcedOff; }
bool sameBoot(uint64_t recordedBoot, uint64_t currentBoot) {
    if (!recordedBoot || !currentBoot) return false;
    const uint64_t gap = recordedBoot > currentBoot ? recordedBoot - currentBoot : currentBoot - recordedBoot;
    return gap < 5 * 60 * Second;
}
int severity(Kind kind) {
    if (tookSystemDown(kind)) return 3;
    if (kind == Kind::freeze || kind == Kind::graphicsReset || kind == Kind::hardwareError) return 2;
    if (kind == Kind::manual || kind == Kind::count) return 0;
    return 1;
}
std::vector<DayMark> timeline(const std::vector<Incident>& incidents, uint64_t now, int days, int64_t utcOffsetSeconds) {
    std::vector<DayMark> out(size_t(std::max(days, 1)));
    auto day = [&](uint64_t utc) { return (int64_t(utc / Second) + utcOffsetSeconds) / 86400; };
    const int64_t today = day(now);
    for (const auto& i : incidents) {
        const int64_t age = today - day(i.when());
        if (age < 0 || age >= int64_t(out.size())) continue;
        auto& mark = out[out.size() - 1 - size_t(age)];
        const int s = severity(i.kind);
        if (!s) continue;
        mark.severity = std::max(mark.severity, s);
        if (s >= 2) ++mark.count; else if (!mark.count) mark.count = 1;
    }
    return out;
}

} // namespace bs::stability
