#include "../src/platform.h"
#include <iostream>
using namespace bs;
uint64_t threadCpu() {
    FILETIME c{}, e{}, k{}, u{};
    GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u);
    return fileTime(k) + fileTime(u);
}
int main() {
    bool eco = setSamplerEcoQoS(true), automatic = setSamplerEcoQoS(false);
    std::cout << "ecoqos_enable=" << eco << ",restore_automatic=" << automatic << '\n';
    std::cout << "block,cache,samples,thread_cpu_ms,wall_ms,path_queries,safety_queries,memory_queries,process_opens\n";
    for (int block = 0; block < 4; ++block) {
        bool cached = block == 1 || block == 2; // A B B A, identical cadence and warmup.
        Collector collector(cached); size_t total = 0, inaccessible = 0;
        collector.collect(total, inaccessible);
        CollectionWork work; uint64_t cpu = threadCpu(), start = GetTickCount64();
        for (int i = 0; i < 10; ++i) {
            Sleep(300); collector.collect(total, inaccessible);
            work.paths += collector.work().paths; work.safety += collector.work().safety; work.memory += collector.work().memory;
            work.processOpens += collector.work().processOpens;
        }
        std::cout << block << ',' << cached << ",10," << (threadCpu() - cpu) / 10000.0 << ',' << GetTickCount64() - start
            << ',' << work.paths << ',' << work.safety << ',' << work.memory << ',' << work.processOpens << '\n';
    }
}
