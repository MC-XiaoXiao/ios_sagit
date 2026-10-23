// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <CPUPerformancePolicy.h>

// Generic policy adapter. Only the backend callbacks below know the hardware
// controller; the XNU capture and policy contain no OSM registers or CPU IDs.
class CPUPerformanceAdapter : public OSObject {
    OSDeclareDefaultStructors(CPUPerformanceAdapter);
public:
    struct Backend {
        void *context;
        bool (*online)(void *);
        uint32_t (*count)(void *,unsigned);
        uint32_t (*frequency)(void *,unsigned,unsigned);
        uint32_t (*current)(void *,unsigned);
        uint32_t (*ceiling)(void *,unsigned);
        IOReturn (*set)(void *,unsigned,uint32_t);
    };
    struct Mapping {unsigned cpu,cluster;};
    enum Status : uint32_t {Automatic=1,Manual=2,Fault=4,Paused=8,OwnershipLost=16,Unavailable=32};
private:
    Backend fBackend{};
    IOWorkLoop *fLoop=nullptr;
    IOTimerEventSource *fTimer=nullptr;
    CPUPerformance::Policy fPolicy;
    Mapping fCPUs[CPUPerformance::MaxCPUs]={};
    uint64_t fDrops[CPUPerformance::MaxCPUs]={};
    unsigned fCPUCount=0,fClusters=0;
    uint32_t fEpoch=0;
    IOReturn fStartStatus=kIOReturnBadArgument;
    bool fAdded=false,fInstalled=false,fClaimed=false,fWasOnline=false,fLost=false,fTimerFailed=false;
    struct State {bool automatic=false,fault=false,available=false;uint32_t utilization=0,demand=0;uint64_t transitions=0,dropped=0;IOReturn error=kIOReturnSuccess;} fState[CPUPerformance::MaxClusters];
    static void timer(OSObject *,IOTimerEventSource *);
    static IOReturn shutdown(OSObject *,void *,void *,void *,void *);
    void tick();
    IOReturn armTimer();
public:
    bool init() APPLE_KEXT_OVERRIDE;
    IOReturn startStatus() const {return fStartStatus;}
    bool start(IOWorkLoop *,const Backend &,const Mapping *,unsigned cpus,unsigned clusters);
    void stop();
    void free() APPLE_KEXT_OVERRIDE;
    IOReturn setAutomatic(unsigned,bool);
    void manual(unsigned);
    uint32_t status(unsigned) const;
    uint32_t utilization(unsigned c) const {return c<fClusters ? fState[c].utilization : 0;}
    uint32_t demand(unsigned c) const {return c<fClusters ? fState[c].demand : 0;}
    uint64_t dropped(unsigned c) const {return c<fClusters ? fState[c].dropped : 0;}
    uint64_t transitions(unsigned c) const {return c<fClusters ? fState[c].transitions : 0;}
};
