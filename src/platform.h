#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "model.h"
#include <map>

namespace bs {
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE h = nullptr) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    operator HANDLE() const { return value; }
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};
uint64_t fileTime(FILETIME t);
std::wstring errorText(DWORD code);
std::wstring executablePath();
PowerSample readPower();
std::vector<ServiceInfo> readServices(bool& ok);
bool safeProcess(HANDLE process, DWORD pid, std::wstring* reason = nullptr);
DWORD closeProcess(const ProcessRow& expected, bool force);
DWORD stopService(const std::wstring& name, DWORD expectedPid, uint64_t expectedCreated);
bool setSamplerEcoQoS(bool enabled);
struct SaverMode {
    DWORD error = ERROR_SUCCESS;
    bool efficient = false;
    bool restorable = false;
};
SaverMode querySaverMode();
DWORD toggleSaverMode();

struct CollectionWork { size_t paths = 0, safety = 0, memory = 0, processOpens = 0; };

class Collector {
    struct Previous {
        uint64_t created = 0, cpu = 0, io = 0, ms = 0;
        double averageCpu = 0, averageIo = 0, busySeconds = 0;
        bool ioKnown = false;
    };
    std::map<DWORD, Previous> previous_;
    struct Metadata {
        uint64_t created = 0, pathAt = 0, safetyAt = 0, memoryAt = 0;
        std::wstring path;
        bool safe = false, memoryKnown = false, readable = false;
        double memoryMB = 0;
    };
    std::map<DWORD, Metadata> metadata_;
    bool cacheEnabled_;
    bool forceToolhelp_;
    CollectionWork work_;
    std::vector<ServiceInfo> services_;
    bool servicesKnown_ = false;
    uint64_t servicesAt_ = 0;
    uint64_t sampleMs_ = 0, previousSampleMs_ = 0;
    double intervalSeconds_ = 0, snapshotDurationMs_ = 0;
    bool systemSnapshot_ = false;
public:
    explicit Collector(bool cacheEnabled = true, bool forceToolhelp = false)
        : cacheEnabled_(cacheEnabled), forceToolhelp_(forceToolhelp) {}
    std::vector<ProcessRow> collect(size_t& total, size_t& inaccessible, bool background = false);
    const CollectionWork& work() const { return work_; }
    uint64_t sampleMs() const { return sampleMs_; }
    double intervalSeconds() const { return intervalSeconds_; }
    double snapshotDurationMs() const { return snapshotDurationMs_; }
    bool systemSnapshot() const { return systemSnapshot_; }
    bool servicesKnown() const { return servicesKnown_; }
    void reset() { previous_.clear(); metadata_.clear(); servicesAt_ = sampleMs_ = previousSampleMs_ = 0; intervalSeconds_ = snapshotDurationMs_ = 0; systemSnapshot_ = false; }
};
}
