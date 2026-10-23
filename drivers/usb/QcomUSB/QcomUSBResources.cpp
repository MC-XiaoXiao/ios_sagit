// SPDX-License-Identifier: BSD-2-Clause
#include "QcomUSBResources.h"
#include "QcomUSBMapper.h"
#include "USBControllerResources.h"
#include "USBPlatform.h"
#include "USBConnectorProtocol.h"
#include "USBPhyProtocol.h"
#include "USBInterconnect.h"
#include "USBDirectDMA.h"
#include "USBMMIO.h"
#include "DWC3Core.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMapper.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOMessage.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <kern/thread_call.h>
#include <machine/machine_routines.h>

OSDefineMetaClassAndStructors(QcomUSBResources, IOService);
namespace {
UInt32 cell(const UInt8 *p) { return UInt32(p[0])|UInt32(p[1])<<8|UInt32(p[2])<<16|UInt32(p[3])<<24; }
USB::Result result(IOReturn r) {
    if (r==kIOReturnSuccess) return USB::Result::Success;
    if (r==kIOReturnAborted) return USB::Result::Aborted;
    if (r==kIOReturnTimeout) return USB::Result::Timeout;
    if (r==kIOReturnBusy) return USB::Result::Busy;
    if (r==kIOReturnUnsupported) return USB::Result::Unsupported;
    if (r==kIOReturnNotReady || r==kIOReturnOffline || r==kIOReturnNotAttached) return USB::Result::NotReady;
    if (r==kIOReturnBadArgument) return USB::Result::Invalid;
    return USB::Result::HardwareError;
}
IOReturn native(USB::Result r) {
    switch (r) {
    case USB::Result::Success:return kIOReturnSuccess;
    case USB::Result::Invalid:return kIOReturnBadArgument;
    case USB::Result::Aborted:return kIOReturnAborted;
    case USB::Result::Timeout:return kIOReturnTimeout;
    case USB::Result::Busy:return kIOReturnBusy;
    case USB::Result::Unsupported:return kIOReturnUnsupported;
    case USB::Result::NotReady:return kIOReturnNotReady;
    case USB::Result::Overflow:return kIOReturnOverrun;
    default:return kIOReturnIOError;
    }
}
QcomPower::Status smmuStatus(USB::Result r) {
    switch (r) {
    case USB::Result::Success:return QcomPower::OK;
    case USB::Result::Busy:return QcomPower::Busy;
    case USB::Result::NotReady:return QcomPower::NotReady;
    case USB::Result::Timeout:return QcomPower::TimedOut;
    default:return QcomPower::Failed;
    }
}
bool below(IOService *service, IOService *bus) {
    for (unsigned i=0;service && i<8;++i,service=service->getProvider()) if (service==bus) return true;
    return false;
}
IOService *copyPHY(IOService *bus, UInt64 address) {
    auto *matching=IOService::serviceMatching("QcomUSBPHY");
    if (!matching) return nullptr;
    auto *services=IOService::getMatchingServices(matching); matching->release();
    if (!services) return nullptr;
    IOService *found=nullptr, *service;
    while ((service=OSDynamicCast(IOService,services->getNextObject()))) {
        auto *nub=service->getProvider();
        auto *memory=nub ? nub->getDeviceMemoryWithIndex(0) : nullptr;
        if (!nub || nub->getProvider()!=bus || !memory || memory->getPhysicalAddress()!=address) continue;
        if (found) { found->release(); found=nullptr; break; }
        found=service; found->retain();
    }
    services->release(); return found;
}
}

struct QcomUSBResources::State {
    QcomUSBResources *self;
    IOService *provider, *bus, *connector=nullptr, *interconnect=nullptr, *phys[2]{}, *owner=nullptr, *noticeOwner=nullptr;
    IOMapper *mapper=nullptr;
    QcomUSBMapper *ownedMapper=nullptr;
    IOLock *lock=nullptr;
    IOSimpleLock *noticeLock=nullptr;
    IOWorkLoop *loop=nullptr;
    IOTimerEventSource *timer=nullptr;
    thread_call_t notifier=nullptr;
    const OSSymbol *clockFunction=nullptr, *phyFunction=nullptr, *connectorFunction=nullptr, *interconnectFunction=nullptr;
    IOMemoryMap *map=nullptr, *scratchMap=nullptr, *bypassMap=nullptr;
    USB::MMIO *io=nullptr, *scratchIO=nullptr, *bypassRegisters=nullptr;
    SMMU::BypassHandoff bypassProof{};
    bool bypassRecorded=false,bypassConfigured=false;
    SMMU::ContextHandoff physicalSnapshot{};
    bool physicalConfigured=false,physicalRecorded=false;
    USB::ControllerResources<State> clocks{*this};
    USB::DirectDMA<State> directDMA{*this};
    UInt64 serial=0, lease=0, noticeLease=0, connectorLease=0, interconnectLease=0, phyLease[2]{};
    bool connectorOpen=false, interconnectOpen=false, phyOpen[2]{}, timerAdded=false, stopping=false, releasing=false, notifying=false;
    UInt32 targetHz=0, addressBits=0, maximumSpeed=0, sourceMicroamps=0;
    bool directConfigured=false, ownedConfigured=false, ownedReady=false, isolatedDMA=false, suspended=false, bootProfile=false;
    USB::Orientation orientation=USB::Orientation::Unknown;
    USB::Role phyRole[2]{USB::Role::Device,USB::Role::Device};
    USB::PlatformContract contract{};
    struct BypassAccess {
        State *owner;
        QcomPower::Status accessible() {return smmuStatus(owner->dmaAccessible());}
        uint32_t read(uint32_t offset) {return owner->bypassRegisters ? owner->bypassRegisters->read(offset):UINT32_MAX;}
        uint64_t read64(uint32_t offset) {return owner->bypassRegisters ? owner->bypassRegisters->read64(offset):UINT64_MAX;}
    } bypassAccess{this};
    explicit State(QcomUSBResources *service, IOService *nub):self(service),provider(nub),bus(nub->getProvider()) {
        provider->retain(); if (bus) bus->retain(); lock=IOLockAlloc(); noticeLock=IOSimpleLockAlloc();
        clockFunction=OSSymbol::withCStringNoCopy(ClockController::Function);
        phyFunction=OSSymbol::withCStringNoCopy(USBPHY::Function);
        connectorFunction=OSSymbol::withCStringNoCopy(USBConnector::Function);
        interconnectFunction=OSSymbol::withCStringNoCopy(USBInterconnect::Function);
    }
    USB::Result clockRequest(ClockController::Controller controller,ClockController::Operation op, UInt32 id, UInt64 &value) {
        auto r=ClockController::targeted(controller,op,id,value,ClockController::USBControllerConsumer);
        const auto status=bus->callPlatformFunction(clockFunction,false,&r.request,nullptr,nullptr,nullptr);
        if (status!=kIOReturnSuccess && !self->getProperty("USBClockFailure")) {
            UInt64 record[]={controller,op,id,value,r.request.value,static_cast<UInt32>(status)};
            self->setProperty("USBClockFailure",record,sizeof(record));
        }
        if (r.request.version!=2 || r.request.size!=sizeof(r) || !ClockController::valid(r.request) || r.controller!=controller ||
            r.consumer!=ClockController::USBControllerConsumer || r.request.operation!=op || r.request.id!=id || r.flags)
            return USB::Result::HardwareError; // a write may already have reached hardware
        value=r.request.value; return result(status);
    }
    USB::Result request(ClockController::Operation op,UInt32 id,UInt64 &value) {
        return clockRequest(ClockController::GCC,op,id,value);
    }
    bool bootStopped() {return QcomUSBMapper::bootConfiguration(provider);}
    USB::Result prepareClocks(UInt32 coreHz) {
        if(!bootProfile)return USB::Result::Success;
        if(!ownedConfigured && !bypassConfigured && !physicalConfigured)return USB::Result::Unsupported;
        return USB::prepareControllerBootResources(*this,coreHz);
    }
    USB::Result halt() {
        if (!io || io->failed()) return USB::Result::NotReady;
        // The other QcomUSB object can have changed PRTCAPDIR since Acquire.
        // Probe a fresh view on every isolation attempt.
        DWC3::Core<USB::MMIO> core(*io);
        auto r=core.probe(map->getLength(),addressBits);
        if (r==USB::Result::Success) r=core.halt();
        return io->failed() ? USB::Result::HardwareError : r;
    }
    USB::Result stopped() {
        if (clocks.inReset()) return USB::Result::Success;
        if (!io || io->failed()) return USB::Result::NotReady;
        const auto gctl=io->read(DWC3::GCTL);
        if (gctl==UINT32_MAX) return USB::Result::HardwareError;
        if ((gctl&DWC3::PortMask)==DWC3::PortDevice) {
            const auto ctl=io->read(DWC3::DCTL), status=io->read(DWC3::DSTS);
            if (ctl==UINT32_MAX || status==UINT32_MAX) return USB::Result::HardwareError;
            return !(ctl&DWC3::Run) && (status&DWC3::Halted) ? USB::Result::Success : USB::Result::Busy;
        }
        if ((gctl&DWC3::PortMask)!=DWC3::PortHost) return USB::Result::Unsupported;
        const auto cap=io->read(0); const unsigned op=cap&255;
        if (cap==UINT32_MAX || op<0x20 || op>0x80 || (op&3)) return USB::Result::HardwareError;
        const auto command=io->read(op), status=io->read(op+4);
        if (command==UINT32_MAX || status==UINT32_MAX) return USB::Result::HardwareError;
        return !(command&1) && (status&1) ? USB::Result::Success : USB::Result::Busy;
    }
    USB::Result connectorCall(USBConnector::Operation op, USB::ConnectorState *sample=nullptr,
                             USB::Role next=USB::Role::None, const USB::ConnectorState *target=nullptr, bool source=false) {
        if (!connector || !connectorOpen) return USB::Result::NotReady;
        USBConnector::Request r; r.operation=op; r.lease=connectorLease; r.next=next; r.source=source;
        if (target) r.connector=*target;
        const auto status=connector->callPlatformFunction(connectorFunction,false,&r,self,nullptr,nullptr);
        if (op==USBConnector::Operation::Acquire) {
            const auto irq=IOSimpleLockLockDisableInterrupt(noticeLock);
            connectorLease=r.lease; IOSimpleLockUnlockEnableInterrupt(noticeLock,irq);
        }
        if (r.version!=USBConnector::Version || r.size!=sizeof(r) || r.operation!=op || r.next!=next ||
            r.reserved || r.budgetReserved || !USBConnector::validBudget(r.sourceMicroamps) ||
            (op!=USBConnector::Operation::Acquire && r.sourceMicroamps) ||
            (op==USBConnector::Operation::Acquire && status!=kIOReturnSuccess && r.sourceMicroamps) ||
            !r.connector.valid() || r.source!=source || (op!=USBConnector::Operation::Acquire && r.lease!=connectorLease) ||
            (op==USBConnector::Operation::Acquire && status==kIOReturnSuccess && !r.lease)) return USB::Result::Invalid;
        for (auto byte:r.padding) if (byte) return USB::Result::Invalid;
        if(op==USBConnector::Operation::Acquire && status==kIOReturnSuccess) {
            sourceMicroamps=r.sourceMicroamps;
            if(maximumSpeed==4 && sourceMicroamps && sourceMicroamps<900000)return USB::Result::Invalid;
        }
        if (op!=USBConnector::Operation::Acquire && op!=USBConnector::Operation::Sample && target &&
            (!r.connector.sameRoute(*target) || r.connector.vbus!=target->vbus || r.connector.overCurrent!=target->overCurrent))
            return USB::Result::Invalid;
        if (status==kIOReturnSuccess && sample) {
            if (!r.connector.valid()) return USB::Result::Invalid;
            *sample=r.connector;
        }
        if (op==USBConnector::Operation::Release && status==kIOReturnSuccess) {
            const auto irq=IOSimpleLockLockDisableInterrupt(noticeLock);
            connectorLease=0; IOSimpleLockUnlockEnableInterrupt(noticeLock,irq);
            sourceMicroamps=0;
        }
        return result(status);
    }
    USB::Result noc(USBInterconnect::Action action) {
        if (!interconnect) return USB::Result::NotReady;
        USBInterconnect::Request r; r.action=action; r.lease=interconnectLease;
        if (action==USBInterconnect::Action::Acquire) {
            r.averageBytesPerSecond=240000000; r.peakBytesPerSecond=800000000;
        }
        const auto status=interconnect->callPlatformFunction(interconnectFunction,false,&r,self,nullptr,nullptr);
        if (action==USBInterconnect::Action::Acquire) interconnectLease=r.lease;
        if (r.version!=1 || r.size!=sizeof(r) || r.action!=action || r.reserved || r.padding || (r.flags&~3u) ||
            (action!=USBInterconnect::Action::Acquire && r.lease!=interconnectLease)) return USB::Result::Invalid;
        if (action==USBInterconnect::Action::Acquire ?
            (r.averageBytesPerSecond!=240000000 || r.peakBytesPerSecond!=800000000) :
            (r.averageBytesPerSecond || r.peakBytesPerSecond)) return USB::Result::Invalid;
        if (status==kIOReturnSuccess && (action==USBInterconnect::Action::Acquire || action==USBInterconnect::Action::Check) &&
            (!r.lease || (r.flags&3)!=3)) return USB::Result::Invalid;
        if (status==kIOReturnSuccess && action==USBInterconnect::Action::Query && !(r.flags&USBInterconnect::Supported))
            return USB::Result::Unsupported;
        if (status==kIOReturnSuccess && action==USBInterconnect::Action::Release) interconnectLease=0;
        return result(status);
    }
    USB::Result phy(unsigned index, USBPHY::Operation op, USB::Orientation direction=USB::Orientation::Normal,
                    USB::Role next=USB::Role::Device) {
        if (!phys[index] || !phyOpen[index]) return USB::Result::NotReady;
        USBPHY::Request r; r.operation=op; r.lease=phyLease[index]; r.orientation=unsigned(direction);
        r.role=unsigned(op==USBPHY::Operation::Enable ? next : phyRole[index]);
        const auto role=r.role;
        const auto status=phys[index]->callPlatformFunction(phyFunction,false,&r,self,nullptr,nullptr);
        if (op==USBPHY::Operation::Enable) { phyLease[index]=r.lease;phyRole[index]=next; }
        if (r.version!=USBPHY::Version || r.size!=sizeof(r) || r.operation!=op || r.reserved || r.role!=role || r.orientation!=unsigned(direction) ||
            (op!=USBPHY::Operation::Enable && r.lease!=phyLease[index]) ||
            (op==USBPHY::Operation::Enable && status==kIOReturnSuccess && !r.lease)) return USB::Result::Invalid;
        if (op==USBPHY::Operation::Disable && status==kIOReturnSuccess) phyLease[index]=0;
        return result(status);
    }
    USB::Result readDMAWindow(USB::DirectDMAWindow &window) {
        if (provider->getProperty("iommu-parent")) return USB::Result::Unsupported;
        auto *object=provider->copyProperty(USB::DirectDMAProperty);
        auto *data=OSDynamicCast(OSData,object);
        const bool valid=data && USB::DirectDMAWindow::decode(data->getBytesNoCopy(),data->getLength(),window);
        OSSafeReleaseNULL(object);
        return valid ? USB::Result::Success : USB::Result::NotReady;
    }
    USB::Result dmaAccessible() { return noc(USBInterconnect::Action::Check); }
    USB::Result dmaRoute() {
        if(physicalConfigured) {
            if(!physicalRecorded || !bypassRegisters || bypassRegisters->failed() ||
                !QcomUSBMapper::firmwarePhysicalConfiguration(provider,addressBits) ||
                !physicalSnapshot.registersUnchanged(bypassAccess)) return USB::Result::HardwareError;
            const auto fault=bypassRegisters->read(0x48);
            if(fault) {
                self->setProperty("USBDMAPhysicalGlobalFault",fault,32);
                return USB::Result::HardwareError;
            }
            return bypassRegisters->failed() ? USB::Result::HardwareError:USB::Result::Success;
        }
        if(!bypassConfigured)return USB::Result::Success;
        SMMU::BypassHandoff current;
        if(!bypassRecorded || !bypassRegisters || bypassRegisters->failed() ||
            !QcomUSBMapper::bypassConfiguration(provider,addressBits,current) || !bypassProof.same(current))return USB::Result::HardwareError;
        return bypassProof.unchanged(bypassAccess) && !bypassRegisters->failed() ? USB::Result::Success:USB::Result::HardwareError;
    }
    USB::Result prepareFirmwarePhysical() {
        if(!physicalRecorded) {
            const auto status=QcomUSBMapper::captureFirmwarePhysical(mapperResources(),provider,addressBits,physicalSnapshot);
            if(status!=kIOReturnSuccess)return result(status);
            physicalRecorded=true;
        }
        if(!bypassMap) {
            bypassMap=provider->mapDeviceMemoryWithIndex(2,kIOMapInhibitCache);
            if(!bypassMap || !bypassMap->getVirtualAddress() || bypassMap->getPhysicalAddress()!=physicalSnapshot.smmu ||
                bypassMap->getLength()!=physicalSnapshot.aperture)return USB::Result::NotReady;
            bypassRegisters=new USB::MMIO(reinterpret_cast<void *>(bypassMap->getVirtualAddress()),bypassMap->getLength());
            if(!bypassRegisters)return USB::Result::NotReady;
        }
        auto r=dmaRoute();if(r!=USB::Result::Success)return r;
        USB::DirectDMAWindow window{1,32,0,0,uint64_t(1)<<32};
        if(!provider->setProperty(USB::DirectDMAProperty,&window,sizeof(window)))return USB::Result::NotReady;
        self->setProperty("USBDMAValidation","FirmwarePhysical32Trial");
        return USB::Result::Success;
    }
    QcomUSBMapper::Resources mapperResources() {
        QcomUSBMapper::Resources resources;
        resources.owner=self;resources.loop=loop;
        resources.accessible=[](IOService *service) {return smmuStatus(static_cast<QcomUSBResources *>(service)->fState->dmaAccessible());};
        resources.isolated=[](IOService *service) {return smmuStatus(static_cast<QcomUSBResources *>(service)->fState->dmaIsolated());};
        resources.fault=[](IOService *service) {
            auto *state=static_cast<QcomUSBResources *>(service)->fState;
            if(state && state->notifier)thread_call_enter(state->notifier);
        };
        return resources;
    }
    USB::Result prepareBypass() {
        if(provider->getProperty(USB::DirectDMACaptureProperty)) {
            const auto status=QcomUSBMapper::captureBypass(mapperResources(),provider,addressBits);
            if(status!=kIOReturnSuccess)return result(status);
        }
        SMMU::BypassHandoff current;
        if(!QcomUSBMapper::bypassConfiguration(provider,addressBits,current) ||
            (bypassRecorded && !bypassProof.same(current)))return USB::Result::HardwareError;
        if(!bypassRecorded) {bypassProof=current;bypassRecorded=true;}
        if(!bypassMap) {
            bypassMap=provider->mapDeviceMemoryWithIndex(2,kIOMapInhibitCache);
            if(!bypassMap || !bypassMap->getVirtualAddress() || bypassMap->getPhysicalAddress()!=current.smmu ||
                bypassMap->getLength()!=current.aperture)return USB::Result::NotReady;
            bypassRegisters=new USB::MMIO(reinterpret_cast<void *>(bypassMap->getVirtualAddress()),bypassMap->getLength());
            if(!bypassRegisters)return USB::Result::NotReady;
        }
        return dmaRoute();
    }
    USB::Result dmaIsolated() {
        UInt64 value=0;
        const auto r=request(ClockController::GetResetState,18,value);
        if (r!=USB::Result::Success) return r;
        return value==1 ? USB::Result::Success : value ? USB::Result::HardwareError : USB::Result::Busy;
    }
    bool mapperCurrent() const {
        return mapper && !provider->getProperty(USB::DirectDMAProperty) &&
            provider->getProperty("iommu-parent")==mapper && !mapper->isInactive() &&
            (!ownedMapper || (isolatedDMA ? ownedMapper->isolated():ownedMapper->healthy()));
    }
    bool dmaCurrent() {
        return directConfigured ? directDMA.check()==USB::Result::Success : mapperCurrent();
    }
    bool dependencies() {
        if (!bus) return false;
        if (reinterpret_cast<uintptr_t>(IOMapper::gSystem)&3) {
            self->setProperty("USBResourceWait","SystemMapperDecision");return false;
        }
        const bool bypassCapture=provider->getProperty(USB::DirectDMACaptureProperty)!=nullptr;
        const bool bypass=bypassCapture || provider->getProperty(SMMU::BypassHandoffProperty)!=nullptr;
        const bool physical=provider->getProperty(USB::FirmwarePhysical32Property)!=nullptr;
        const bool direct=physical || bypass || provider->getProperty(USB::DirectDMAProperty)!=nullptr;
        const bool capture=provider->getProperty(SMMU::ContextCaptureProperty)!=nullptr;
        const bool owned=capture || provider->getProperty("qcom,usb-smmu-handoff")!=nullptr;
        if ((direct && owned) || (physical && bypass) ||
            (physical && !QcomUSBMapper::firmwarePhysicalConfiguration(provider,addressBits)) ||
            (lease && (direct!=directConfigured || owned!=ownedConfigured || bypass!=bypassConfigured || physical!=physicalConfigured))) return false;
        if(bypass) {
            SMMU::BypassHandoff proof;
            if(bypassCapture ? !QcomUSBMapper::bypassCaptureConfiguration(provider,addressBits) :
                (!QcomUSBMapper::bypassConfiguration(provider,addressBits,proof) || (bypassRecorded && !bypassProof.same(proof)))) {
                self->setProperty("USBResourceWait","SMMUBypassProof");return false;
            }
        }
        if (owned) {
            SMMU::ContextHandoff handoff{};
            if ((capture ? (ownedMapper || !QcomUSBMapper::captureConfiguration(provider,addressBits)) :
                    !QcomUSBMapper::configuration(provider,addressBits,handoff)) ||
                (ownedMapper && !ownedMapper->matchesHandoff(handoff)) ||
                (provider->getProperty("iommu-parent") && provider->getProperty("iommu-parent")!=ownedMapper)) {
                self->setProperty("USBResourceWait","SMMUHandoff");return false;
            }
        } else if (mapper && !mapperCurrent()) {
            // An unused, pre-publication mapper may be replaced by its real
            // producer. An active lease must retain its original translator.
            if (lease) return false;
            OSSafeReleaseNULL(mapper);
        }
        if (direct && !bypassCapture && !physical) {
            USB::DirectDMAWindow window{};
            if (readDMAWindow(window)!=USB::Result::Success || !window.valid(addressBits)) {
                self->setProperty("USBResourceWait","DirectDMATopology");return false;
            }
        }
        if (!direct && !owned && !mapper) {
            auto *object=provider->copyProperty("iommu-parent");
            mapper=OSDynamicCast(IOMapper,object);
            if (!mapper) { OSSafeReleaseNULL(object); self->setProperty("USBResourceWait","DeviceIOMapper"); return false; }
        }
        if (!direct && !owned && !mapperCurrent()) return false;
        directConfigured=direct;ownedConfigured=owned;bypassConfigured=bypass;physicalConfigured=physical;
        self->setProperty("USBDMAOwnershipMode",physical ? "FirmwarePhysical32Trial":bypass ? "VerifiedSMMUBypass":direct ? "ExplicitPhysicalWindow" : owned ? "OwnedSMMUContext":"PerDeviceIOMapper");
        if (!phys[0]) phys[0]=copyPHY(bus,0x0c012000);
        if (maximumSpeed==4 && !phys[1]) phys[1]=copyPHY(bus,0x0c010000);
        if (!phys[0] || (maximumSpeed==4 && !phys[1])) { self->setProperty("USBResourceWait","CalibratedPHYServices"); return false; }
        if (!connector) {
            auto *object=IOService::getResourceService()->copyProperty(USBConnector::Resource);
            connector=OSDynamicCast(IOService,object);
            if (!connector) { OSSafeReleaseNULL(object); return false; }
        }
        if (!below(connector,bus) || connector->getProperty("USBConnectorPowerReady")!=kOSBooleanTrue) return false;
        if (!interconnect) {
            auto *object=IOService::getResourceService()->copyProperty(USBInterconnect::Resource);
            interconnect=OSDynamicCast(IOService,object);
            if (!interconnect) { OSSafeReleaseNULL(object); self->setProperty("USBResourceWait","InterconnectProvider"); return false; }
        }
        if (!below(interconnect,bus) || noc(USBInterconnect::Action::Query)!=USB::Result::Success) return false;
        UInt64 caps=0;
        const auto r=request(ClockController::GetCapabilities,0,caps);
        const UInt64 required=ClockController::Clocks|ClockController::Domains|ClockController::Resets|ClockController::ClockHandoff;
        return r==USB::Result::Success && (caps&required)==required;
    }
    USB::Result acquire() {
        self->setProperty("USBResourceAcquireStage","Dependencies");
        if (!dependencies()) return USB::Result::NotReady;
        self->setProperty("USBResourceAcquireStage","ConnectorOpen");
        if (!connector->open(self)) return USB::Result::Busy;
        connectorOpen=true;
        self->setProperty("USBResourceAcquireStage","ConnectorConfigure");
        USB::ConnectorState signal{};
        auto r=connectorCall(USBConnector::Operation::Acquire,&signal); if (r!=USB::Result::Success) return r;
        auto *memory=provider->getDeviceMemoryWithIndex(0);
        auto *scratch=provider->getDeviceMemoryWithIndex(1);
        if (!memory || memory->getPhysicalAddress()!=0x0a800000 || memory->getLength()!=DWC3::Aperture ||
            !scratch || scratch->getPhysicalAddress()!=0x0a8f8800 || scratch->getLength()!=0x400 ||
            provider->getDeviceMemoryCount()!=((ownedConfigured || bypassConfigured || physicalConfigured) ? 3u:2u)) return USB::Result::Invalid;
        map=provider->mapDeviceMemoryWithIndex(0,kIOMapInhibitCache);
        scratchMap=provider->mapDeviceMemoryWithIndex(1,kIOMapInhibitCache);
        if (!map || !map->getVirtualAddress() || !scratchMap || !scratchMap->getVirtualAddress()) return USB::Result::NotReady;
        io=new USB::MMIO(reinterpret_cast<void *>(map->getVirtualAddress()),map->getLength());
        scratchIO=new USB::MMIO(reinterpret_cast<void *>(scratchMap->getVirtualAddress()),scratchMap->getLength());
        if (!io || !scratchIO) return USB::Result::NotReady;
        self->setProperty("USBResourceAcquireStage","ControllerClocks");
        r=clocks.acquire(targetHz); if (r!=USB::Result::Success) return r;
        if (!interconnect->open(self)) return USB::Result::Busy;
        interconnectOpen=true;
        self->setProperty("USBResourceAcquireStage","Interconnect");
        r=noc(USBInterconnect::Action::Acquire); if (r!=USB::Result::Success) return r;
        self->setProperty("USBResourceAcquireStage","DMA");
        if(physicalConfigured) {r=prepareFirmwarePhysical();if(r!=USB::Result::Success)return r;}
        if(bypassConfigured) {r=prepareBypass();if(r!=USB::Result::Success)return r;}
        if (directConfigured) { r=directDMA.acquire(addressBits);if (r!=USB::Result::Success) return r; }
        if (ownedConfigured) {
            if (!ownedMapper) {
                const auto status=QcomUSBMapper::create(mapperResources(),provider,addressBits,ownedMapper);
                if (status!=kIOReturnSuccess) return USB::Result::HardwareError;
                ownedReady=true;
            } else if (!ownedReady || !ownedMapper->activate()) return USB::Result::HardwareError;
            if (!mapper) {mapper=ownedMapper;mapper->retain();}
            isolatedDMA=false;
        }
        orientation=signal.orientation==USB::Orientation::Unknown ? USB::Orientation::Normal : signal.orientation;
        self->setProperty("USBResourceAcquireStage","PHY");
        for (unsigned i=0;i<(maximumSpeed==4 ? 2u:1u);++i) {
            if (!phys[i]->open(self)) return USB::Result::Busy;
            phyOpen[i]=true;
            r=phy(i,USBPHY::Operation::Enable,i ? orientation : USB::Orientation::Normal);
            if (r!=USB::Result::Success) return r;
        }
        self->setProperty("USBResourceAcquireStage","ControllerActivation");
        r=clocks.activate(); if (r!=USB::Result::Success) return r;
        r=DWC3::setPipeClock(*scratchIO,maximumSpeed==3); if (r!=USB::Result::Success) return r;
        // BCR has erased the firmware rings. Establish a non-running Device
        // view before QcomUSB probes, without enabling endpoints or events.
        const auto ctl=io->read(DWC3::DCTL), gctl=io->read(DWC3::GCTL);
        if (ctl==UINT32_MAX || gctl==UINT32_MAX) return USB::Result::HardwareError;
        io->write(DWC3::DEVTEN,0); io->write(DWC3::DCTL,ctl&~DWC3::Run);
        io->write(DWC3::GCTL,(gctl&~DWC3::PortMask)|DWC3::PortDevice);
        r=halt(); if (r!=USB::Result::Success) return r;
        if (!dmaCurrent()) return USB::Result::NotReady;
        contract={2,USB::PlatformContract::Clocks|USB::PlatformContract::Rails|USB::PlatformContract::PHY|
            USB::PlatformContract::Interconnect|USB::PlatformContract::Exclusive|
            (directConfigured ? USB::PlatformContract::DMABypass : 0u),clocks.coreHz(),19200000,addressBits,maximumSpeed,sourceMicroamps,0};
        self->setProperty("USBResourceAcquireStage","Ready");
        return contract.valid() ? USB::Result::Success : USB::Result::Invalid;
    }
    USB::Result release() {
        contract={}; // partial teardown is never a usable resource contract
        if (connectorLease) {
            auto r=connectorCall(USBConnector::Operation::Source); if (r!=USB::Result::Success) return r;
        }
        auto r=clocks.isolate(); if (r!=USB::Result::Success) return r;
        if (ownedMapper) {
            if (!ownedMapper->quiesce() || !ownedMapper->park()) return USB::Result::HardwareError;
            isolatedDMA=true;
            if (!ownedReady) {
                if (!ownedMapper->shutdown()) return USB::Result::HardwareError;
                OSSafeReleaseNULL(ownedMapper);
            }
        }
        r=directDMA.release();if (r!=USB::Result::Success) return r;
        for (unsigned i=2;i;--i) {
            if (phyLease[i-1]) { r=phy(i-1,USBPHY::Operation::Disable); if (r!=USB::Result::Success) return r; }
            if (phyOpen[i-1]) { phys[i-1]->close(self); phyOpen[i-1]=false; }
        }
        if (interconnectLease) { r=noc(USBInterconnect::Action::Release); if (r!=USB::Result::Success) return r; }
        if (interconnectOpen) { interconnect->close(self); interconnectOpen=false; }
        if (connectorLease) { r=connectorCall(USBConnector::Operation::Release); if (r!=USB::Result::Success) return r; }
        if (connectorOpen) { connector->close(self); connectorOpen=false; }
        r=clocks.release(); if (r!=USB::Result::Success) return r;
        delete io; io=nullptr; delete scratchIO; scratchIO=nullptr;
        delete bypassRegisters;bypassRegisters=nullptr;OSSafeReleaseNULL(bypassMap);
        OSSafeReleaseNULL(map); OSSafeReleaseNULL(scratchMap);
        OSSafeReleaseNULL(mapper);
        contract={}; orientation=USB::Orientation::Unknown;
        return USB::Result::Success;
    }
    USB::Result isolateDMA() {
        auto r=stopped();if (r!=USB::Result::Success) return r;
        if (!ownedMapper) return USB::Result::Success;
        r=clocks.isolate();if (r!=USB::Result::Success) return r;
        if (!ownedMapper->quiesce()) return USB::Result::HardwareError;
        isolatedDMA=true;return USB::Result::Success;
    }
    USB::Result target(const USB::PlatformRequest &request) {
        USB::ConnectorState current{};
        auto r=connectorCall(USBConnector::Operation::Sample,&current);
        if (r==USB::Result::NotReady || r==USB::Result::Busy || r==USB::Result::Unsupported) return USB::Result::Aborted;
        return r!=USB::Result::Success ? r : USB::platformTargetCurrent(request,current) ? USB::Result::Success : USB::Result::Aborted;
    }
    USB::Result prepare(const USB::PlatformRequest &request) {
        auto r=target(request); if (r!=USB::Result::Success) return r;
        r=noc(USBInterconnect::Action::Check); if (r!=USB::Result::Success) return r;
        r=stopped(); if (r!=USB::Result::Success) return r;
        if (request.next!=phyRole[0]) {
            // The previous Native controller has drained DMA before this
            // point. Recalibrate QUSB2 for the new data role with its own
            // board sequence; Type-C Source/Sink does not select that table.
            r=phy(0,USBPHY::Operation::Disable); if (r!=USB::Result::Success) return r;
            r=phy(0,USBPHY::Operation::Enable,USB::Orientation::Normal,request.next);
            if (r!=USB::Result::Success) return r;
        }
        if (maximumSpeed==4 && request.connector.orientation!=orientation) {
            r=phy(1,USBPHY::Operation::Disable); if (r!=USB::Result::Success) return r;
            r=phy(1,USBPHY::Operation::Enable,request.connector.orientation); if (r!=USB::Result::Success) return r;
            orientation=request.connector.orientation;
        }
        r=phy(0,USBPHY::Operation::Check); if (r!=USB::Result::Success) return r;
        if (maximumSpeed==4) { r=phy(1,USBPHY::Operation::Check); if (r!=USB::Result::Success) return r; }
        if (isolatedDMA) {
            if (!ownedMapper || !ownedMapper->activate()) return USB::Result::HardwareError;
            r=clocks.activate();if (r!=USB::Result::Success) return r;
            r=DWC3::setPipeClock(*scratchIO,maximumSpeed==3);if (r!=USB::Result::Success) return r;
            const auto ctl=io->read(DWC3::DCTL),gctl=io->read(DWC3::GCTL);
            if (ctl==UINT32_MAX || gctl==UINT32_MAX) return USB::Result::HardwareError;
            io->write(DWC3::DEVTEN,0);io->write(DWC3::DCTL,ctl&~DWC3::Run);
            io->write(DWC3::GCTL,(gctl&~DWC3::PortMask)|DWC3::PortDevice);
            r=halt();if (r!=USB::Result::Success) return r;
            isolatedDMA=false;
        }
        return target(request);
    }
};

bool QcomUSBResources::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    fStarted=true; fState=new State(this,provider);
    if (!fState || !fState->lock || !fState->noticeLock || !fState->bus || !fState->clockFunction ||
        !fState->phyFunction || !fState->connectorFunction || !fState->interconnectFunction) goto fail;
    {
        auto &s=*fState;
        auto *data=OSDynamicCast(OSData,provider->getProperty("qcom,usb-controller-resources"));
        if (!data || data->getLength()!=12) goto fail;
        const auto *p=static_cast<const UInt8 *>(data->getBytesNoCopy());
        s.targetHz=cell(p); s.addressBits=cell(p+4); s.maximumSpeed=cell(p+8);
        if(auto *object=provider->getProperty(USB::ControllerBootProperty)) {
            auto *marker=OSDynamicCast(OSData,object);
            if(!marker || marker->getLength()!=4 || cell(static_cast<const UInt8 *>(marker->getBytesNoCopy()))!=1)goto fail;
            s.bootProfile=true;
        }
        if ((s.targetHz!=60000000 && s.targetHz!=120000000 && s.targetHz!=150000000) ||
            s.addressBits<32 || s.addressBits>40 || (s.maximumSpeed!=3 && s.maximumSpeed!=4) ||
            provider->getResources()!=kIOReturnSuccess) goto fail;
        s.loop=IOWorkLoop::workLoop(); s.timer=IOTimerEventSource::timerEventSource(this,poll);
        s.notifier=thread_call_allocate_with_options(notify,this,THREAD_CALL_PRIORITY_KERNEL,THREAD_CALL_OPTIONS_ONCE);
        if (!s.loop || !s.timer || !s.notifier || s.loop->addEventSource(s.timer)!=kIOReturnSuccess) goto fail;
        s.timerAdded=true;
        setProperty("USBColdSuspendSupported",true);
        setProperty("USBDMAOwnershipMode","AwaitingExplicitTopology");
        registerService(); s.timer->setTimeoutMS(1); return true;
    }
fail:stop(provider); return false;
}
void QcomUSBResources::poll(OSObject *owner, IOTimerEventSource *timer) {
    auto *self=static_cast<QcomUSBResources *>(owner); auto &s=*self->fState;
    bool rematch=false;
    IOLockLock(s.lock);
    if (!s.stopping && !self->fPublished) {
        if (s.dependencies()) {
            self->publishResource(USB::PlatformResource,self); self->fPublished=true;
            self->setProperty("USBResourceWait","OwnerAcquire");
            rematch=true;
        } else timer->setTimeoutMS(1000);
    }
    IOLockUnlock(s.lock);
    if (rematch) s.provider->registerService();
}
void QcomUSBResources::notify(void *argument, void *) {
    auto *self=static_cast<QcomUSBResources *>(argument); auto &s=*self->fState;
    const auto irq=IOSimpleLockLockDisableInterrupt(s.noticeLock);
    auto *owner=s.noticeOwner; const auto lease=s.noticeLease;
    if (owner) s.notifying=true;
    IOSimpleLockUnlockEnableInterrupt(s.noticeLock,irq);
    if (!owner) return;
    USB::ConnectorNotice notice{}; notice.lease=lease;
    owner->message(iokit_vendor_specific_msg(USB::ConnectorChanged),self,&notice);
    const auto done=IOSimpleLockLockDisableInterrupt(s.noticeLock);
    s.notifying=false; IOSimpleLockUnlockEnableInterrupt(s.noticeLock,done);
}
IOReturn QcomUSBResources::message(UInt32 type, IOService *provider, void *argument) {
    if (type!=iokit_vendor_specific_msg(USBConnector::Changed)) return IOService::message(type,provider,argument);
    if (!argument || !fState || provider!=fState->connector) return kIOReturnBadArgument;
    auto &s=*fState; const auto &notice=*static_cast<const USBConnector::Notice *>(argument);
    if (notice.version!=USBConnector::Version || notice.size!=sizeof(notice)) return kIOReturnBadArgument;
    const auto irq=IOSimpleLockLockDisableInterrupt(s.noticeLock);
    const bool valid=notice.lease && notice.lease==s.connectorLease;
    if (valid) thread_call_enter(s.notifier);
    IOSimpleLockUnlockEnableInterrupt(s.noticeLock,irq);
    return valid ? kIOReturnSuccess : kIOReturnAborted;
}
void QcomUSBResources::pin() {
    if (!fPinned) { retain(); fPinned=true; }
    setProperty("USBResourceFault",true);
    if (fState && fState->notifier) thread_call_enter(fState->notifier);
}
IOWorkLoop *QcomUSBResources::getWorkLoop() const { return fState ? fState->loop : nullptr; }
IOReturn QcomUSBResources::callPlatformFunction(const OSSymbol *name, bool wait, void *a, void *b, void *c, void *d) {
    if (!name || !name->isEqualTo(USB::PlatformFunction)) return IOService::callPlatformFunction(name,wait,a,b,c,d);
    auto *r=static_cast<USB::PlatformRequest *>(a); auto *owner=static_cast<IOService *>(b);
    if (!r || !owner || c || d || !USB::validPlatformRequest(*r) || ml_at_interrupt_context() || !fState) return kIOReturnBadArgument;
    auto &s=*fState; IOLockLock(s.lock);
    USB::Result status=USB::Result::NotReady;
    IOService *released=nullptr; bool unpin=false;
    const bool shutdown=r->action==USB::PlatformAction::Release || r->action==USB::PlatformAction::Suspend || r->action==USB::PlatformAction::Isolate ||
        (r->action==USB::PlatformAction::Source && !r->source) ||
        (r->action==USB::PlatformAction::SafePower && r->next==USB::Role::None);
    if ((s.stopping || fPinned) && !shutdown) goto out;
    if (r->action==USB::PlatformAction::Acquire) {
        if (s.lease || s.releasing || owner->getProvider()!=s.provider || !s.provider->isOpen(owner)) { status=USB::Result::Busy; goto out; }
        if (s.serial==UINT64_MAX) { status=USB::Result::Overflow; goto out; }
        s.lease=++s.serial; s.owner=owner; owner->retain(); r->lease=s.lease;
        {
            const auto irq=IOSimpleLockLockDisableInterrupt(s.noticeLock);
            s.noticeOwner=owner; s.noticeLease=s.lease;
            IOSimpleLockUnlockEnableInterrupt(s.noticeLock,irq);
        }
        status=s.acquire();
        setProperty("USBResourceAcquireStatus",static_cast<UInt32>(native(status)),32);
        if (status==USB::Result::Success) r->resources=s.contract;
        goto out;
    }
    if (!s.lease || s.owner!=owner || r->lease!=s.lease || s.releasing) { status=USB::Result::Invalid; goto out; }
    if (r->action==USB::PlatformAction::Release || r->action==USB::PlatformAction::Suspend) {
        const bool suspend=r->action==USB::PlatformAction::Suspend;
        if (suspend && s.suspended) { status=USB::Result::Success;goto out; }
        if (suspend) {
            // The owner has already drained Native objects and DMA mappings.
            // Independently refuse to remove clocks from a running core.
            status=s.stopped();if (status!=USB::Result::Success) goto out;
        }
        {
            const auto irq=IOSimpleLockLockDisableInterrupt(s.noticeLock);
            const bool active=s.notifying;
            if (!active) { s.noticeOwner=nullptr; s.noticeLease=0; s.releasing=true; }
            IOSimpleLockUnlockEnableInterrupt(s.noticeLock,irq);
            if (active) { status=USB::Result::Busy; goto out; }
        }
        status=s.release();
        IOLockUnlock(s.lock); thread_call_cancel_wait(s.notifier); IOLockLock(s.lock);
        s.releasing=false;
        if (status==USB::Result::Success) {
            s.suspended=suspend;
            if (!suspend) { released=s.owner; s.owner=nullptr; s.lease=0; }
            if (fPinned) { fPinned=false; unpin=true; setProperty("USBResourceFault",false); }
        } else {
            const auto irq=IOSimpleLockLockDisableInterrupt(s.noticeLock);
            s.noticeOwner=s.owner; s.noticeLease=s.lease;
            IOSimpleLockUnlockEnableInterrupt(s.noticeLock,irq); pin();
        }
    } else if (r->action==USB::PlatformAction::Isolate) {
        status=s.suspended ? USB::Result::Success:s.isolateDMA();
        if (status!=USB::Result::Success) pin();
    } else if (r->action==USB::PlatformAction::Resume) {
        if (!s.suspended) {
            status=s.contract.valid() && s.dmaCurrent() ? USB::Result::Success : USB::Result::NotReady;
        } else if (!s.dependencies()) {
            // PM parents may still be restoring the connector. No acquire
            // has started, so this result can safely be retried by the owner.
            status=USB::Result::NotReady;
        } else {
            status=s.acquire();
            if (status==USB::Result::Success) {
                s.suspended=false;
                const auto irq=IOSimpleLockLockDisableInterrupt(s.noticeLock);
                s.noticeOwner=s.owner;s.noticeLease=s.lease;
                IOSimpleLockUnlockEnableInterrupt(s.noticeLock,irq);
            } else {
                // An acquisition may have obtained only part of the rails,
                // clocks or DMA lease. Never retry it as an empty acquisition.
                status=USB::Result::HardwareError;pin();
            }
        }
        if (status==USB::Result::Success) r->resources=s.contract;
    } else if (s.suspended && (r->action==USB::PlatformAction::Source || r->action==USB::PlatformAction::SafePower) &&
        !r->source && r->next==USB::Role::None) {
        status=USB::Result::Success;
    } else if (r->action==USB::PlatformAction::Source || r->action==USB::PlatformAction::SafePower) {
        const bool targeted=r->source || r->next!=USB::Role::None;
        status=targeted && (!s.contract.valid() || !s.dmaCurrent()) ?
            USB::Result::NotReady : targeted ? s.target(*r) : USB::Result::Success;
        if (targeted && status==USB::Result::Success) status=s.noc(USBInterconnect::Action::Check);
        if (r->source && status==USB::Result::Success) status=s.phy(0,USBPHY::Operation::Check);
        if (r->source && status==USB::Result::Success && s.maximumSpeed==4) status=s.phy(1,USBPHY::Operation::Check);
        if (status==USB::Result::Success) status=s.connectorCall(
            r->action==USB::PlatformAction::Source ? USBConnector::Operation::Source : USBConnector::Operation::SafePower,
            nullptr,r->next,&r->connector,r->source);
    } else if (!s.contract.valid() || !s.dmaCurrent()) {
        status=USB::Result::NotReady;
    } else if (r->action==USB::PlatformAction::Sample) {
        // Sampling also observes remote RPM close/reset. An existing nub
        // must lose admission when its interconnect grant becomes unknown.
        status=s.noc(USBInterconnect::Action::Check);
        if (status==USB::Result::Success) status=s.connectorCall(USBConnector::Operation::Sample,&r->connector);
    } else if (r->action==USB::PlatformAction::Prepare) {
        status=s.prepare(*r);
    } else if (r->action==USB::PlatformAction::Validate) {
        status=s.target(*r);
        if (status==USB::Result::Success) status=s.noc(USBInterconnect::Action::Check);
        if (status==USB::Result::Success) status=s.stopped();
        if (status==USB::Result::Success) status=s.phy(0,USBPHY::Operation::Check);
        if (status==USB::Result::Success && s.maximumSpeed==4) status=s.phy(1,USBPHY::Operation::Check);
        if (status==USB::Result::Success) status=s.target(*r);
    }
out:
    IOLockUnlock(s.lock);
    if (released) released->release();
    if (unpin) release();
    return native(status);
}
bool QcomUSBResources::requestTerminate(IOService *, IOOptionBits) {
    if (!fState || !fState->lock) return !fPinned;
    IOLockLock(fState->lock); const bool allowed=!fState->lease && !fState->releasing && !fPinned;
    IOLockUnlock(fState->lock); return allowed;
}
void QcomUSBResources::stop(IOService *provider) {
    if (fState && fState->lock) {
        auto &s=*fState; IOLockLock(s.lock); s.stopping=true;
        if (s.lease || s.releasing) { pin(); IOLockUnlock(s.lock); return; }
        if (s.ownedMapper) {
            if (!s.ownedMapper->shutdown()) {pin();IOLockUnlock(s.lock);return;}
            OSSafeReleaseNULL(s.mapper);OSSafeReleaseNULL(s.ownedMapper);
        }
        IOLockUnlock(s.lock);
        if (s.timer) s.timer->cancelTimeout();
        if (fPublished) {
            auto *resources=getResourceService();
            if (resources->getProperty(USB::PlatformResource)==this) resources->removeProperty(USB::PlatformResource);
            fPublished=false;
        }
        if (s.notifier) thread_call_cancel_wait(s.notifier);
        if (s.timerAdded) { s.loop->removeEventSource(s.timer); s.timerAdded=false; }
    }
    if (fStarted) { IOService::stop(provider); fStarted=false; }
}
void QcomUSBResources::free() {
    if (fState) {
        stop(fState->provider); if (fPinned) return;
        auto *s=fState; fState=nullptr;
        if (s->notifier) thread_call_free(s->notifier);
        delete s->io; delete s->scratchIO; OSSafeReleaseNULL(s->map); OSSafeReleaseNULL(s->scratchMap);
        for (auto *phy:s->phys) if (phy) phy->release();
        OSSafeReleaseNULL(s->connector); OSSafeReleaseNULL(s->interconnect); OSSafeReleaseNULL(s->mapper); OSSafeReleaseNULL(s->ownedMapper);
        OSSafeReleaseNULL(s->clockFunction); OSSafeReleaseNULL(s->phyFunction); OSSafeReleaseNULL(s->connectorFunction);
        OSSafeReleaseNULL(s->interconnectFunction);
        OSSafeReleaseNULL(s->timer); OSSafeReleaseNULL(s->loop); OSSafeReleaseNULL(s->provider); OSSafeReleaseNULL(s->bus);
        if (s->noticeLock) IOSimpleLockFree(s->noticeLock);
        if (s->lock) IOLockFree(s->lock);
        delete s;
    }
    IOService::free();
}
