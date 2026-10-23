// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKitUser.h>
#include <PowerUserProtocol.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/processor_info.h>
#include <sys/sysctl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// Read-only companion diagnostics: CPU scheduling load and accepted OSM
// targets are separate observations. Neither is a measured clock frequency.
namespace CPUStatus {
static bool topology() {
    bool valid=false;
    const char *names[]={"hw.ncpu","hw.activecpu","hw.physicalcpu","hw.logicalcpu"};
    for(const char *name:names) {
        int value=0;size_t bytes=sizeof(value);errno=0;
        int result=sysctlbyname(name,&value,&bytes,nullptr,0),error=errno;
        bool ok=!result && bytes==sizeof(value) && value>0 && value<=64;
        printf("CPU_TOPOLOGY name=%s status=%d errno=%d valid=%u value=%d\n",name,result,error,ok,value);
        valid|=ok;
    }
    return valid;
}
static void osm() {
    mach_port_t iterator=0;
    auto status=IOServiceGetMatchingServices(0,IOServiceMatching("QcomOSM"),&iterator);
    if(status){printf("CPU_OSM_MATCH status=0x%x\n",status);return;}
    unsigned count=0;
    for(mach_port_t service;(service=IOIteratorNext(iterator));) {
        ++count;mach_port_t connection=0;
        status=IOServiceOpen(service,mach_task_self(),0,&connection);IOObjectRelease(service);
        printf("CPU_OSM_OPEN status=0x%x\n",status);
        if(status)continue;
        uint64_t info[3]{};uint32_t words=3;
        status=IOConnectCallScalarMethod(connection,PowerUser::GetInfo,nullptr,0,info,&words);
        const bool valid=!status && words==3 && info[0]==2 && info[1]==ClockController::OSM;
        printf("CPU_OSM_INFO status=0x%x valid=%u protocol=%llu controller=%llu capabilities=0x%llx frequency_observation=desired_not_measured\n",
            status,valid,(unsigned long long)info[0],(unsigned long long)info[1],(unsigned long long)info[2]);
        if(valid)for(unsigned cluster=0;cluster<2;++cluster) {
            using namespace ClockController;
            const struct {Operation op;const char *name;} requests[]={
                {GetRate,"desired_hz"},{GetMinimumRate,"minimum_hz"},{GetMaximumRate,"maximum_hz"},
                {GetPerformanceLimit,"ceiling_hz"},{GetPerformanceStateCount,"state_count"},
                {GetAutomaticPerformance,"automatic"},{GetPerformanceStatus,"policy_status"},
                {GetPerformanceUtilization,"utilization_permille"},{GetPerformanceDemand,"demand"},
                {GetPerformanceTransitions,"transitions"},{GetPerformanceDroppedSamples,"dropped"}};
            for(const auto &r:requests) {
                const uint64_t input[]={r.op,cluster};uint64_t value=0;words=1;
                status=IOConnectCallScalarMethod(connection,PowerUser::ReadResource,input,2,&value,&words);
                printf("CPU_OSM_READ cluster=%u name=%s status=0x%x valid=%u value=%llu\n",
                    cluster,r.name,status,!status && words==1,(unsigned long long)value);
            }
        }
        IOServiceClose(connection);
    }
    IOObjectRelease(iterator);printf("CPU_OSM_END services=%u writes=0\n",count);
}
static void snapshot() {
    mach_port_t iterator=0;
    const auto status=IOServiceGetMatchingServices(0,IOServiceMatching("QcomOSM"),&iterator);
    if(status){printf("CPU_OSM_SNAPSHOT_ERROR status=0x%x\n",status);return;}
    for(mach_port_t service;(service=IOIteratorNext(iterator));) {
        auto value=IORegistryEntryCreateCFProperty(service,CFSTR("CPUPerformanceSnapshot"),nullptr,0);
        IOObjectRelease(service);
        char text[2048];
        if(value && CFGetTypeID(value)==CFStringGetTypeID() &&
            CFStringGetCString(static_cast<CFStringRef>(value),text,sizeof(text),kCFStringEncodingUTF8))
            fputs(text,stdout);
        else puts("CPU_OSM_SNAPSHOT_UNAVAILABLE");
        if(value)CFRelease(value);
    }
    IOObjectRelease(iterator);
}
struct Load {
    natural_t count=0;
    uint32_t ticks[64][CPU_STATE_MAX]{};
    uint64_t time=0;
};
static bool load(host_t host,Load &out) {
    out={};processor_info_array_t values=nullptr;mach_msg_type_number_t words=0;
    auto status=host_processor_info(host,PROCESSOR_CPU_LOAD_INFO,&out.count,&values,&words);
    const bool valid=!status && values && out.count && out.count<=64 &&
        words==out.count*PROCESSOR_CPU_LOAD_INFO_COUNT;
    if(valid)memcpy(out.ticks,values,words*sizeof(integer_t));
    if(values)vm_deallocate(mach_task_self(),reinterpret_cast<vm_address_t>(values),words*sizeof(integer_t));
    out.time=mach_absolute_time();
    if(!valid)printf("CPU_LOAD_ERROR status=0x%x cpus=%u words=%u\n",status,out.count,words);
    return valid;
}
static int run(unsigned seconds) {
    setbuf(stdout,nullptr);const bool present=topology();osm();snapshot();
    if(!seconds)return present?0:1;
    const host_t host=mach_host_self();Load before{};
    if(!load(host,before)){mach_port_deallocate(mach_task_self(),host);return 1;}
    mach_timebase_info_data_t timebase{};mach_timebase_info(&timebase);
    printf("CPU_LOAD_BEGIN seconds=%u period_ms=500 timebase=%u/%u clock_frequency_measurement=0\n",seconds,timebase.numer,timebase.denom);
    int result=0;
    for(unsigned sample=0;sample<seconds*2;++sample) {
        usleep(500000);Load after{};
        if(!load(host,after)){result=1;break;}
        if(after.count!=before.count || after.time<=before.time) {
            puts("CPU_LOAD_RESET topology_or_clock_changed=1");before=after;continue;
        }
        for(unsigned cpu=0;cpu<after.count;++cpu) {
            uint64_t total=0,idle=0;
            for(unsigned state=0;state<CPU_STATE_MAX;++state) {
                const uint32_t delta=after.ticks[cpu][state]-before.ticks[cpu][state];
                total+=delta;if(state==CPU_STATE_IDLE)idle=delta;
            }
            const bool valid=total && total<1000000;
            printf("CPU_LOAD sample=%u cpu=%u begin=%llu end=%llu ticks=%llu idle=%llu valid=%u busy_permille=%llu\n",
                sample,cpu,(unsigned long long)before.time,(unsigned long long)after.time,
                (unsigned long long)total,(unsigned long long)idle,valid,
                (unsigned long long)(valid?(total-idle)*1000/total:0));
        }
        before=after;snapshot();
    }
    mach_port_deallocate(mach_task_self(),host);puts("CPU_LOAD_END");return result;
}
}
