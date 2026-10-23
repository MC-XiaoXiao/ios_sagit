// SPDX-License-Identifier: GPL-2.0-only
#include "QcomUSBMapper.h"
#include "USBMMIO.h"
#include "DMARetirement.h"
#include "USBDirectDMA.h"
#include "USBFirmware.h"
#include "../../bus/QcomSMMU/HardwareContext.h"
#include "../../bus/QcomSMMU/IOTableMemory.h"
#include "../../bus/QcomSMMU/MappingEngine.h"
#include "../../bus/QcomSMMU/PhysicalPages.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <mach/vm_param.h>
#include <machine/machine_routines.h>

OSDefineMetaClassAndStructors(QcomUSBMapper,IOMapper);
namespace {
using namespace QcomPower;
IOReturn native(Status s) {
    switch (s) {
    case OK:return kIOReturnSuccess;case Invalid:return kIOReturnBadArgument;
    case Unsupported:return kIOReturnUnsupported;case Busy:return kIOReturnBusy;
    case NotReady:return kIOReturnNoResources;case TimedOut:return kIOReturnTimeout;
    default:return kIOReturnIOError;
    }
}
bool topology(IOService *provider,SMMU::IORTTopology &out,USB::FirmwareController *outBinding=nullptr) {
    auto *chosen=IORegistryEntry::fromPath("/chosen",gIODTPlane);
    if (!chosen) return false;
    auto *table=OSDynamicCast(OSData,chosen->getProperty("qcom,usb-firmware-iort"));
    auto *component=OSDynamicCast(OSData,chosen->getProperty("usb-iort-component"));
    auto *binding=OSDynamicCast(OSData,provider->getProperty("usb-iort-component"));
    auto *resource=OSDynamicCast(OSData,chosen->getProperty(USB::FirmwareControllerProperty));
    USB::FirmwareController controller;
    bool valid=false;
    if (table && component && binding && resource &&
        USB::FirmwareController::decode(resource->getBytesNoCopy(),resource->getLength(),controller) &&
        component->getLength()>1 && component->getLength()<=65 &&
        component->isEqualTo(binding)) {
        const auto *name=static_cast<const char *>(component->getBytesNoCopy());
        bool terminated=name[component->getLength()-1]==0;
        for (unsigned n=0;n+1<component->getLength();++n) if (!name[n]) terminated=false;
        valid=terminated && SMMU::parseIORT(table->getBytesNoCopy(),table->getLength(),name,0,out)==SMMU::IORTResult::Success;
        auto *core=provider->getDeviceMemoryWithIndex(0),*scratch=provider->getDeviceMemoryWithIndex(1);
        auto *specifiers=OSDynamicCast(OSArray,provider->getProperty("IOInterruptSpecifiers"));
        auto *spec=specifiers && specifiers->getCount() ? OSDynamicCast(OSData,specifiers->getObject(0)):nullptr;
        const auto *p=spec && spec->getLength()==8 ? static_cast<const uint8_t *>(spec->getBytesNoCopy()):nullptr;
        int type=0;
        valid=valid && core && scratch && controller.contains(core->getPhysicalAddress(),core->getLength()) &&
            controller.contains(scratch->getPhysicalAddress(),scratch->getLength()) && controller.coherent==out.coherent &&
            p && QcomPower::le32(p)==controller.interrupt && QcomPower::le32(p+4)==1 &&
            provider->getInterruptType(0,&type)==kIOReturnSuccess && type==kIOInterruptTypeLevel;
    }
    if (outBinding) *outBinding=valid ? controller:USB::FirmwareController{};
    chosen->release();return valid;
}
bool bootStopped(const USB::FirmwareController &binding) {
    auto *chosen=IORegistryEntry::fromPath("/chosen",gIODTPlane);
    if (!chosen) return false;
    auto *data=OSDynamicCast(OSData,chosen->getProperty(USB::FirmwareStopProperty));USB::FirmwareStop stop{};
    auto *proof=OSDynamicCast(OSData,chosen->getProperty(USB::HardwareStopProperty));USB::HardwareStop hardware{};
    const bool present=data && data->getLength()==sizeof(stop) && proof && proof->getLength()==sizeof(hardware);
    if (present) {memcpy(&stop,data->getBytesNoCopy(),sizeof(stop));memcpy(&hardware,proof->getBytesNoCopy(),sizeof(hardware));}
    const bool valid=present && stop.valid() && hardware.valid() && stop.controller==binding.base && stop.length==binding.length &&
        hardware.controller==binding.base && hardware.length==binding.length;
    chosen->release();return valid;
}
bool bypassMetadata(IOService *provider,unsigned bits,SMMU::IORTTopology &route) {
    if(!provider || bits<32 || bits>40 || provider->getDeviceMemoryCount()!=3 || provider->getProperty("iommu-parent") ||
        provider->getProperty(SMMU::ContextCaptureProperty) || provider->getProperty("qcom,usb-smmu-handoff"))return false;
    USB::FirmwareController binding;
    auto *controllers=OSDynamicCast(OSArray,provider->getProperty("IOInterruptControllers"));
    auto *specifiers=OSDynamicCast(OSArray,provider->getProperty("IOInterruptSpecifiers"));
    if(!controllers || controllers->getCount()!=1 || !controllers->getObject(0) || !specifiers || specifiers->getCount()!=1 ||
        !topology(provider,route,&binding) || route.coherent || bits>route.addressBits || !bootStopped(binding))return false;
    auto *memory=provider->getDeviceMemoryWithIndex(2);
    return memory && memory->getPhysicalAddress()==route.base && memory->getLength()==route.span;
}
struct CaptureAccess {
    const QcomUSBMapper::Resources &resources;USB::MMIO &registers;
    Status accessible() {return resources.accessible(resources.owner);}
    Status isolated() {return resources.isolated(resources.owner);}
    uint32_t read(uint32_t offset) {return registers.read(offset);}
    uint64_t read64(uint32_t offset) {return registers.read64(offset);}
};
struct NativeContextInterrupts {
    IOService *provider;
    OSArray *controllers,*specifiers;
    explicit NativeContextInterrupts(IOService *service):provider(service),
        controllers(OSDynamicCast(OSArray,service->getProperty("IOInterruptControllers"))),
        specifiers(OSDynamicCast(OSArray,service->getProperty("IOInterruptSpecifiers"))) {}
    unsigned count() const {
        return controllers && specifiers && controllers->getObject(0) && controllers->getCount()==specifiers->getCount() ?
            specifiers->getCount():0;
    }
    bool read(unsigned n,uint32_t &irq,uint32_t &trigger) const {
        if (n>=count()) return false;
        auto *controller=controllers->getObject(n);
        auto *spec=OSDynamicCast(OSData,specifiers->getObject(n));
        if (!controller || !controllers->getObject(0)->isEqualTo(controller) || !spec || spec->getLength()!=8) return false;
        const auto *p=static_cast<const uint8_t *>(spec->getBytesNoCopy());int type=0;
        irq=QcomPower::le32(p);trigger=QcomPower::le32(p+4);
        return trigger<=1 && provider->getInterruptType(n,&type)==kIOReturnSuccess &&
            type==(trigger ? kIOInterruptTypeLevel:kIOInterruptTypeEdge);
    }
};
bool interruptList(IOService *provider,const SMMU::IORTTopology &route,unsigned &count) {
    NativeContextInterrupts lines(provider);return SMMU::contextInterruptList(route,lines,count);
}
bool interruptRoute(IOService *provider,const SMMU::ContextHandoff &handoff,const SMMU::IORTTopology &route,unsigned &index) {
    NativeContextInterrupts lines(provider);return SMMU::contextInterruptRoute(route,lines,handoff.bank,index);
}
struct PhysicalSource {
    IOMemoryDescriptor *memory;
    const IODMAMapPageList *list;
    bool hasPageList() const {return list!=nullptr;}
    uint64_t pageOffset() const {return list->pageOffset;}
    uint64_t pageCount() const {return list->pageListCount;}
    uint64_t pageFrame(unsigned index) const {return list->pageList[index].phys_addr;}
    uint64_t segment(uint64_t offset,uint64_t &span) {
        IOByteCount count=0;const auto physical=memory->getPhysicalSegment(offset,&count,kIOMemoryMapperNone);
        span=count;return physical;
    }
};
}
struct QcomUSBMapper::State {
    using Status=QcomPower::Status;
    Resources resources;
    IOService *provider;
    SMMU::ContextHandoff handoff;
    IOLock *lock=nullptr;
    IOMemoryMap *mapping=nullptr;
    USB::MMIO *registers=nullptr;
    IOInterruptEventSource *interrupt=nullptr;
    SMMU::ContextInterruptBinding interruptBinding{};
    bool interruptAdded=false,bound=false,ready=false,retiring=false,fault=false,parked=false,shutdownComplete=false;
    SMMU::HardwareContext<State> context{*this};
    SMMU::IOTableMemory<State> storage{*this,handoff.outputBits};
    SMMU::PageTables<SMMU::IOTableMemory<State>> tables{storage};
    struct References {
        void retain(uintptr_t m,uintptr_t c) {reinterpret_cast<IOMemoryDescriptor *>(m)->retain();reinterpret_cast<IODMACommand *>(c)->retain();}
        void release(uintptr_t m,uintptr_t c) {reinterpret_cast<IODMACommand *>(c)->release();reinterpret_cast<IOMemoryDescriptor *>(m)->release();}
    } references;
    using Engine=SMMU::MappingEngine<decltype(tables),decltype(context),References>;
    Engine engine{tables,context,references};
    Engine::Record records[512]{};
    State(const Resources &r,IOService *p,const SMMU::ContextHandoff &h):resources(r),provider(p),handoff(h) {
        resources.owner->retain();resources.loop->retain();provider->retain();lock=IOLockAlloc();
    }
    Status accessible() {return resources.accessible(resources.owner);}
    Status isolated() {return resources.isolated(resources.owner);}
    uint32_t read(uint32_t offset) {return registers ? registers->read(offset):UINT32_MAX;}
    uint64_t read64(uint32_t offset) {return registers ? registers->read64(offset):UINT64_MAX;}
    void write(uint32_t offset,uint32_t value) {registers->write(offset,value);}
    void write64(uint32_t offset,uint64_t value) {registers->write64(offset,value);}
    void barrier() {__asm__ volatile("dsb osh" ::: "memory");}
    void delay(unsigned us) {registers->delay(us);}
    // A failed setup before the first context write has never published this
    // private root. There is then no hardware walker or TLB entry to retire.
    Status rootDetached() {return context.hasHardwareOwnership() ? context.rootDetached():OK;}
    Status sync() {return context.hasHardwareOwnership() ? context.sync():OK;}
    bool check() {
        if (!ready || fault || retiring) return false;
        if (context.check()!=OK || read(0)!=handoff.control || !handoff.otherContextsInactive(*this)) {
            // A later quiet observation cannot retire a foreign global TLB
            // entry. Recovery requires the ordinary detach/rebuild sequence.
            fault=true;return false;
        }
        return true;
    }
    bool detachRoot() {
        ready=false;retiring=true;
        if (context.hasHardwareOwnership() && context.detach()!=OK) {fault=true;return false;}
        if (engine.isolate()!=OK && context.hasHardwareOwnership()) {fault=true;return false;}
        return rootDetached()==OK;
    }
};

bool QcomUSBMapper::configuration(IOService *provider,unsigned bits,SMMU::ContextHandoff &handoff) {
    if (!provider || provider->getProperty(USB::DirectDMAProperty) || provider->getProperty(SMMU::ContextCaptureProperty) ||
        provider->getProperty(USB::DirectDMACaptureProperty) || provider->getProperty(SMMU::BypassHandoffProperty) ||
        provider->getDeviceMemoryCount()!=3) return false;
    auto *data=OSDynamicCast(OSData,provider->getProperty("qcom,usb-smmu-handoff"));
    auto *core=provider->getDeviceMemoryWithIndex(0),*smmu=provider->getDeviceMemoryWithIndex(2);
    SMMU::IORTTopology route{};unsigned index=0;
    return data && core && smmu && SMMU::ContextHandoff::decode(data->getBytesNoCopy(),data->getLength(),handoff) &&
        topology(provider,route) && handoff.valid(route,core->getPhysicalAddress(),bits) &&
        smmu->getPhysicalAddress()==handoff.smmu && smmu->getLength()==handoff.aperture &&
        interruptRoute(provider,handoff,route,index);
}
bool QcomUSBMapper::bootConfiguration(IOService *provider) {
    SMMU::IORTTopology route;USB::FirmwareController binding{};
    return provider && topology(provider,route,&binding) && bootStopped(binding);
}
bool QcomUSBMapper::firmwarePhysicalConfiguration(IOService *provider,unsigned bits) {
    SMMU::IORTTopology route;
    if (bits!=32 || !bypassMetadata(provider,bits,route) ||
        provider->getProperty(USB::DirectDMACaptureProperty) || provider->getProperty(SMMU::BypassHandoffProperty)) return false;
    auto *data=OSDynamicCast(OSData,provider->getProperty(USB::FirmwarePhysical32Property));
    return data && data->getLength()==4 && QcomPower::le32(data->getBytesNoCopy())==1 &&
        route.base==0x1680000 && route.span==0x10000 && route.stream==0x1400 &&
        provider->getDeviceMemoryWithIndex(0)->getPhysicalAddress()==0xa800000;
}
IOReturn QcomUSBMapper::captureFirmwarePhysical(const Resources &resources,IOService *provider,
                                               unsigned bits,SMMU::ContextHandoff &out) {
    out={};SMMU::IORTTopology route;
    if (!resources.owner || resources.owner->getProvider()!=provider || !resources.accessible || !resources.isolated ||
        !firmwarePhysicalConfiguration(provider,bits) || !topology(provider,route)) return kIOReturnNotReady;
    auto status=resources.accessible(resources.owner);if(status!=OK)return native(status);
    status=resources.isolated(resources.owner);if(status!=OK)return native(status);
    auto *mapping=provider->mapDeviceMemoryWithIndex(2,kIOMapInhibitCache);
    if(!mapping || !mapping->getVirtualAddress() || mapping->getLength()!=route.span) {
        OSSafeReleaseNULL(mapping);return kIOReturnNotReady;
    }
    USB::MMIO registers(reinterpret_cast<void *>(mapping->getVirtualAddress()),mapping->getLength());
    CaptureAccess access{resources,registers};
    // Reuse only the read-only cold-state audit. No context, stream or global
    // register is written and the snapshot is never installed as an IOMapper.
    status=SMMU::captureFreshContext(access,route,0xa800000,bits,0,out);
    resources.owner->setProperty("USBDMACaptureStatus",unsigned(status),32);
    if(registers.failed())status=Failed;
    mapping->release();return native(status);
}
bool QcomUSBMapper::captureConfiguration(IOService *provider,unsigned bits) {
    if (!provider || bits<32 || bits>40 || provider->getDeviceMemoryCount()!=3 ||
        provider->getProperty(USB::DirectDMAProperty) || provider->getProperty("iommu-parent") ||
        provider->getProperty(USB::DirectDMACaptureProperty) || provider->getProperty(SMMU::BypassHandoffProperty) ||
        provider->getProperty("qcom,usb-smmu-handoff")) return false;
    auto *request=OSDynamicCast(OSData,provider->getProperty(SMMU::ContextCaptureProperty));
    SMMU::IORTTopology route{};USB::FirmwareController binding{};unsigned count=0;
    if (!request || request->getLength()!=4 ||
        (QcomPower::le32(request->getBytesNoCopy())!=1 && QcomPower::le32(request->getBytesNoCopy())!=2) ||
        !topology(provider,route,&binding) || route.coherent || bits>route.addressBits || !interruptList(provider,route,count)) return false;
    auto *memory=provider->getDeviceMemoryWithIndex(2);
    if (!memory || memory->getPhysicalAddress()!=route.base || memory->getLength()!=route.span) return false;
    return bootStopped(binding);
}
bool QcomUSBMapper::bypassCaptureConfiguration(IOService *provider,unsigned bits,SMMU::BypassMode *mode) {
    SMMU::IORTTopology route;
    if(!bypassMetadata(provider,bits,route) || provider->getProperty(USB::DirectDMAProperty) ||
        provider->getProperty(SMMU::BypassHandoffProperty))return false;
    auto *request=OSDynamicCast(OSData,provider->getProperty(USB::DirectDMACaptureProperty));
    if(!request || request->getLength()!=4)return false;
    const auto value=QcomPower::le32(request->getBytesNoCopy());
    if(value!=unsigned(SMMU::BypassMode::Architected) && value!=unsigned(SMMU::BypassMode::DisabledContext) &&
        value!=unsigned(SMMU::BypassMode::UnmatchedStream))return false;
    if(mode)*mode=SMMU::BypassMode(value);
    return true;
}
bool QcomUSBMapper::bypassConfiguration(IOService *provider,unsigned bits,SMMU::BypassHandoff &handoff) {
    SMMU::IORTTopology route;
    if(!bypassMetadata(provider,bits,route) || provider->getProperty(USB::DirectDMACaptureProperty))return false;
    auto *data=OSDynamicCast(OSData,provider->getProperty(SMMU::BypassHandoffProperty));
    auto *window=OSDynamicCast(OSData,provider->getProperty(USB::DirectDMAProperty));USB::DirectDMAWindow direct;
    auto *core=provider->getDeviceMemoryWithIndex(0);
    return data && SMMU::BypassHandoff::decode(data->getBytesNoCopy(),data->getLength(),handoff) &&
        handoff.valid(route,core->getPhysicalAddress(),bits) && window &&
        USB::DirectDMAWindow::decode(window->getBytesNoCopy(),window->getLength(),direct) && direct.valid(bits);
}
IOReturn QcomUSBMapper::captureBypass(const Resources &resources,IOService *provider,unsigned bits) {
    SMMU::BypassMode mode;
    if(!resources.owner || !resources.loop || !resources.accessible || !resources.isolated || !resources.fault ||
        resources.owner->getProvider()!=provider || !bypassCaptureConfiguration(provider,bits,&mode))return kIOReturnNotReady;
    auto status=resources.accessible(resources.owner);if(status!=OK)return native(status);
    status=resources.isolated(resources.owner);if(status!=OK)return native(status);
    SMMU::IORTTopology route;
    if(!topology(provider,route))return kIOReturnNotReady;
    auto *mapping=provider->mapDeviceMemoryWithIndex(2,kIOMapInhibitCache);
    if(!mapping || !mapping->getVirtualAddress() || mapping->getLength()!=route.span) {OSSafeReleaseNULL(mapping);return kIOReturnNotReady;}
    USB::MMIO registers(reinterpret_cast<void *>(mapping->getVirtualAddress()),mapping->getLength());
    CaptureAccess access{resources,registers};SMMU::BypassHandoff observed;
    const auto id0=registers.read(0x20),id1=registers.read(0x24);
    resources.owner->setProperty("USBSMMUControl",registers.read(0),32);
    resources.owner->setProperty("USBSMMUIDR0",id0,32);
    resources.owner->setProperty("USBSMMUIDR1",id1,32);
    resources.owner->setProperty("USBSMMUIDR2",registers.read(0x28),32);
    if (id0!=UINT32_MAX && (id0&255) && (id0&255)<=16) {
        uint32_t streams[32]{};const unsigned groups=id0&255;
        for (unsigned n=0;n<groups;++n) {
            streams[2*n]=registers.read(0x800+4*n);streams[2*n+1]=registers.read(0xc00+4*n);
        }
        const unsigned first=groups<8 ? groups:8;
        resources.owner->setProperty("USBSMMUStreams0",streams,first*8);
        if (groups>8) resources.owner->setProperty("USBSMMUStreams1",streams+16,(groups-8)*8);
    }
    const unsigned pageShift=(id1>>31) ? 16:12,globalPages=1u<<(((id1>>28)&7)+1),banks=id1&255;
    if (id1!=UINT32_MAX && banks && banks<=6 && banks<=globalPages &&
        uint64_t(2)*globalPages*(1ULL<<pageShift)==mapping->getLength()) {
        uint32_t contexts[24]{};
        for (unsigned n=0;n<banks;++n) {
            const auto cb=(globalPages+n)<<pageShift,gr1=1u<<pageShift;
            contexts[4*n]=registers.read(cb);contexts[4*n+1]=registers.read(gr1+4*n);
            contexts[4*n+2]=registers.read(gr1+0x800+4*n);contexts[4*n+3]=registers.read(cb+0x58);
        }
        resources.owner->setProperty("USBSMMUBanks",contexts,banks*16);
    }
    status=SMMU::captureBypass(access,route,provider->getDeviceMemoryWithIndex(0)->getPhysicalAddress(),bits,observed,mode);
    resources.owner->setProperty("USBDMACaptureStatus",unsigned(status),32);
    if(registers.failed())status=Failed;
    mapping->release();if(status!=OK)return native(status);
    const USB::DirectDMAWindow window{1,32,0,0,uint64_t(1)<<bits};
    auto *proof=OSData::withBytes(&observed,observed.bytes),*description=OSData::withBytes(&window,sizeof(window));
    if(!proof || !description) {OSSafeReleaseNULL(proof);OSSafeReleaseNULL(description);return kIOReturnNoMemory;}
    const bool first=provider->setProperty(SMMU::BypassHandoffProperty,proof);
    const bool published=first && provider->setProperty(USB::DirectDMAProperty,description);
    if(published)provider->removeProperty(USB::DirectDMACaptureProperty);
    else if(first)provider->removeProperty(SMMU::BypassHandoffProperty);
    proof->release();description->release();return published ? kIOReturnSuccess:kIOReturnNoMemory;
}
IOReturn QcomUSBMapper::create(const Resources &resources,IOService *provider,unsigned bits,QcomUSBMapper *&out) {
    out=nullptr;SMMU::ContextHandoff handoff{};
    if (!resources.owner || !resources.loop || !resources.accessible || !resources.isolated || !resources.fault ||
        !provider || provider->getProperty("iommu-parent") || (reinterpret_cast<uintptr_t>(IOMapper::gSystem)&3)) return kIOReturnNotReady;
    resources.owner->setProperty("USBDMASetupStage","CaptureTopology");
    if (provider->getProperty(SMMU::ContextCaptureProperty)) {
        if (resources.owner->getProvider()!=provider || !captureConfiguration(provider,bits)) return kIOReturnNotReady;
        auto status=resources.accessible(resources.owner);if (status!=OK) return native(status);
        status=resources.isolated(resources.owner);if (status!=OK) return native(status);
        SMMU::IORTTopology route{};
        if (!topology(provider,route)) return kIOReturnNotReady;
        auto *mapping=provider->mapDeviceMemoryWithIndex(2,kIOMapInhibitCache);
        if (!mapping || !mapping->getVirtualAddress() || mapping->getLength()!=route.span) {
            OSSafeReleaseNULL(mapping);return kIOReturnNotReady;
        }
        USB::MMIO registers(reinterpret_cast<void *>(mapping->getVirtualAddress()),mapping->getLength());
        CaptureAccess access{resources,registers};
        auto *core=provider->getDeviceMemoryWithIndex(0);
        auto *request=OSDynamicCast(OSData,provider->getProperty(SMMU::ContextCaptureProperty));
        if (request && request->getLength()==4 && QcomPower::le32(request->getBytesNoCopy())==2) {
            status=Unsupported;NativeContextInterrupts lines(provider);
            for (unsigned bank=0;bank<route.contextCount;++bank) {
                unsigned source=0;
                if (!SMMU::contextInterruptRoute(route,lines,bank,source)) continue;
                status=SMMU::captureFreshContext(access,route,core->getPhysicalAddress(),bits,bank,handoff);
                break;
            }
        } else status=SMMU::captureInheritedContext(access,route,core->getPhysicalAddress(),bits,handoff);
        resources.owner->setProperty("USBDMACaptureStatus",unsigned(status),32);
        if (registers.failed()) status=Failed;
        mapping->release();unsigned index=0;
        if (status!=OK) return native(status);
        if (!interruptRoute(provider,handoff,route,index)) return kIOReturnNotReady;
        if (!provider->setProperty("qcom,usb-smmu-handoff",&handoff,sizeof(handoff))) return kIOReturnNoMemory;
        provider->removeProperty(SMMU::ContextCaptureProperty);
    }
    if (!configuration(provider,bits,handoff)) return kIOReturnNotReady;
    auto *mapper=new QcomUSBMapper;
    if (!mapper || !mapper->init()) {if (mapper) mapper->release();return kIOReturnNoMemory;}
    mapper->fState=new State(resources,provider,handoff);
    if (!mapper->fState) {mapper->release();return kIOReturnNoMemory;}
    out=mapper;auto &s=*mapper->fState;
    resources.owner->setProperty("USBDMASetupStage","NativeMapperStart");
    if (!s.lock || !(mapper->fAttached=mapper->attach(provider)) || !mapper->start(provider)) return kIOReturnNotReady;
    if (!provider->setProperty("iommu-parent",mapper)) return kIOReturnNoMemory;
    s.bound=true;mapper->registerService();return kIOReturnSuccess;
}
bool QcomUSBMapper::start(IOService *provider) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
    static_assert(sizeof(IOMapper)==208 && __builtin_offsetof(QcomUSBMapper,fIsSystem)==200,"Native per-device IOMapper ABI");
#pragma clang diagnostic pop
    fIsSystem=false;const bool ok=IOMapper::start(provider);
    fStarted=ok;if (!ok) IOService::stop(provider);return ok;
}
bool QcomUSBMapper::initHardware(IOService *provider) {
    if (!fState || !fState->lock || provider!=fState->provider) return false;
    auto &s=*fState;s.resources.owner->setProperty("USBDMASetupStage","MapMMIO");
    s.mapping=provider->mapDeviceMemoryWithIndex(2,kIOMapInhibitCache);
    if (!s.mapping || !s.mapping->getVirtualAddress() || s.mapping->getLength()!=s.handoff.aperture) return false;
    s.registers=new USB::MMIO(reinterpret_cast<void *>(s.mapping->getVirtualAddress()),s.mapping->getLength());
    const SMMU::Stream stream{uint16_t(s.handoff.slot),uint16_t(s.handoff.stream)};
    s.resources.owner->setProperty("USBDMASetupStage","ValidateHandoff");
    if (!s.registers || !s.handoff.unchanged(s)) return false;
    const auto configured=s.context.configure(s.handoff.aperture,s.handoff.bank,s.handoff.asid,s.handoff.inputBits,s.handoff.outputBits,&stream,1,s.handoff.cbar&255);
    s.resources.owner->setProperty("USBDMAContextStatus",unsigned(configured),32);
    if (configured!=OK || s.engine.initialize(s.handoff.inputBits)!=OK) return false;
    s.resources.owner->setProperty("USBDMASetupStage","BindContextIRQ");
    SMMU::IORTTopology route{};NativeContextInterrupts lines(provider);
    if (!topology(provider,route) || !s.interruptBinding.capture(route,lines,s.handoff.bank)) return false;
    s.interrupt=IOInterruptEventSource::interruptEventSource(this,interruptAction,provider,s.interruptBinding.index);
    if (!s.interrupt || s.resources.loop->addEventSource(s.interrupt)!=kIOReturnSuccess) return false;
    s.interruptAdded=true;return activate();
}
void QcomUSBMapper::interruptAction(OSObject *object,IOInterruptEventSource *source,int) {
    auto *self=static_cast<QcomUSBMapper *>(object);auto &s=*self->fState;
    IOLockLock(s.lock);const bool signal=s.ready && !s.check();if (signal) s.fault=true;
    IOLockUnlock(s.lock);if (signal) {source->disable();s.resources.fault(s.resources.owner);}
}
bool QcomUSBMapper::healthy() const {
    if (!fState || !fState->lock) return false;auto &s=*fState;IOLockLock(s.lock);
    const bool good=s.check(),signal=s.ready && !good;if (signal) s.fault=true;
    IOLockUnlock(s.lock);if (signal) s.resources.fault(s.resources.owner);return good;
}
bool QcomUSBMapper::matchesHandoff(const SMMU::ContextHandoff &handoff) const {
    return fState && !memcmp(&handoff,&fState->handoff,sizeof(handoff));
}
bool QcomUSBMapper::isolated() const {
    if (!fState || !fState->lock) return false;auto &s=*fState;IOLockLock(s.lock);
    const bool good=s.retiring && s.rootDetached()==OK;IOLockUnlock(s.lock);return good;
}
bool QcomUSBMapper::quiesce() {
    if (!fState || !fState->lock) return false;auto &s=*fState;IOLockLock(s.lock);
    const bool good=s.detachRoot();IOLockUnlock(s.lock);
    if (good && s.interrupt) s.interrupt->disable();return good;
}
bool QcomUSBMapper::activate() {
    if (!fState || !fState->lock) return false;auto &s=*fState;
    SMMU::IORTTopology route{};NativeContextInterrupts lines(s.provider);
    // The event source retains its original index/type. PM must not silently
    // adopt a newly routed IRQ before enabling it and attaching a private root.
    if (!topology(s.provider,route) || !s.handoff.valid(route,s.handoff.controller,s.handoff.inputBits) ||
        !s.interruptBinding.valid(route,lines)) return false;
    s.resources.owner->setProperty("USBDMASetupStage","PreAttachChecks");
    IOLockLock(s.lock);
    bool good=!s.ready && !s.engine.count() && s.interruptAdded && s.accessible()==OK && s.isolated()==OK &&
        s.read(0)==s.handoff.control && s.handoff.otherContextsInactive(s) && s.rootDetached()==OK && s.tables.destroy()==OK;
    if (good) {
        s.resources.owner->setProperty("USBDMASetupStage","AllocatePageTables");
        s.parked=false;good=s.tables.initialize(s.handoff.inputBits,s.handoff.outputBits)==OK;
    }
    if (good) {
        s.resources.owner->setProperty("USBDMASetupStage","AttachPrivateRoot");
        s.interrupt->enable();const auto status=s.context.attach(s.tables.root(),true);
        s.resources.owner->setProperty("USBDMAContextStatus",unsigned(status),32);good=status==OK;
        if (!good) {
            uint64_t failure[4]{};
            if (s.context.copyWriteFailure(failure)) s.resources.owner->setProperty("USBDMAWriteFailure",failure,sizeof(failure));
            const auto cb=s.context.bankOffset(),gr1=s.handoff.globalOffset(),bank=s.handoff.bank;
            uint32_t registers[]={cb,bank,s.handoff.slot,s.handoff.asid,
                s.read(0x800+4*s.handoff.slot),s.read(0xc00+4*s.handoff.slot),s.read(cb),
                s.read(gr1+4*bank),s.read(gr1+0x800+4*bank),s.read(cb+0x10),s.read(cb+0x30),
                s.read(cb+0x20),s.read(cb+0x24),s.read(cb+0x28),s.read(cb+0x2c),s.read(cb+0x34),
                s.read(cb+0x38),s.read(cb+0x3c),s.read(cb+0x58),s.read(cb+0x7f4)};
            s.resources.owner->setProperty("USBDMAFailureRegisters",registers,sizeof(registers));
        }
    }
    if (good) good=s.engine.begin()==OK;
    if (good) {
        s.ready=true;s.retiring=false;s.fault=false;s.parked=false;
        s.resources.owner->setProperty("USBDMASetupStage","Ready");
    } else if (s.context.hasHardwareOwnership()) s.fault=true;
    IOLockUnlock(s.lock);return good;
}
bool QcomUSBMapper::park() {
    if (!fState || !fState->lock) return false;auto &s=*fState;IOLockLock(s.lock);
    const bool good=!s.engine.count() && s.retiring && s.rootDetached()==OK && s.tables.destroy()==OK;
    if (good) s.parked=true;IOLockUnlock(s.lock);
    if (good && s.interrupt) s.interrupt->disable();return good;
}
IOReturn QcomUSBMapper::iovmMapMemory(IOMemoryDescriptor *memory,uint64_t offset,uint64_t length,uint32_t options,
    const IODMAMapSpecification *spec,IODMACommand *command,const IODMAMapPageList *list,uint64_t *address,uint64_t *bytes) {
    if (address) *address=0;if (bytes) *bytes=0;
    if (!fState || !address || !bytes || !memory || !command || !length || length>16*1024*1024 ||
        offset>memory->getLength() || length>memory->getLength()-offset || !(options&3) ||
        (options&~uint32_t(3|kIODMAMapPageListFullyOccupied))) return kIOReturnUnsupported;
    if (spec) {
        if (spec->options) return kIOReturnUnsupported;
        for (auto value:spec->resvA) if (value) return kIOReturnBadArgument;
        for (auto value:spec->resvB) if (value) return kIOReturnBadArgument;
    }
    if (list && (offset || !list->pageList || !list->pageListCount || list->pageOffset>=PAGE_SIZE ||
        length>uint64_t(list->pageListCount)*PAGE_SIZE-list->pageOffset)) return kIOReturnBadArgument;
    PhysicalSource reader{memory,list};SMMU::PhysicalPages<PhysicalSource> source(reader);
    if (source.initialize(offset,length,PAGE_SHIFT)!=OK) return kIOReturnBadArgument;
    auto &s=*fState;
    IOLockLock(s.lock);Status status=NotReady;State::Engine::Record *slot=nullptr;
    if (s.check()) {
        for (auto &record:s.records) if (!record.identity) {slot=&record;break;}
        const unsigned requestedBits=spec && spec->numAddressBits ? spec->numAddressBits:s.handoff.inputBits;
        const unsigned bits=requestedBits<s.handoff.inputBits ? requestedBits:s.handoff.inputBits;
        if (slot) status=s.engine.map(reinterpret_cast<uintptr_t>(memory),reinterpret_cast<uintptr_t>(command),length,
            source.pageOffset(),spec ? spec->alignment:0,bits,options&3,source,*slot);
        if (status==OK) {*address=slot->address;*bytes=slot->length;}
    } else if (s.ready) s.fault=true;
    const bool signal=s.fault || s.engine.faulted();if (signal) s.fault=true;
    IOLockUnlock(s.lock);if (signal) s.resources.fault(s.resources.owner);return native(status);
}
IOReturn QcomUSBMapper::iovmUnmapMemory(IOMemoryDescriptor *memory,IODMACommand *command,uint64_t address,uint64_t bytes) {
    if (!fState || !memory || !command) return kIOReturnBadArgument;auto &s=*fState;
    IOLockLock(s.lock);Status status=Invalid;
    const auto m=reinterpret_cast<uintptr_t>(memory),c=reinterpret_cast<uintptr_t>(command);
    for (auto &record:s.records) if (record.identity && record.memory==m && record.command==c && record.address==address && record.length==bytes) {
        if (s.engine.retired(m,c,address,bytes) && s.rootDetached()==OK) status=s.engine.acknowledge(record.identity,m,c);
        else status=s.engine.unmap(m,c,address,bytes);
        if (status==OK) record={};break;
    }
    const bool signal=s.engine.faulted();if (signal) s.fault=true;
    IOLockUnlock(s.lock);if (signal) s.resources.fault(s.resources.owner);return native(status);
}
uint64_t QcomUSBMapper::mapToPhysicalAddress(uint64_t address) {
    if (!fState) return 0;auto &s=*fState;IOLockLock(s.lock);uint64_t pa=0;unsigned access=0;
    if (!s.check() || s.tables.translate(address,pa,access)!=OK) pa=0;
    const bool signal=s.ready && s.fault;
    IOLockUnlock(s.lock);if (signal) s.resources.fault(s.resources.owner);return pa;
}
IOReturn QcomUSBMapper::callPlatformFunction(const OSSymbol *name,bool wait,void *a,void *b,void *c,void *d) {
    if (!name || !name->isEqualTo(USB::DMARetirementFunction)) return IOService::callPlatformFunction(name,wait,a,b,c,d);
    auto *request=static_cast<USB::DMARetirement *>(a);
    if (!fState || !request || b || c || d || request->version!=1 || request->size!=sizeof(*request) ||
        bool(request->memory)!=bool(request->command) || ml_at_interrupt_context()) return kIOReturnBadArgument;
    auto &s=*fState;IOLockLock(s.lock);Status status=NotReady;unsigned count=0;
    if (s.retiring && s.context.hasHardwareOwnership() && s.rootDetached()==OK) {
        status=OK;
        if (!request->memory) {IOLockUnlock(s.lock);return kIOReturnSuccess;}
        for (const auto &record:s.records) if (record.identity && record.memory==request->memory && record.command==request->command) {
            ++count;if (!s.engine.retired(record.memory,record.command,record.address,record.length)) status=Busy;
        }
        if (!count) status=Invalid;
        if (status==OK) for (auto &record:s.records)
            if (record.identity && record.memory==request->memory && record.command==request->command) {
                status=s.engine.acknowledge(record.identity,record.memory,record.command);
                if (status!=OK) break;record={};
            }
    }
    IOLockUnlock(s.lock);return native(status);
}
bool QcomUSBMapper::shutdown() {
    if (!fState) return true;auto &s=*fState;
    if (s.shutdownComplete) return true;
    if (s.lock) {
        IOLockLock(s.lock);
        const bool clean=!s.engine.count() && (s.parked || (s.detachRoot() && s.tables.destroy()==OK));
        IOLockUnlock(s.lock);if (!clean) return false;
    }
    if (s.interrupt) s.interrupt->disable();
    if (s.interruptAdded) {s.resources.loop->removeEventSource(s.interrupt);s.interruptAdded=false;}
    OSSafeReleaseNULL(s.interrupt);
    if (s.bound) {if (s.provider->getProperty("iommu-parent")!=this) return false;s.provider->removeProperty("iommu-parent");s.bound=false;}
    if (fStarted) {IOService::stop(s.provider);fStarted=false;}
    if (fAttached) {detach(s.provider);fAttached=false;}
    s.shutdownComplete=true;
    return true;
}
void QcomUSBMapper::free() {
    if (!shutdown()) {IOLog("QcomUSB: unproved SMMU retirement; mapper and DMA retained\n");return;}
    if (fState) {
        auto &s=*fState;delete s.registers;OSSafeReleaseNULL(s.mapping);OSSafeReleaseNULL(s.provider);
        auto *owner=s.resources.owner;auto *loop=s.resources.loop;if (s.lock) IOLockFree(s.lock);
        delete fState;fState=nullptr;loop->release();owner->release();
    }
    IOMapper::free();
}
