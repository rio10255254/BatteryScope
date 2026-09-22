#include "../src/platform.h"
#include "../src/energy.h"
#include <atomic>
#include <thread>
#include <iostream>
#include <iomanip>

int main() {
    using namespace bs;
    EnergyReader reader; Collector collector; CpuAttributor estimator;
    std::atomic<bool> stop{false};
    std::atomic<unsigned> phase{0};
    std::thread load([&] {
        volatile uint64_t work=1;
        unsigned old=0;
        while(!stop) {
            unsigned mode=phase.load();
            if(!mode) { Sleep(20); continue; }
            if(mode!=old) { SetThreadAffinityMask(GetCurrentThread(),KAFFINITY(1)<<(mode==1?0:12)); old=mode; }
            for(unsigned i=0;i<100000 && !stop;++i) work=work*6364136223846793005ULL+1442695040888963407ULL;
        }
    });
    std::cout<<"phase,coreW,packageW,ownCpuW,ownAverageW,ownAverageLowW,ownAverageHighW,placementCoverage,unassignedW,ownCpuPercent,core0W,core6W,ready\n";
    for(unsigned i=0;i<15;++i) {
        if(i==3) phase=1;
        if(i==7) phase=2;
        if(i==11) phase=0;
        size_t total=0,inaccessible=0;
        auto rows=collector.collect(total,inaccessible); reader.sampleThreadPlacement(rows); auto energy=reader.sample();
        estimator.apply(rows,energy,GetTickCount64(),GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
        ProcessRow ours;
        for(const auto& row:rows) if(row.pid==GetCurrentProcessId()) ours=row;
        auto v=[](Value x) { return x?*x:-1.0; };
        std::cout<<phase<<','<<v(energy.coreWatts)<<','<<v(energy.packageWatts)<<','<<v(ours.cpuWatts)<<','<<v(ours.averageCpuWatts)<<','
            <<v(ours.cpuWattsLow)<<','<<v(ours.cpuWattsHigh)<<','<<ours.placementCoverage<<','<<v(energy.unassignedCpuWatts)<<','<<ours.cpu<<','
            <<(energy.cores.size()>0?energy.cores[0].watts:-1)<<','<<(energy.cores.size()>6?energy.cores[6].watts:-1)<<','<<energy.ready<<'\n';
        std::cout.flush();
        if(i<14) Sleep(2000);
    }
    stop=true; load.join();
}
