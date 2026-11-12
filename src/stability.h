#pragma once
// Stability monitor: finds out why the PC went down or froze, and keeps what
// was happening just before so it can be reported in one click.
//
// - A black box: a small ring file of fixed 512-byte slots, written through to
//   the disk every 10 seconds (and on every hardware write), so the last
//   minutes survive a blue screen or a hard reset.
// - The previous session's ending: clean exit, BatteryScope ended alone (same
//   boot), or the whole system went down (a later boot).
// - Windows' own records: blue screens (Kernel-Power 41, BugCheck 1001),
//   unexpected shutdowns (6008), graphics driver resets (Display 4101,
//   LiveKernelEvent), hardware errors (WHEA) and disk errors.
// - Live freezes: a high-priority thread notices when the whole system stops
//   scheduling it for seconds while the screen is on.
#include <windows.h>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace bs::stability {

// ---- Time: FILETIME ticks (100 ns since 1601, UTC) as one number.
uint64_t nowUtc();
uint64_t bootUtc(); // now minus the uptime
std::optional<uint64_t> parseIsoUtc(const std::wstring& text); // 2026-11-12T08:09:31.5000000Z
std::wstring localTimeText(uint64_t utc, bool seconds = false); // 2026-11-12 16:09
constexpr uint64_t Second = 10000000ull;

// ---- Blue screen codes.
enum class Area { unknown, storage, memory, graphics, power, processor, driver, system, freeze };
uint32_t normalizeBugcheck(uint32_t code); // 0x1000007E -> 0x7E
const wchar_t* bugcheckName(uint32_t code); // nullptr when unknown
Area bugcheckArea(uint32_t code);
const wchar_t* areaKey(Area area); // "storage", ...

// ---- Black box.
struct Record {
    uint64_t seq = 0, time = 0, boot = 0;
    DWORD pid = 0;
    std::wstring text; // "hb ac=0 pct=74 ..." or "ev ..." / "start" / "exit ..."
};
constexpr size_t SlotBytes = 512;
std::string encodeSlot(const Record& record);           // exactly SlotBytes
std::optional<Record> decodeSlot(const char* data, size_t size);
// key=value pairs of a heartbeat or event ("hb ac=0 pct=74" -> {ac:0, pct:74}).
std::map<std::wstring, std::wstring> fields(const std::wstring& text);

class BlackBox {
public:
    static constexpr size_t Slots = 720; // two hours of heartbeats
    bool open(const std::wstring& path);
    void close();
    bool isOpen() const;
    void write(const std::wstring& text);
    std::vector<Record> records() const;                          // oldest first
    static std::vector<Record> readFile(const std::wstring& path); // oldest first
private:
    mutable std::mutex mutex_;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    uint64_t seq_ = 0;
    uint64_t boot_ = 0;
    std::wstring path_;
};
// The process-wide black box; note() is a no-op until it is opened.
BlackBox& blackBox();
void note(const std::wstring& text);

// ---- How the previous session ended.
enum class Ending { none, clean, appEnded, appCrashed, appHung, systemDown, systemDownAsleep, batteryEmpty };
struct PreviousSession {
    Ending ending = Ending::none;
    uint64_t lastTime = 0;   // the last record written
    uint64_t lastBoot = 0;
    std::wstring lastHeartbeat;
    std::vector<Record> tail; // the records of its last five minutes
};
// records: the black box as it was found at startup (before this session wrote).
PreviousSession classify(const std::vector<Record>& records, uint64_t currentBoot);

// ---- Windows event log.
struct LogEvent {
    uint64_t time = 0;
    std::wstring provider, channel;
    uint32_t id = 0;
    int level = 0;
    std::vector<std::pair<std::wstring, std::wstring>> data; // in order; unnamed data has an empty name
    std::wstring message;
    std::wstring value(const std::wstring& name) const;
};
std::optional<LogEvent> parseEventXml(const std::wstring& xml);
enum class Channel { system, application, installs };
std::wstring eventQueryXml(uint64_t sinceUtc, Channel channel);
// 6008's "unexpected shutdown at" time, written in the PC's own date and time format: as UTC,
// the reading closest before the record itself.
std::optional<uint64_t> parseShutdownTime(const std::wstring& time, const std::wstring& date, uint64_t loggedUtc);
// Reads the events that matter since a time, newest first. withMessages formats each one's text.
// Each channel has its own limit, and one program's repeated crashes are kept to 20, so a
// crash loop of one app cannot push the system's records out.
std::vector<LogEvent> readEvents(uint64_t sinceUtc, bool withMessages, size_t limit = 2000);
// Driver and update installs (Windows Update 19, Windows Installer 1033/1034/1035), newest first.
std::vector<LogEvent> readInstalls(uint64_t sinceUtc);
// Third-party kernel drivers installed here that are known to cause blue screens: name and why.
std::vector<std::pair<std::wstring, std::wstring>> knownTroubleDrivers();
// A kernel minidump's stop code, parameters, and the drivers named in its crash data.
std::wstring summarizeKernelDump(const std::wstring& path);

// ---- Incidents.
enum class Kind {
    blueScreen,       // a stop code
    powerLoss,        // went down with no stop code (hard reset, power cut)
    forcedOff,        // the power button was held: usually after a freeze
    batteryEmpty,     // the battery ran out
    freeze,           // the whole system stopped for seconds (detected live)
    graphicsReset,    // display driver stopped responding and recovered
    hardwareError,    // WHEA
    diskError,
    appCrash,         // BatteryScope itself
    appHang,
    manual,           // marked by the user
    count
};
const wchar_t* kindKey(Kind kind);
std::optional<Kind> kindFromKey(const std::wstring& key);
bool serious(Kind kind); // worth a notification
struct Incident {
    uint64_t time = 0;
    Kind kind = Kind::manual;
    uint32_t code = 0;
    double seconds = 0;          // freezes
    std::wstring detail;         // provider, device or stop code parameters
    std::wstring context;        // the black box heartbeat before it
    std::wstring recentWrites;   // hardware writes in the minute before it
    bool sleeping = false;       // went down while asleep or entering sleep
    uint64_t wentDown = 0;       // when it went down (6008's last-alive time); time is when Windows logged it
    bool dumpFailed = false;     // Windows could not write the crash dump (volmgr 161/162/46)
    uint64_t when() const { return wentDown ? wentDown : time; }
    std::wstring key() const;    // stable identity for merging
};
// Groups the boot-time records of one crash (41, 1001, 6008) into one incident.
std::vector<Incident> incidentsFromEvents(const std::vector<LogEvent>& events);
// Adds incidents not already known (same kind within 3 minutes), newest first, at most 100.
std::vector<Incident> merge(std::vector<Incident> known, const std::vector<Incident>& found, std::vector<Incident>* added = nullptr);
std::wstring saveLine(const Incident& incident);
std::optional<Incident> loadLine(const std::wstring& line);

// Fills an incident's context from the black box: the last heartbeat before it, and hardware writes in the minute before.
void attachContext(Incident& incident, const std::vector<Record>& records);

// ---- Freeze detection, testable: feed it unbiased-time gaps.
class StallMeter {
public:
    // gapMs: unbiased time between two wakes that should be intervalMs apart.
    // Returns the stall in seconds when it is one.
    std::optional<double> feed(uint64_t gapMs, uint64_t intervalMs, bool screenOn, uint64_t msSinceScreenOrPowerChange);
    static constexpr uint64_t ThresholdMs = 2500;
};

// ---- Files.
std::wstring folder();                    // %LOCALAPPDATA%\BatteryScope\stability
std::vector<Incident> loadIncidents();
void saveIncidents(const std::vector<Incident>& incidents);
// This process's own crashes: installs the handler that writes crash-*.txt and a minidump.
void installCrashHandler();
bool writeOwnDump(const std::wstring& path, DWORD threadId = 0);
// crash-*.txt files written since a time (the previous session's crash).
std::vector<std::pair<uint64_t, std::wstring>> ownCrashes(uint64_t sinceUtc);

// Copies the latest blue screen dumps (Windows\Minidump and LiveKernelReports) into a folder.
// Needs administrator rights; returns how many were copied.
int copySystemDumps(const std::wstring& target, std::wstring& listing);
// Lists the dumps without copying (names, sizes, times), or why it could not.
std::wstring listSystemDumps(bool& accessDenied, int& count);

} // namespace bs::stability
