#define UNICODE
#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
#include <vector>
#include <string>
#include <iostream>
#include <cstring>
#include <cstdint>

constexpr GUID meterGuid{0x45bd8344,0x7ed6,0x49cf,{0xa4,0x40,0xc2,0x76,0xc9,0x33,0xb0,0x53}};
constexpr DWORD VersionIo = CTL_CODE(FILE_DEVICE_UNKNOWN,0,METHOD_BUFFERED,FILE_READ_ACCESS);
constexpr DWORD SizeIo = CTL_CODE(FILE_DEVICE_UNKNOWN,1,METHOD_BUFFERED,FILE_READ_ACCESS);
constexpr DWORD MetadataIo = CTL_CODE(FILE_DEVICE_UNKNOWN,2,METHOD_BUFFERED,FILE_READ_ACCESS);
constexpr DWORD MeasurementIo = CTL_CODE(FILE_DEVICE_UNKNOWN,3,METHOD_BUFFERED,FILE_READ_ACCESS);
struct Reading { uint64_t energy, time; };
struct Meter { HANDLE handle; std::vector<std::wstring> names; std::vector<Reading> previous; };
template<class T> T at(const std::vector<BYTE>& b, size_t offset) { T value{}; if(offset + sizeof(T) <= b.size()) std::memcpy(&value,b.data()+offset,sizeof(T)); return value; }
std::wstring nameAt(const std::vector<BYTE>& b, size_t offset, size_t size) {
    if(offset + size > b.size() || size % 2) return L"INVALID";
    std::wstring s(size/2,0); std::memcpy(s.data(),b.data()+offset,size);
    auto zero=s.find(L'\0'); if(zero!=s.npos) s.resize(zero); return s;
}
int main() {
    HDEVINFO devices=SetupDiGetClassDevsW(&meterGuid,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
    if(devices==INVALID_HANDLE_VALUE) { std::cout<<"enumeration_error="<<GetLastError()<<'\n'; return 1; }
    std::vector<Meter> meters;
    for(DWORD i=0;;++i) {
        SP_DEVICE_INTERFACE_DATA iface{}; iface.cbSize=sizeof(iface);
        if(!SetupDiEnumDeviceInterfaces(devices,nullptr,&meterGuid,i,&iface)) break;
        DWORD n=0; SetupDiGetDeviceInterfaceDetailW(devices,&iface,nullptr,0,&n,nullptr);
        std::vector<BYTE> bytes(n); auto detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(bytes.data()); detail->cbSize=sizeof(*detail);
        if(!SetupDiGetDeviceInterfaceDetailW(devices,&iface,detail,n,nullptr,nullptr)) continue;
        HANDLE handle=CreateFileW(detail->DevicePath,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
        if(handle==INVALID_HANDLE_VALUE) { std::cout<<"device="<<i<<" open_error="<<GetLastError()<<'\n'; continue; }
        USHORT version=0; DWORD returned=0,size=0;
        if(!DeviceIoControl(handle,VersionIo,nullptr,0,&version,sizeof(version),&returned,nullptr)) { std::cout<<"version_error="<<GetLastError()<<'\n'; CloseHandle(handle); continue; }
        if(!DeviceIoControl(handle,SizeIo,nullptr,0,&size,sizeof(size),&returned,nullptr)||size>1048576) { CloseHandle(handle); continue; }
        std::vector<BYTE> metadata(size);
        if(!DeviceIoControl(handle,MetadataIo,nullptr,0,metadata.data(),size,&returned,nullptr)) { CloseHandle(handle); continue; }
        std::cout<<"device="<<i<<" version="<<version<<" metadata_bytes="<<size<<'\n';
        Meter meter{handle,{},{}};
        if(version==1) {
            std::wcout<<L" unit="<<at<ULONG>(metadata,0)<<L" rail="<<nameAt(metadata,72,at<USHORT>(metadata,70))<<'\n';
            meter.names.push_back(nameAt(metadata,72,at<USHORT>(metadata,70)));
        } else if(version==2) {
            auto count=at<USHORT>(metadata,66); size_t pos=68;
            for(unsigned j=0;j<count && pos+6<=size;++j) {
                auto bytes=at<USHORT>(metadata,pos+4); auto name=nameAt(metadata,pos+6,bytes);
                std::wcout<<L" unit="<<at<ULONG>(metadata,pos)<<L" rail="<<name<<'\n';
                meter.names.push_back(name); pos+=6+bytes;
            }
        }
        meter.previous.resize(meter.names.size());
        if(!meter.previous.empty() && DeviceIoControl(handle,MeasurementIo,nullptr,0,meter.previous.data(),DWORD(meter.previous.size()*sizeof(Reading)),&returned,nullptr)) meters.push_back(std::move(meter));
        else { std::cout<<"measurement_error="<<GetLastError()<<'\n'; CloseHandle(handle); }
    }
    SetupDiDestroyDeviceInfoList(devices);
    Sleep(2000);
    for(auto& meter:meters) {
        std::vector<Reading> now(meter.names.size()); DWORD returned=0;
        if(DeviceIoControl(meter.handle,MeasurementIo,nullptr,0,now.data(),DWORD(now.size()*sizeof(Reading)),&returned,nullptr)) {
            for(size_t i=0;i<now.size();++i) {
                double dt=(now[i].time-meter.previous[i].time)/1e7;
                double watts=now[i].energy>=meter.previous[i].energy && dt>0 ? (now[i].energy-meter.previous[i].energy)*3.6e-9/dt : -1;
                std::wcout<<meter.names[i]<<L" watts="<<watts<<L" interval="<<dt<<L" energy="<<now[i].energy<<'\n';
            }
        } else std::cout<<"second_read_error="<<GetLastError()<<'\n';
        CloseHandle(meter.handle);
    }
    std::cout<<"readable_devices="<<meters.size()<<'\n';
}
