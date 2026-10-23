// SPDX-License-Identifier: GPL-2.0-only
#include "SecureApp.h"
#include <IOKit/IOLib.h>
#include <kern/task.h>
#include <kern/clock.h>
#include <IOKit/IOMapper.h>
#include <IOBufferAllocation.h>
#include <SecureMonitorAction.h>
#include <SMCCC.h>
#include <pexpert/pexpert.h>
#define HOME_LOG(...) do { if (fTrace) kprintf(__VA_ARGS__); else IOLog(__VA_ARGS__); } while (false)
using namespace QcomHomeProtocol;

namespace {
struct NativeSCM {
    void invoke(FingerprintSCM::Frame &frame) {
        QcomSecure::Registers registers{};
        for (unsigned i=0;i<7;++i) registers.x[i]=frame.x[i];
        QcomSecure::smc(registers);
        for (unsigned i=0;i<7;++i) frame.x[i]=registers.x[i];
    }
    void busyDelay() { IOSleep(30); }
};
}
IOReturn HomeSecureApp::runSecureCall(void *argument) {
    auto &c=*static_cast<CallContext *>(argument); auto &s=*c.self;
    NativeSCM backend;
    c.reply=FingerprintSCM::call(backend,c.id,c.types,c.args,c.count,
        reinterpret_cast<uint64_t *>(s.fBytes+0x100),s.fPhysical+0x100,s.fBlocked);
    return s.fBlocked ? kIOReturnNotResponding : kIOReturnSuccess;
}
HomeSecureApp::Reply HomeSecureApp::call(uint32_t id,uint32_t types,const uint64_t *args,unsigned count) {
    if (!fBytes || !fSCM || !fClient || fBlocked) return {-1,0xffffffffU,0,0};
    bool previous=fBlocked;
    CallContext context{this,id,types,args,count,{-1,0xffffffffU,0,0}};
    SecureMonitorAction::Request request;
    request.action=runSecureCall; request.context=&context;
    auto result=fSCM->callPlatformFunction(SecureMonitorAction::Function,true,&request,fClient,nullptr,nullptr);
    Reply reply=(result==kIOReturnSuccess || result==kIOReturnNotResponding) ? context.reply : Reply{-1,0xffffffffU,0,0};
    if (!previous && fBlocked) {
        // The trusted app has retained our buffer for a listener continuation.
        // Do not reuse/free it or send invented storage/keymaster responses.
        HOME_LOG("FingerprintHID: incomplete secure transaction (result %u, data %u); session quarantined\n",reply.result,reply.data);
    }
    return reply;
}
bool HomeSecureApp::initialize(IOService *client) {
    if (fMemory || fSCM || !client) return false;
    UInt32 trace=0; PE_parse_boot_argn("input-report-trace",&trace,sizeof(trace)); fTrace=trace!=0;
    auto *matching=IOService::serviceMatching("QcomSCM");
    if (!matching) return false;
    fSCM=IOService::waitForMatchingService(matching,5000000000ULL); matching->release();
    auto *convention=fSCM ? OSDynamicCast(OSString,fSCM->getProperty("SecureFirmwareConvention")) : nullptr;
    if (!convention || !convention->isEqualTo("smc64") || (reinterpret_cast<uintptr_t>(IOMapper::gSystem)&3)) {
        OSSafeReleaseNULL(fSCM); return false;
    }
    fClient=client;
    // On ARM, InhibitCache selects Device memory and faults on bzero's DC ZVA
    // and unaligned protocol fields. The common allocator selects Normal NC;
    // SMCCC supplies the ordering barriers around every secure transaction.
    static_assert((IOKitMemory::bufferOptions(false,true)&kIOMapCacheMask)==kIOMapWriteCombineCache);
    fMemory=IOKitMemory::allocateBuffer(IOKitMemory::bufferOptions(false,true),
        Capacity,4096,0xfffff000ULL);
    if (!fMemory || fMemory->prepare()!=kIOReturnSuccess) { OSSafeReleaseNULL(fMemory); release(); return false; }
    IOByteCount length=0;
    fPhysical=fMemory->getPhysicalSegment(0,&length,kIOMemoryMapperNone);
    fBytes=static_cast<uint8_t *>(fMemory->getBytesNoCopy());
    if (!fBytes || !fPhysical || length<Capacity || fPhysical>0x100000000ULL-Capacity) {
        release(); return false;
    }
    bzero(fBytes,Capacity);
    // QcomSCM probed the conduit and serializes this entire QSEE transaction
    // with its PAS clients, including retries and interrupted continuations.
    uint64_t feature[]={15}; Reply r=call(0x02000603,1,feature,1);
    HOME_LOG("FingerprintHID: QSEE feature status=%d version=0x%x\n",r.status,r.result);
    if (r.status) { release(); return false; }
    fWhitelist=r.result>=0x00400000; // MAKE_WHITELIST_VERSION(1,0,0)
    return true;
}
bool HomeSecureApp::load(const char *name,OSData *image) {
    if (!name || !image || !fBytes || fApp || fBlocked || image->getLength()<56 ||
        image->getLength()>Capacity-ImageOffset) return false;
    const auto *p=static_cast<const uint8_t *>(image->getBytesNoCopy());
    unsigned mdt=read32(p), size=image->getLength()-4;
    if (mdt<52 || mdt>size || p[4]!=0x7f || p[5]!='E' || p[6]!='L' || p[7]!='F' || p[8]!=1) return false;
    size_t n=strnlen(name,32);
    if (!n || n>=32) return false;
    bzero(fBytes,64); memcpy(fBytes,name,n);
    uint64_t lookup[]={fPhysical,n};
    Reply existing=call(0x32000103,2|(2<<4),lookup,2);
    HOME_LOG("FingerprintHID: lookup %s status=%d result=0x%x type=0x%x data=0x%x\n",name,existing.status,existing.result,existing.type,existing.data);
    if (existing.status || fBlocked) return false;
    if (!existing.result && existing.type==0xee01 && existing.data) {
        HOME_LOG("FingerprintHID: trusted app %s already has an owner\n",name); return false;
    }
    if (existing.result!=0xffffffffU) return false;
    memcpy(fBytes+ImageOffset,p+4,size);
    uint64_t args[]={mdt,size,fPhysical+ImageOffset};
    Reply r=call(0x32000101,3,args,3);
    HOME_LOG("FingerprintHID: load %s status=%d result=0x%x type=0x%x data=0x%x\n",name,r.status,r.result,r.type,r.data);
    if (r.status || r.result || r.type!=0xee01 || !r.data) return false;
    fApp=uint32_t(r.data); fLoaded=true; return true;
}
bool HomeSecureApp::invoke(unsigned size,unsigned pointerOffset) {
    if (!fApp || fBlocked || !size || size>ImageOffset-PayloadOffset) return false;
    uint64_t args[]={fApp,fPhysical,64,fPhysical+64,64,fPhysical+0x200,32};
    memset(fBytes+64,0xff,64);
    // Match the kernel's modified-command whitelist: one contiguous 32-bit
    // physical pointer in the request envelope, with its actual shared length.
    bzero(fBytes+0x200,32);
    write32(fBytes+0x200,(1U<<31)|pointerOffset);
    write32(fBytes+0x204,size);
    Reply r=fWhitelist ? call(0x30000006,7|(2<<6)|(2<<10)|(2<<14),args,7) :
        call(0x30000001,5|(2<<6)|(2<<10),args,5);
    if (r.status || r.result) HOME_LOG("FingerprintHID: send status=%d result=0x%x type=0x%x data=0x%x\n",r.status,r.result,r.type,r.data);
    return !r.status && !r.result && !fBlocked;
}
bool HomeSecureApp::fpc(uint32_t group,uint32_t command,int32_t &status) {
    if (!fLoaded || fBlocked || !fBytes) return false;
    if (!fpcRequest(payload(),FPCPayloadSize,group,command)) return false;
    bzero(fBytes,64);
    write32(fBytes,FPCPayloadSize);
    write32(fBytes+4,uint32_t(fPhysical+PayloadOffset)); // modified-command FD slot at offset 4
    if (!invoke(FPCPayloadSize,4)) return false;
    if (read32(fBytes+64)) { HOME_LOG("FingerprintHID: FPC response error=0x%x group=%u command=%u\n",read32(fBytes+64),group,command); return false; }
    status=int32_t(read32(payload()+8)); return true;
}
bool HomeSecureApp::goodix(uint32_t command,unsigned size,uint32_t &status) {
    if (!fLoaded || fBlocked || !fBytes) return false;
    UInt64 ticks,ns; clock_get_uptime(&ticks); absolutetime_to_nanoseconds(ticks,&ns);
    // The HAL supplies a millisecond counter modulo one day. Relative timing
    // for navigation needs a moving counter even without Android wall time.
    if (!goodixEnvelope(fBytes,64,++fSequence,command,uint32_t(fPhysical+PayloadOffset),size,(ns/1000000)%86400000) || !invoke(size,20)) return false;
    if (read32(fBytes+68)) return false;
    status=read32(fBytes+64); return true;
}
bool HomeSecureApp::unload() {
    if (!fLoaded) return !fBlocked;
    if (fBlocked) return false;
    uint64_t args[]={fApp}; Reply r=call(0x32000102,1,args,1);
    if (r.status || r.result) { fBlocked=true; return false; }
    fApp=0; fLoaded=false; return true;
}
void HomeSecureApp::release() {
    if (!unload() || fBlocked) return; // Keep memory pinned while secure world can still use it.
    if (fMemory) {
        if (fMemory->complete()!=kIOReturnSuccess) { fBlocked=true; return; }
        if (fBytes) bzero(fBytes,Capacity);
        fMemory->release(); fMemory=nullptr;
    }
    fBytes=nullptr; fPhysical=0;
    OSSafeReleaseNULL(fSCM); fClient=nullptr;
}
