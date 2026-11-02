#include "energy.h"
#include "platform.h"
#include <setupapi.h>
#include <tlhelp32.h>
#include <cstring>
#include <set>

namespace bs {
namespace {
constexpr GUID MeterGuid{0x45bd8344,0x7ed6,0x49cf,{0xa4,0x40,0xc2,0x76,0xc9,0x33,0xb0,0x53}};
constexpr DWORD VersionIo = CTL_CODE(FILE_DEVICE_UNKNOWN,0,METHOD_BUFFERED,FILE_READ_ACCESS);
constexpr DWORD SizeIo = CTL_CODE(FILE_DEVICE_UNKNOWN,1,METHOD_BUFFERED,FILE_READ_ACCESS);
constexpr DWORD MetadataIo = CTL_CODE(FILE_DEVICE_UNKNOWN,2,METHOD_BUFFERED,FILE_READ_ACCESS);
constexpr DWORD MeasurementIo = CTL_CODE(FILE_DEVICE_UNKNOWN,3,METHOD_BUFFERED,FILE_READ_ACCESS);
struct Reading { uint64_t energy = 0, time = 0; };
struct ProcessorTimes { LARGE_INTEGER idle, kernel, user, dpc, interrupt; ULONG interrupts; };
static_assert(sizeof(ProcessorTimes) == 48);
template<class T> T at(const std::vector<BYTE>& bytes, size_t offset) {
    T value{}; if (offset + sizeof(T) <= bytes.size()) std::memcpy(&value, bytes.data()+offset, sizeof(T)); return value;
}
std::wstring nameAt(const std::vector<BYTE>& bytes, size_t offset, size_t count) {
    if (!count || count % 2 || offset + count > bytes.size()) return {};
    std::wstring name(count / 2, 0); std::memcpy(name.data(), bytes.data()+offset, count);
    auto end = name.find(L'\0'); if (end != name.npos) name.resize(end); return name;
}
bool readProcessorTimes(std::vector<ProcessorTimes>& times) {
    using Query = LONG (WINAPI*)(ULONG,PVOID,ULONG,PULONG);
    static Query query = [] {
        FARPROC symbol = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation");
        Query function = nullptr; static_assert(sizeof(function)==sizeof(symbol));
        std::memcpy(&function,&symbol,sizeof(function)); return function;
    }();
    if (!query || GetActiveProcessorGroupCount() != 1) return false;
    times.resize(GetActiveProcessorCount(0)); ULONG returned = 0;
    return query(8 /* SystemProcessorPerformanceInformation */, times.data(), ULONG(times.size()*sizeof(ProcessorTimes)), &returned) >= 0
        && returned == times.size()*sizeof(ProcessorTimes);
}
std::vector<std::vector<unsigned>> physicalCores() {
    std::vector<std::vector<unsigned>> result; DWORD needed = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore,nullptr,&needed);
    std::vector<BYTE> data(needed);
    if (!needed || !GetLogicalProcessorInformationEx(RelationProcessorCore,reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(data.data()),&needed)) return result;
    for (size_t offset=0; offset+sizeof(ULONG)*2<=needed;) {
        auto row=reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(data.data()+offset);
        if (!row->Size || offset+row->Size>needed) return {};
        if (row->Relationship==RelationProcessorCore) {
            std::vector<unsigned> members;
            for (unsigned g=0;g<row->Processor.GroupCount;++g) {
                if (row->Processor.GroupMask[g].Group!=0) return {};
                auto mask=row->Processor.GroupMask[g].Mask;
                for (unsigned bit=0;bit<sizeof(KAFFINITY)*8;++bit) if (mask & (KAFFINITY(1)<<bit)) members.push_back(bit);
            }
            if (!members.empty()) result.push_back(members);
        }
        offset += row->Size;
    }
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b) { return a.front()<b.front(); });
    return result;
}
}
struct EnergyReader::Impl {
    struct Device {
        HANDLE handle = INVALID_HANDLE_VALUE;
        std::vector<std::wstring> names;
        std::vector<ULONG> units;
        std::vector<Reading> before;
    };
    std::vector<Device> devices;
    std::vector<ProcessorTimes> previousCpu;
    std::vector<std::vector<unsigned>> topology;
    uint64_t previousMs = 0;
    size_t openFailures = 0;
    bool baseline = false;
    struct ThreadReading { uint64_t created=0, cpu=0, ms=0; DWORD pid=0; int core=-1; };
    std::map<DWORD,ThreadReading> previousThreads;
    Impl() { discover(); }
    ~Impl() { for (auto& device:devices) if(device.handle!=INVALID_HANDLE_VALUE) CloseHandle(device.handle); }
    void discover() {
        topology = physicalCores();
        HDEVINFO set=SetupDiGetClassDevsW(&MeterGuid,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
        if(set==INVALID_HANDLE_VALUE) return;
        for(DWORD index=0;;++index) {
            SP_DEVICE_INTERFACE_DATA iface{}; iface.cbSize=sizeof(iface);
            if(!SetupDiEnumDeviceInterfaces(set,nullptr,&MeterGuid,index,&iface)) break;
            DWORD needed=0; SetupDiGetDeviceInterfaceDetailW(set,&iface,nullptr,0,&needed,nullptr);
            if (!needed || needed>65536) continue;
            std::vector<BYTE> data(needed);
            auto detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(data.data()); detail->cbSize=sizeof(*detail);
            if(!SetupDiGetDeviceInterfaceDetailW(set,&iface,detail,needed,nullptr,nullptr)) continue;
            Device device;
            device.handle=CreateFileW(detail->DevicePath,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(device.handle==INVALID_HANDLE_VALUE) { ++openFailures; continue; }
            USHORT version=0; DWORD size=0,returned=0;
            bool valid=DeviceIoControl(device.handle,VersionIo,nullptr,0,&version,sizeof(version),&returned,nullptr)
                && (version==1 || version==2) && DeviceIoControl(device.handle,SizeIo,nullptr,0,&size,sizeof(size),&returned,nullptr)
                && size>=68 && size<=65536;
            std::vector<BYTE> metadata(valid?size:0);
            valid=valid && DeviceIoControl(device.handle,MetadataIo,nullptr,0,metadata.data(),size,&returned,nullptr) && returned==size;
            if(valid && version==1 && size>=74) {
                auto name=nameAt(metadata,72,at<USHORT>(metadata,70));
                if(!name.empty()) { device.names.push_back(name); device.units.push_back(at<ULONG>(metadata,0)); }
            } else if(valid && version==2) {
                unsigned count=at<USHORT>(metadata,66); size_t offset=68;
                if(count>64) valid=false;
                for(unsigned channel=0;valid && channel<count;++channel) {
                    if(offset+6>size) { valid=false; break; }
                    auto nameBytes=at<USHORT>(metadata,offset+4);
                    auto name=nameAt(metadata,offset+6,nameBytes);
                    if(name.empty()) { valid=false; break; }
                    device.units.push_back(at<ULONG>(metadata,offset)); device.names.push_back(name); offset+=6+nameBytes;
                }
            }
            if(valid && !device.names.empty()) { device.before.resize(device.names.size()); devices.push_back(std::move(device)); }
            else CloseHandle(device.handle);
        }
        SetupDiDestroyDeviceInfoList(set);
    }
};
EnergyReader::EnergyReader() : impl_(std::make_unique<Impl>()) {}
EnergyReader::~EnergyReader() = default;
void EnergyReader::reset() { impl_->baseline=false; impl_->previousCpu.clear(); impl_->previousMs=0; impl_->previousThreads.clear(); }
void EnergyReader::clearThreadPlacement() { impl_->previousThreads.clear(); }
void EnergyReader::sampleThreadPlacement(std::vector<ProcessRow>& rows, const ThreadSnapshot* snapshot) {
    auto& state=*impl_;
    if(state.topology.empty() || GetActiveProcessorGroupCount()!=1) return;
    std::vector<ProcessRow*> candidates;
    for(auto& row:rows) {
        row.fixedCoreBusy.assign(state.topology.size(),0);
        if(row.measured && row.cpu>=.1) candidates.push_back(&row);
    }
    std::sort(candidates.begin(),candidates.end(),[](auto a,auto b) { return a->cpu>b->cpu; });
    if(candidates.size()>12) candidates.resize(12);
    if(candidates.empty()) { state.previousThreads.clear(); return; }
    std::map<DWORD,ProcessRow*> selected;
    for(auto row:candidates) selected[row->pid]=row;
    std::vector<KAFFINITY> masks;
    for(const auto& core:state.topology) {
        KAFFINITY mask=0; for(auto logical:core) mask|=KAFFINITY(1)<<logical; masks.push_back(mask);
    }
    // Returns the physical core a thread is confined to, -1 if unconfined, or -2 if unreadable.
    auto confinedCore=[&](DWORD tid) {
        Handle handle(OpenThread(THREAD_QUERY_LIMITED_INFORMATION,FALSE,tid));
        GROUP_AFFINITY affinity{};
        if(!handle || !GetThreadGroupAffinity(handle,&affinity) || affinity.Group!=0) return -2;
        for(size_t i=0;i<masks.size();++i) if(affinity.Mask && (affinity.Mask & ~masks[i])==0) return int(i);
        return -1;
    };
    auto contribute=[](ProcessRow& row,const Impl::ThreadReading& old,const Impl::ThreadReading& reading) {
        if(reading.core<0 || old.pid!=reading.pid || old.created!=reading.created || old.core!=reading.core
            || reading.cpu<old.cpu || reading.ms<=old.ms || reading.ms-old.ms>30000) return;
        row.fixedCoreBusy[size_t(reading.core)]+=double(reading.cpu-old.cpu)/1e7/((reading.ms-old.ms)/1000.0);
    };
    std::map<DWORD,Impl::ThreadReading> next;
    unsigned opened=0;
    if(snapshot) {
        // CPU times come from the collector's snapshot, so every thread shares the
        // process sample's timestamp. Only new threads, or threads that ran since
        // the previous sample, need a handle to read their affinity.
        for(const auto& thread:snapshot->threads) {
            auto row=selected.find(thread.pid);
            if(row==selected.end()) continue;
            Impl::ThreadReading reading{thread.created,thread.cpu,snapshot->sampleMs,thread.pid,-1};
            auto prior=state.previousThreads.find(thread.tid);
            const bool known=prior!=state.previousThreads.end() && prior->second.pid==reading.pid
                && prior->second.created==reading.created && reading.ms>prior->second.ms && reading.ms-prior->second.ms<=30000;
            if(known && reading.cpu==prior->second.cpu) { reading.core=prior->second.core; next[thread.tid]=reading; continue; }
            if(opened>=2048) continue;
            ++opened;
            reading.core=confinedCore(thread.tid);
            if(reading.core==-2) continue;
            if(known) contribute(*row->second,prior->second,reading);
            next[thread.tid]=reading;
        }
        state.previousThreads=std::move(next);
        return;
    }
    Handle toolhelp(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0));
    THREADENTRY32 thread{}; thread.dwSize=sizeof(thread);
    if(!toolhelp || !Thread32First(toolhelp,&thread)) { state.previousThreads.clear(); return; }
    do {
        auto row=selected.find(thread.th32OwnerProcessID);
        if(row==selected.end() || opened>=2048) continue;
        ++opened;
        Handle handle(OpenThread(THREAD_QUERY_INFORMATION,FALSE,thread.th32ThreadID));
        FILETIME created{},exit{},kernel{},user{}; GROUP_AFFINITY affinity{};
        if(!handle || !GetThreadTimes(handle,&created,&exit,&kernel,&user) || !GetThreadGroupAffinity(handle,&affinity) || affinity.Group!=0) continue;
        int core=-1;
        for(size_t i=0;i<masks.size();++i) if(affinity.Mask && (affinity.Mask & ~masks[i])==0) { core=int(i); break; }
        Impl::ThreadReading reading{fileTime(created),fileTime(kernel)+fileTime(user),GetTickCount64(),thread.th32OwnerProcessID,core};
        auto prior=state.previousThreads.find(thread.th32ThreadID);
        if(prior!=state.previousThreads.end()) contribute(*row->second,prior->second,reading);
        next[thread.th32ThreadID]=reading;
    } while(Thread32Next(toolhelp,&thread));
    state.previousThreads=std::move(next);
}
CpuEnergyReport EnergyReader::sample() {
    CpuEnergyReport report;
    auto& state=*impl_;
    if(state.devices.empty()) { report.status=state.openFailures?L"能源介面無法存取":L"硬體未提供可用的 EMI 能源介面"; return report; }
    const uint64_t sampleBegin=GetTickCount64();
    std::vector<ProcessorTimes> cpu;
    bool cpuOK=readProcessorTimes(cpu);
    std::map<unsigned,double> corePower;
    bool packageSeen=false, channelsOK=true;
    double packagePower=0, dtSum=0; size_t dtCount=0;
    std::set<std::wstring> seen;
    for(auto& device:state.devices) {
        std::vector<Reading> readings(device.names.size()); DWORD returned=0;
        if(!DeviceIoControl(device.handle,MeasurementIo,nullptr,0,readings.data(),DWORD(readings.size()*sizeof(Reading)),&returned,nullptr)
            || returned!=readings.size()*sizeof(Reading)) { channelsOK=false; continue; }
        for(size_t channel=0;channel<readings.size();++channel) {
            const auto& name=device.names[channel];
            if(device.units[channel]!=0 || !seen.insert(name).second) continue;
            ++report.channels;
            auto watts=state.baseline?energyDeltaWatts(device.before[channel].energy,readings[channel].energy,device.before[channel].time,readings[channel].time):Value{};
            if(!watts) { channelsOK=false; continue; }
            dtSum+=(readings[channel].time-device.before[channel].time)/1e7; ++dtCount;
            if(name==L"RAPL_Package0_PKG") { packagePower=*watts; packageSeen=true; }
            const std::wstring prefix=L"RAPL_Package0_Core", suffix=L"_CORE";
            if(name.compare(0,prefix.size(),prefix)==0 && name.size()>prefix.size()+suffix.size()
                && name.compare(name.size()-suffix.size(),suffix.size(),suffix)==0) {
                auto number=name.substr(prefix.size(),name.size()-prefix.size()-suffix.size());
                if(number.find_first_not_of(L"0123456789")==number.npos && number.size()<5) corePower[unsigned(std::stoul(number))]=*watts;
            }
        }
        device.before=std::move(readings);
    }
    const uint64_t sampleEnd=GetTickCount64();
    const uint64_t now=sampleBegin+(sampleEnd-sampleBegin)/2;
    report.sampleMs=now;
    if(packageSeen) report.packageWatts=packagePower;
    report.intervalSeconds=dtCount?dtSum/dtCount:0;
    bool timeOK=state.previousMs && now>state.previousMs && now-state.previousMs<=30000;
    if(timeOK && report.intervalSeconds>0) {
        double wall=(now-state.previousMs)/1000.0;
        timeOK=std::abs(report.intervalSeconds-wall)<=std::max(.1,wall*.1);
    }
    bool topologyOK=corePower.size()==state.topology.size() && !state.topology.empty() && cpu.size()==state.previousCpu.size();
    if(topologyOK && channelsOK && cpuOK && timeOK) {
        double sum=0;
        for(size_t core=0;core<state.topology.size();++core) {
            if(!corePower.count(unsigned(core))) { topologyOK=false; break; }
            double busy=0;
            for(auto logical:state.topology[core]) {
                if(logical>=cpu.size()) { topologyOK=false; break; }
                auto& a=state.previousCpu[logical]; auto& b=cpu[logical];
                LONGLONG total=(b.kernel.QuadPart-a.kernel.QuadPart)+(b.user.QuadPart-a.user.QuadPart);
                LONGLONG idle=b.idle.QuadPart-a.idle.QuadPart;
                if(total<=0 || idle<0 || idle>total) { topologyOK=false; break; }
                busy+=double(total-idle)/total;
            }
            double watts=corePower[unsigned(core)]; sum+=watts; report.cores.push_back({watts,busy});
        }
        if(topologyOK) {
            report.coreWatts=sum; report.ready=true;
            report.status=L"EMI 核心能源＋Windows CPU 時間";
        }
    }
    if(!report.ready) report.status=!state.baseline?L"硬體能源計數器暖機中":L"能源／CPU 拓樸資料不足，本次不估算瓦數";
    state.previousCpu=std::move(cpu); state.previousMs=now; state.baseline=cpuOK;
    return report;
}
}
