// SPDX-License-Identifier: BSD-2-Clause
// Internal iOS diagnostic client, linked only against the SDK's actual IOKit
// exports. The scalar ABI comes from PowerUserProtocol.h, not a private PMGR.
#include <IOKitUser.h>
#include <PowerUserProtocol.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <atomic>
#include "../gpuctl/CPUStatus.h"

static bool number(const char *text, uint64_t &out) {
    if (!text || !*text || *text == '-') return false;
    errno = 0; char *end = nullptr; out = strtoull(text,&end,0); return !errno && end && !*end;
}

namespace {
struct LoadWorker {
    int cpu = 0, error = 0, unbindError = 0;
    uint64_t iterations = 0, checksum = 0, deadline = 0, elapsed = 0;
    std::atomic<bool> ready{false};
    std::atomic<bool> *begin = nullptr, *stop = nullptr;
};
void *loadWorker(void *context) {
    auto &w = *static_cast<LoadWorker *>(context);
    if (sysctlbyname("kern.sched_thread_bind_cpu", nullptr, nullptr, &w.cpu, sizeof(w.cpu))) w.error = errno;
    int bound = -1; size_t bytes = sizeof(bound);
    if (!w.error && (sysctlbyname("kern.sched_thread_bind_cpu", &bound, &bytes, nullptr, 0) ||
                     bytes != sizeof(bound) || bound != w.cpu)) w.error = EIO;
    w.ready.store(true, std::memory_order_release);
    while (!w.begin->load(std::memory_order_acquire) && !w.stop->load(std::memory_order_acquire)) usleep(1000);
    const uint64_t started = mach_absolute_time();
    uint64_t value = 1;
    // Each bound worker stops itself even if the sampler cannot get CPU time.
    while (!w.error && !w.stop->load(std::memory_order_acquire) && mach_absolute_time() < w.deadline) {
        for (unsigned i = 0; i < 8192; ++i) value = value * 1664525 + 1013904223;
        w.iterations += 8192;
    }
    w.elapsed = mach_absolute_time() - started;
    w.checksum = value;
    int unbound = -1;
    if (sysctlbyname("kern.sched_thread_bind_cpu", nullptr, nullptr, &unbound, sizeof(unbound))) w.unbindError = errno;
    return nullptr;
}
int runLoad(unsigned seconds) {
    setbuf(stdout, nullptr);
    int cpus = 0, bound = -1; size_t bytes = sizeof(cpus);
    if (sysctlbyname("hw.activecpu", &cpus, &bytes, nullptr, 0) || bytes != sizeof(cpus) || cpus < 1 || cpus > 8)
        return 1;
    bytes = sizeof(bound);
    if (sysctlbyname("kern.sched_thread_bind_cpu", &bound, &bytes, nullptr, 0)) {
        fprintf(stderr, "CPU_LOAD_BIND_UNAVAILABLE errno=%d; development XNU requires boot argument enable_skstb\n", errno);
        return 1;
    }
    std::atomic<bool> begin{false}, stop{false};
    LoadWorker workers[8]; pthread_t threads[8]{}; unsigned created = 0;
    int result = 0;
    for (int cpu = 0; cpu < cpus; ++cpu) {
        auto &w = workers[cpu]; w.cpu = cpu; w.begin = &begin; w.stop = &stop;
        int status = pthread_create(&threads[cpu], nullptr, loadWorker, &w);
        if (status) { fprintf(stderr, "CPU_LOAD_CREATE cpu=%d error=%d\n", cpu, status); result = 1; break; }
        ++created;
    }
    // Readiness has a deadline; no worker runs the workload on a failed bind.
    mach_timebase_info_data_t timebase{};
    if (mach_timebase_info(&timebase) != KERN_SUCCESS || !timebase.numer || !timebase.denom) result = 1;
    const uint64_t timeout = result ? 0 : 5000000000ULL * timebase.denom / timebase.numer;
    const uint64_t start = mach_absolute_time();
    for (unsigned i = 0; i < created && !result; ++i) {
        while (!workers[i].ready.load(std::memory_order_acquire) && mach_absolute_time() - start < timeout) usleep(1000);
        if (!workers[i].ready.load(std::memory_order_acquire)) result = 1;
        else {
            printf("CPU_LOAD_BOUND cpu=%u error=%d\n", i, workers[i].error);
            if (workers[i].error) result = 1;
        }
    }
    if (!result) {
        printf("CPU_LOAD_WORK_BEGIN cpus=%d seconds=%u timebase_numer=%u timebase_denom=%u\n",
               cpus, seconds, timebase.numer, timebase.denom);
        const uint64_t deadline = mach_absolute_time() + static_cast<uint64_t>(seconds) * 1000000000ULL * timebase.denom / timebase.numer;
        for (unsigned i = 0; i < created; ++i) workers[i].deadline = deadline;
        begin.store(true, std::memory_order_release);
        result = CPUStatus::run(seconds);
    }
    stop.store(true, std::memory_order_release);
    for (unsigned i = 0; i < created; ++i) {
        int status = pthread_join(threads[i], nullptr);
        if (status || workers[i].error || workers[i].unbindError || !workers[i].iterations) result = 1;
        printf("CPU_LOAD_WORK_END cpu=%u join=%d unbind_error=%d elapsed_ticks=%llu iterations=%llu checksum=%llu\n",
               i, status, workers[i].unbindError,
               static_cast<unsigned long long>(workers[i].elapsed),
               static_cast<unsigned long long>(workers[i].iterations), static_cast<unsigned long long>(workers[i].checksum));
    }
    if (!result) {
        puts("CPU_LOAD_IDLE_BEGIN seconds=2");
        result = CPUStatus::run(2);
    }
    return result;
}
}
int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "load")) {
        uint64_t seconds = 0;
        if (argc != 3 || !number(argv[2], seconds) || !seconds || seconds > 10) {
            fputs("usage: powerctl load <seconds 1..10>; requires development boot argument enable_skstb\n", stderr);
            return 2;
        }
        return runLoad(static_cast<unsigned>(seconds));
    }
    if (argc == 2 && !strcmp(argv[1],"--schema")) {
        puts("protocol=2; selectors: 0 info, 1 read(operation,id), 2 OSM limit(cluster,hz), 3 OSM frequency(cluster,hz), 4 OSM automatic(cluster,0/1); controllers: GCC=1 RPM=2 SPMI=3 MMCC=4 GPUCC=5 OSM=6; zero limit clears the ceiling");
        return 0;
    }
    bool list = argc == 2 && !strcmp(argv[1],"list");
    bool read = argc == 5 && !strcmp(argv[1],"read");
    bool limit = argc == 4 && !strcmp(argv[1],"limit");
    bool frequency = argc == 4 && !strcmp(argv[1],"frequency");
    bool automatic = argc == 4 && !strcmp(argv[1],"auto");
    uint64_t controller = ClockController::OSM, input[2] = {};
    if (!(list || read || limit || frequency || automatic) || (read && (!number(argv[2],controller) || !number(argv[3],input[0]) ||
        !number(argv[4],input[1]) || input[0] > UINT32_MAX || !PowerUser::readable(input[0]) || input[1] > UINT32_MAX)) ||
        ((limit || frequency || automatic) && (!number(argv[2],input[0]) || !number(argv[3],input[1]) || input[0] > 1 || (frequency && !input[1]) || (automatic && input[1]>1))) ||
        controller < ClockController::GCC || controller > ClockController::OSM) {
        fputs("usage: powerctl list | read <controller> <operation> <id> | limit <cluster> <Hz or 0> | frequency <cluster> <Hz> | auto <cluster> <0 or 1> | load <seconds 1..10> | --schema\n",stderr); return 2;
    }
    CFMutableDictionaryRef matching = IOServiceMatching("GenericClockController");
    if (!matching) return 1;
    mach_port_t iterator = MACH_PORT_NULL;
    kern_return_t status = IOServiceGetMatchingServices(MACH_PORT_NULL,matching,&iterator);
    if (status) { fprintf(stderr,"matching failed: 0x%x\n",status); return 1; }
    unsigned found = 0; int result = 1; mach_port_t selected = MACH_PORT_NULL;
    for (mach_port_t service; (service = IOIteratorNext(iterator));) {
        mach_port_t connection = MACH_PORT_NULL;
        status = IOServiceOpen(service,mach_task_self(),0,&connection); IOObjectRelease(service);
        if (status) { if (list) fprintf(stderr,"open failed: 0x%x\n",status); continue; }
        uint64_t info[3] = {}; uint32_t count = 3;
        status = IOConnectCallScalarMethod(connection,PowerUser::GetInfo,nullptr,0,info,&count);
        if (!status && count == 3 && info[0] == 2 && (list || info[1] == controller)) {
            ++found;
            if (list) { printf("controller=%llu protocol=%llu capabilities=0x%llx\n",info[1],info[0],info[2]); result = 0; }
            else if (!selected) { selected = connection; connection = MACH_PORT_NULL; }
        }
        if (connection) IOServiceClose(connection);
    }
    IOObjectRelease(iterator);
    if (!list && found != 1) { if (selected) IOServiceClose(selected); fprintf(stderr,"expected one controller, found %u\n",found); return 1; }
    if (selected) {
        uint64_t value = 0; uint32_t count = 1;
        status = IOConnectCallScalarMethod(selected,read ? PowerUser::ReadResource : frequency ? PowerUser::SetCPUFrequency : automatic ? PowerUser::SetAutomatic : PowerUser::SetLimit,input,2,&value,&count);
        IOServiceClose(selected);
        if (status || count != 1) { fprintf(stderr,"request failed: 0x%x\n",status); result = 1; }
        else { printf("%llu\n",value); result = 0; }
    }
    return result;
}
