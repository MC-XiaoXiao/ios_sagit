// SPDX-License-Identifier: BSD-2-Clause
#include "QcomUSBHost.h"
#include "QcomUSBHostPort.h"
#include "QcomUSB.h"
#include "QcomUSBHostDMACommand.h"
#include "USBIODMA.h"
#include "USBHostCurrent.h"
#include "XHCICore.h"
#include "XHCIEvents.h"
#include "XHCISlots.h"
#include "XHCIPipes.h"
#include "XHCIRequests.h"
#include "XHCIPower.h"
#include "XHCITTClear.h"
#include <IOKit/usb/IOUSBHostDevice.h>
#include <IOKit/usb/AppleUSBHostRequest.h>
#include <IOKit/usb/AppleUSBRequestCompleter.h>
#include <libkern/c++/OSNumber.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOInterrupts.h>
#include <kern/clock.h>

OSDefineMetaClassAndStructors(QcomUSBHost,AppleUSBHostController);

namespace {
constexpr IOReturn HostPipeStalled=IOReturn(0xe0005000);
IOReturn hostResult(USB::Result r) {
    switch (r) {
    case USB::Result::Success:return kIOReturnSuccess;
    case USB::Result::Busy:return kIOReturnBusy;
    case USB::Result::Timeout:return kIOReturnTimeout;
    case USB::Result::Invalid:return kIOReturnBadArgument;
    case USB::Result::Unsupported:return kIOReturnUnsupported;
    case USB::Result::NotReady:return kIOReturnNotReady;
    case USB::Result::Pending:return kIOReturnNotReady;
    case USB::Result::Aborted:return kIOReturnAborted;
    default:return kIOReturnIOError;
    }
}
UInt64 hostTimeUS() {
    UInt64 ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);return ns/1000;
}
}

struct QcomUSBHost::State {
    using Region=USB::DMARegion<USB::IOMemoryAPI>;
    static constexpr unsigned RingEntries=256, RingBytes=RingEntries*sizeof(XHCI::TRB);
    static constexpr unsigned CommandOffset=0, EventOffset=4096, DCBAAOffset=8192;
    static constexpr unsigned TableOffset=12288, ScratchOffset=12352, ArenaBytes=16384;
    static constexpr unsigned MaximumScratchpads=32;
    static constexpr unsigned MaximumIsoFrames=256,MaximumPipeIsoFrames=128;
    QcomUSBHost *owner;
    QcomUSBController *nub;
    USB::MMIO &io;
    XHCI::Core<USB::MMIO> core;
    XHCI::PortProtocols protocols;
    XHCI::ProducerRing<USB::MMIO> commands;
    XHCI::EventRing<USB::MMIO> events;
    struct CommandBackend {
        State &state;
        auto &ring() { return state.commands; }
        bool admitted(UInt64 generation) {
            return generation==state.epoch && !state.stopping && !state.fault && state.nub->accepts(state.owner,generation);
        }
        USB::Result doorbell() { return state.core.doorbell(0,0); }
        void retain(uintptr_t object) { reinterpret_cast<OSObject *>(object)->retain(); }
        USB::Result isolation(UInt64 generation) {
            return generation==state.epoch && state.dmaStopped ? USB::Result::Success : USB::Result::NotReady;
        }
    } commandBackend{*this};
    using Commands=XHCI::CommandQueue<CommandBackend>;
    Commands commandQueue{commandBackend};
    IOWorkLoop *loop=nullptr;
    IOCommandGate *gate=nullptr;
    IOFilterInterruptEventSource *irq=nullptr;
    IOTimerEventSource *timer=nullptr;
    thread_call_t releaseWork=nullptr,deviceWork=nullptr,requestWork=nullptr,commandReleaseWork=nullptr,ttWork=nullptr,closeWork=nullptr;
    // Include 255 devices, their 31 Native pipes, 64 retained commands and
    // the normal backlog when the release worker is joined during shutdown.
    OSObject *releaseObjects[256*32+128]{};
    unsigned releaseCount=0,controllerReleases=0,waiters=0,ttRecoveryCount=0;
    bool changedPorts[128]{};
    struct RuntimePort {
        XHCI::Port<XHCI::Core<USB::MMIO>> hardware;
        USB::Result result=USB::Result::NotReady;
        bool busy=false;
        explicit RuntimePort(XHCI::Core<USB::MMIO> &core) : hardware(core) {}
    } *ports[16]{};
    struct Device;
    struct Request;
    struct Pipe {
        USB::DMAOrdering order;
        XHCI::PipeMemory<USB::IOMemoryAPI,USB::DMAOrdering> memory;
        Device *device;
        XHCI::EndpointConfiguration configuration;
        IOUSBHostPipe *native=nullptr;
        StandardUSB::EndpointDescriptor descriptor;
        StandardUSB::SuperSpeedEndpointCompanionDescriptor companion;
        IOUSBHostPipe::StandardUSBDescriptors descriptors;
        unsigned id;
        struct TransferBackend {
            State &state;
            XHCI::ProducerRing<USB::DMAOrdering> *producer;
            auto &ring() { return *producer; }
            bool admitted(UInt64 generation) {
                return generation==state.epoch && state.running && !state.stopping && !state.fault &&
                    state.nub->accepts(state.owner,generation);
            }
            USB::Result doorbell(unsigned slot,unsigned endpoint) { return state.core.doorbell(slot,endpoint); }
            void retain(uintptr_t object) { reinterpret_cast<OSObject *>(object)->retain(); }
            USB::Result isolation(UInt64 generation) {
                return generation==state.epoch && state.dmaStopped ? USB::Result::Success : USB::Result::NotReady;
            }
        } backend;
        XHCI::TransferQueue<TransferBackend,Commands,MaximumPipeIsoFrames> transfers;
        Request *head=nullptr,*tail=nullptr,*active=nullptr,*ttRequest=nullptr;
        unsigned requests=0;
        unsigned isoFrames=0;
        XHCI::IsochronousFlow isoFlow;
        UInt64 stopTicket=0;
        XHCI::TTBufferCleanup ttCleanup;
        IOReturn isoRecoveryReason=kIOReturnAborted;
        bool isoRecovering=false,isoNeedsFreshContext=false;
        bool closing=false,closeFailed=false;
        bool workQueued=true,workRelease=false,workDone=false;
        USB::Result workResult=USB::Result::Pending;
        Pipe(State &state,USB::IOMemoryAPI &api,Device *parent,const IOUSBHostPipe::StandardUSBDescriptors &source,
            const XHCI::EndpointConfiguration &endpoint,XHCI::ProducerRing<USB::DMAOrdering> *control) :
            memory(api,order),device(parent),configuration(endpoint),
            descriptor(*source.descriptor),companion(source.ssCompanionDescriptor ? *source.ssCompanionDescriptor :
                StandardUSB::SuperSpeedEndpointCompanionDescriptor{}),
            descriptors{source.bcdUSB,&descriptor,source.ssCompanionDescriptor ? &companion : nullptr,nullptr},
            id(XHCI::endpointID(endpoint.address,endpoint.type)),backend{state,id==1 ? control : &memory.ring()},
            transfers(backend,state.commandQueue) {}
    };
    struct Device : XHCI::TopologyNode {
        USB::DMAOrdering order;
        XHCI::SlotMemory<USB::IOMemoryAPI,USB::DMAOrdering> memory;
        IOUSBHostDevice *native=nullptr;
        unsigned address=0;
        UInt32 endpoints=2;
        XHCI::DevicePause power;
        XHCI::TTEnumerationRecovery enumerationTT;
        Pipe *pipes[32]{};
        unsigned requests=0,maintenance=0;
        USB::Result preparation=USB::Result::Pending;
        bool prepared=false,prepareQueued=true,busy=true,cleanupFailed=false;
        explicit Device(USB::IOMemoryAPI &api,UInt32 identifier) : TopologyNode(identifier),memory(api,order) {}
    } *devices[256]{};
    Device *slotDevices[256]{};
    enum class RequestPhase { Queued,Preparing,Ready,Submitted,Isochronous,Recovering,TTRecovering,Completing,Enqueuing,Pinned };
    struct Request {
        XHCI::RequestMemory<USB::IOMemoryAPI> memory;
        Pipe *pipe;
        Request *next=nullptr;
        AppleUSBHostRequest *native;
        AppleUSBHostRequest *handoff=nullptr;
        AppleUSBRequestCompleter *completer;
        IOMemoryDescriptor *prepared;
        IOService *client;
        USB::SetupPacket setup{};
        IOUSBHostIsochronousTransaction *frameList=nullptr;
        XHCI::IsochronousProgress iso;
        XHCI::CommandRecord ttStop;
        UInt64 firstFrame=0;
        UInt64 deadline=0,stamp=0;
        UInt32 length=0;
        IOReturn status=kIOReturnSuccess;
        RequestPhase phase=RequestPhase::Queued;
        bool busy=false,cancelled=false,queueRetain=false,isochronous=false,ttHeld=false,ttPaused=false;
        bool invariantPinned=false;
        USB::CleanupRecovery cleanupRecovery;
        Request(USB::IOMemoryAPI &api,Pipe *source,AppleUSBHostRequest *request,
            AppleUSBRequestCompleter *completion,IOMemoryDescriptor *buffer,UInt32 bytes) :
            memory(api),pipe(source),native(request),completer(completion),prepared(buffer),
            client(request->getClient()),length(bytes) {}
    };
    enum class IsoFramePhase { Ready,Submitted,Completing,Pinned };
    struct IsoFrame {
        XHCI::RequestMemory<USB::IOMemoryAPI> memory;
        Request *request;
        UInt32 index;
        UInt64 target=0,stamp=0;
        IOReturn status=kIOReturnSuccess;
        IsoFramePhase phase=IsoFramePhase::Ready;
        bool queueRetain=false,usesSIA=false;
        USB::CleanupRecovery cleanupRecovery;
        IsoFrame(USB::IOMemoryAPI &api,Request *owner,UInt32 number) : memory(api),request(owner),index(number) {}
    } *isoFrames[MaximumIsoFrames]{};
    unsigned isoFrameCount=0,isoPreparationCursor=0;
    Request *requests[256]{};
    unsigned requestCount=0;
    USB::PlatformContract resources;
    USB::HostCurrent current;
    XHCI::ConnectorPorts connectorPorts;
    IOMapper *mapper=nullptr;
    USB::IOMemoryAPI *memory=nullptr;
    Region *arena=nullptr, *scratchpads[MaximumScratchpads]{};
    UInt64 epoch;
    UInt64 consumedFrameRollovers=0;
    bool opened=false, published=false, configured=false;
    bool gateAdded=false,irqAdded=false,timerAdded=false;
    bool running=false,stopping=false,fault=false,dmaStopped=false,portsComplete=false;
    IOReturn faultReason=kIOReturnSuccess;
    State(QcomUSBHost *controller,QcomUSBController *provider,USB::MMIO &registers) :
        owner(controller),nub(provider),io(registers),core(io),commands(io),events(io),
        resources(nub->resourceContract()),epoch(nub->epoch()) { nub->retain(); }
    ~State() { if (mapper) mapper->release();nub->release(); }

    template<class F> IOReturn gated(F function) {
        if (!gate) return kIOReturnNotReady;
        return gate->runAction([](OSObject *,void *argument,void *,void *,void *)->IOReturn {
            return (*static_cast<F *>(argument))();
        },&function);
    }

    template<class F> IOReturn hardware(F function) {
        // All state-changing MMIO uses this path. The provider's separately
        // revocable, IRQ-safe MFINDEX snapshot is the only read exception.
        auto callback=[](IOService *,USB::MMIO &,void *argument)->IOReturn {
            return (*static_cast<F *>(argument))();
        };
        return nub->runHardwareAction(owner,epoch,callback,&function);
    }
    IOReturn probe() {
        return hardware([&] {
            auto r=core.probe(0xc100,resources.addressBits);
            if (r!=USB::Result::Success) return hostResult(r);
            // Native locationID has one nibble for each root/hub port.
            if (core.scratchpadCount()>MaximumScratchpads || core.maxPorts()>15) return kIOReturnUnsupported;
            r=protocols.initialize(io,core.maxPorts());
            if (r==USB::Result::Success) r=connectorPorts.initialize(protocols,core.maxPorts(),resources.maximumSpeed);
            return hostResult(r);
        });
    }
    IOReturn reset() {
        auto result=hardware([&] { return hostResult(core.beginReset(hostTimeUS())); });
        if (result!=kIOReturnSuccess) return result;
        // This runs in the start/cleanup thread, outside all workloop gates.
        // Each timer step checks the provider generation again under its gate.
        do {
            IOSleep(1);
            result=hardware([&] { return hostResult(core.advanceReset(hostTimeUS())); });
        } while (result==kIOReturnBusy);
        return result;
    }
    IOReturn allocate() {
        // A 32-bit-only xHCI cannot use the wider platform DMA aperture.
        // Construct the memory policy after probing the controller width.
        resources.addressBits=core.dmaBits();
        mapper=nub->copyDeviceMapper();
        memory=new USB::IOMemoryAPI(mapper,resources);
        if (!memory) return kIOReturnNoMemory;
        for (unsigned n=1;n<=core.maxPorts();++n) {
            ports[n]=new RuntimePort(core);
            if (!ports[n]) return kIOReturnNoMemory;
            const auto r=ports[n]->hardware.initialize(n,protocols);
            if (r!=USB::Result::Success) return hostResult(r);
        }
        arena=new Region(*memory);
        if (!arena) return kIOReturnNoMemory;
        // Both ring segments fit in one 64 KiB window, even with physical DMA.
        auto r=arena->allocate(ArenaBytes,65536);
        if (r!=USB::Result::Success) return hostResult(r);
        auto *pointers=reinterpret_cast<UInt64 *>(static_cast<UInt8 *>(arena->bytes())+ScratchOffset);
        for (unsigned n=0;n<core.scratchpadCount();++n) {
            scratchpads[n]=new Region(*memory);
            if (!scratchpads[n]) return kIOReturnNoMemory;
            r=scratchpads[n]->allocate(4096,4096);
            if (r!=USB::Result::Success) return hostResult(r);
            pointers[n]=scratchpads[n]->busAddress();
        }
        // DCBAA[0] points at the scratchpad pointer array, never at a page.
        auto *dcbaa=reinterpret_cast<UInt64 *>(static_cast<UInt8 *>(arena->bytes())+DCBAAOffset);
        if (core.scratchpadCount()) dcbaa[0]=arena->busAddress()+ScratchOffset;
        return kIOReturnSuccess;
    }
    IOReturn configure() {
        return hardware([&] {
            auto *cpu=static_cast<UInt8 *>(arena->bytes());const auto bus=arena->busAddress();
            const auto bits=core.dmaBits();
            if (!commands.initialize(reinterpret_cast<XHCI::TRB *>(cpu+CommandOffset),bus+CommandOffset,RingEntries,bits) ||
                !events.initialize(reinterpret_cast<XHCI::TRB *>(cpu+EventOffset),bus+EventOffset,RingEntries,bits))
                return kIOReturnBadArgument;
            // Ownership is recorded before writing any hardware DMA pointer.
            // Partial publication is also retired only after a hardware halt.
            published=true;
            auto r=arena->publish(epoch);
            for (unsigned n=0;r==USB::Result::Success && n<core.scratchpadCount();++n)
                r=scratchpads[n]->publish(epoch);
            if (r!=USB::Result::Success) return hostResult(r);
            const XHCI::MemoryLayout layout{bus+CommandOffset,bus+DCBAAOffset,bus+TableOffset,
                bus+EventOffset,core.scratchpadCount() ? bus+ScratchOffset : 0,
                reinterpret_cast<XHCI::SegmentTable *>(cpu+TableOffset),RingEntries,
                core.scratchpadCount(),core.maxSlots()};
            r=core.configure(layout);configured=r==USB::Result::Success;
            return hostResult(r);
        });
    }
    IOReturn installEvents() {
        loop=owner->getWorkLoop();
        if (!loop) return kIOReturnNotReady;
        loop->retain();
        gate=IOCommandGate::commandGate(owner);
        if (!gate) return kIOReturnNoMemory;
        auto result=loop->addEventSource(gate);
        if (result!=kIOReturnSuccess) return result;
        gateAdded=true;
        int type=0;result=nub->getInterruptType(0,&type);
        // A level source lets IOKit's primary path mask the line before the
        // thread action. No blocking gate bridge or MMIO runs in the filter.
        if (result!=kIOReturnSuccess || type!=kIOInterruptTypeLevel) return kIOReturnUnsupported;
        irq=IOFilterInterruptEventSource::filterInterruptEventSource(owner,
            QcomUSBHost::interruptAction,QcomUSBHost::interruptFilter,nub,0);
        if (!irq) return kIOReturnNoMemory;
        irq->disable();
        result=loop->addEventSource(irq);
        if (result!=kIOReturnSuccess) return result;
        irqAdded=true;
        timer=IOTimerEventSource::timerEventSource(owner,QcomUSBHost::timerAction);
        if (!timer) return kIOReturnNoMemory;
        result=loop->addEventSource(timer);
        if (result!=kIOReturnSuccess) return result;
        timerAdded=true;
        releaseWork=thread_call_allocate_with_options(QcomUSBHost::releaseWorker,this,
            THREAD_CALL_PRIORITY_KERNEL,THREAD_CALL_OPTIONS_ONCE);
        deviceWork=thread_call_allocate_with_options(QcomUSBHost::deviceWorker,this,
            THREAD_CALL_PRIORITY_KERNEL,THREAD_CALL_OPTIONS_ONCE);
        requestWork=thread_call_allocate_with_options(QcomUSBHost::requestWorker,this,
            THREAD_CALL_PRIORITY_KERNEL,THREAD_CALL_OPTIONS_ONCE);
        commandReleaseWork=thread_call_allocate_with_options(QcomUSBHost::commandReleaseWorker,this,
            THREAD_CALL_PRIORITY_KERNEL,THREAD_CALL_OPTIONS_ONCE);
        ttWork=thread_call_allocate_with_options(QcomUSBHost::ttWorker,this,
            THREAD_CALL_PRIORITY_KERNEL,THREAD_CALL_OPTIONS_ONCE);
        closeWork=thread_call_allocate_with_options(QcomUSBHost::closeWorker,this,
            THREAD_CALL_PRIORITY_KERNEL,THREAD_CALL_OPTIONS_ONCE);
        return releaseWork && deviceWork && requestWork && commandReleaseWork && ttWork && closeWork ? kIOReturnSuccess : kIOReturnNoMemory;
    }
    IOReturn run() {
        return gated([&] {
            if (stopping || fault || !configured) return kIOReturnNotReady;
            const auto result=hardware([&] {
                auto r=commandQueue.initialize(epoch,core.maxSlots(),core.contextStride(),core.dmaBits());
                if (r==USB::Result::Success) r=core.run();
                if (r!=USB::Result::Success) return hostResult(r);
                const auto armed=nub->armHostFrameCounter(owner,epoch,core.frameCounterOffset());
                if (armed!=kIOReturnSuccess) return armed;
                auto *cpu=static_cast<UInt8 *>(arena->bytes());
                return owner->beginFrameEvents(reinterpret_cast<XHCI::TRB *>(cpu+EventOffset),RingEntries,epoch) ?
                    kIOReturnSuccess : kIOReturnNotReady;
            });
            if (result!=kIOReturnSuccess) { fail(result);return result; }
            running=true;irq->enable();timer->setTimeoutMS(1);
            return kIOReturnSuccess;
        });
    }
    void command(const XHCI::CommandRecord &) {}
    USB::Result prepareEventAcknowledgement() { return owner->observeFrameEvents(); }
    USB::Result frameRollover(UInt32) {
        // A new wrap may arrive between the initial scan and this peek.
        const auto observed=owner->observeFrameEvents();
        if (observed!=USB::Result::Success) return observed;
        if (consumedFrameRollovers==UINT64_MAX || !owner->observedFrameRollover(consumedFrameRollovers+1))
            return USB::Result::HardwareError;
        ++consumedFrameRollovers;return USB::Result::Success;
    }
    USB::Result port(unsigned number) {
        if (number>15 || !ports[number]) return USB::Result::HardwareError;
        UInt32 observed=0;
        const auto result=ports[number]->hardware.acknowledgeChanges(observed);
        if (result==USB::Result::Success) changedPorts[number]=true;
        return result;
    }
    void scheduleRequests() { if (requestWork) thread_call_enter(requestWork); }
    IsoFrame *findIsoFrame(Request &request,UInt32 index) {
        for (auto *frame:isoFrames) if (frame && frame->request==&request && frame->index==index) return frame;
        return nullptr;
    }
    bool retireIsoTransfers(Pipe &pipe,bool all=false) {
        for (auto *frame:isoFrames) if (frame && frame->request->pipe==&pipe && frame->phase==IsoFramePhase::Submitted) {
            XHCI::TransferRecord record{};
            if (!pipe.transfers.copy(frame->memory.ticket(),record)) return false;
            if (record.phase!=XHCI::TransferPhase::Completed && record.phase!=XHCI::TransferPhase::Isolated) {
                if (all) return false;
                continue;
            }
            uintptr_t released=0;
            if (frame->memory.retire(pipe.transfers,released)!=USB::Result::Success ||
                released!=reinterpret_cast<uintptr_t>(frame->request->native)) return false;
            frame->queueRetain=true;frame->phase=IsoFramePhase::Completing;frame->stamp=mach_absolute_time();
            frame->status=record.result==USB::Result::Success ? kIOReturnSuccess :
                record.completionCode==23 ? kIOReturnIsoTooOld :
                record.completionCode==3 || record.completionCode==31 ? kIOReturnOverrun :
                record.completionCode==2 ? (pipe.configuration.address&128 ? kIOReturnOverrun : kIOReturnUnderrun) :
                record.completionCode==4 ? kIOReturnIOError :
                frame->request->cancelled ? frame->request->status :
                pipe.isoRecovering ? pipe.isoRecoveryReason : hostResult(record.result);
        }
        scheduleRequests();return true;
    }
    bool isoHardwareComplete(const Request &request) {
        if (request.iso.nextSubmission()!=request.iso.count()) return false;
        for (auto *frame:isoFrames) if (frame && frame->request==&request &&
            (frame->phase==IsoFramePhase::Ready || frame->phase==IsoFramePhase::Submitted)) return false;
        return true;
    }
    void cancelIsoPipe(Pipe &pipe,IOReturn reason) {
        // A Stop/Set Dequeue changes the entire endpoint. Account for every
        // affected Native request and every payload before reopening it.
        for (auto *request=pipe.head;request;request=request->next) if (request->native &&
            request->phase!=RequestPhase::Completing && request->phase!=RequestPhase::Pinned && !request->cancelled &&
            !isoHardwareComplete(*request)) {
            request->cancelled=true;request->status=reason;
        }
        bool owned=false;
        for (auto *frame:isoFrames) if (frame && frame->request->pipe==&pipe && frame->phase==IsoFramePhase::Submitted) {
            frame->memory.quarantine();owned=true;
        }
        if (owned || pipe.transfers.faulted()) {
            pipe.transfers.quiesce();pipe.isoRecovering=true;
        }
        pipe.isoRecoveryReason=reason;pipe.isoFlow.drain();scheduleRequests();
    }
    void resynchronizeIsoPipe(Pipe &pipe) {
        pipe.isoFlow.drain();
        bool owned=false;
        for (auto *frame:isoFrames) if (frame && frame->request->pipe==&pipe && frame->phase==IsoFramePhase::Submitted) {
            frame->memory.quarantine();owned=true;
        }
        if (owned || pipe.transfers.faulted()) {
            pipe.transfers.quiesce();pipe.isoRecovering=true;pipe.isoRecoveryReason=kIOReturnIsoTooOld;
        }
        // Ready/unprepared frames retain their Native request and payload.
        // Only already published, possibly misaligned TDs are discarded.
        scheduleRequests();
    }
    USB::Result isoFrameSnapshot(UInt64 &frame,UInt32 &raw) {
        // Native frame getters are IRQ-safe seqlock readers. Their only MMIO
        // uses the provider's separately revocable MFINDEX snapshot; they
        // neither allocate nor take the Family/role workloop gate.
        for (unsigned retry=0;retry<3;++retry) {
            frame=owner->getFrameNumber();raw=UInt32(owner->getRawMicroFrameCount());
            if (__atomic_load_n(&owner->fFrameReadFailed,__ATOMIC_ACQUIRE)) return USB::Result::HardwareError;
            if ((frame&2047)==(raw>>3)) return USB::Result::Success;
        }
        return USB::Result::Busy;
    }
    bool submitIsoFrames() {
        bool changed=false;
        for (auto *device:devices) if (device) for (auto *pipe:device->pipes) {
            if (!pipe || pipe->configuration.type!=XHCI::EndpointType::Isochronous || pipe->isoRecovering) continue;
            unsigned budget=MaximumPipeIsoFrames;
            for (auto *request=pipe->head;request && budget;request=request->next) {
                if (request->phase!=RequestPhase::Isochronous || request->busy) break;
                while (request->iso.nextSubmission()<request->iso.count() && budget) {
                    auto *frame=findIsoFrame(*request,request->iso.nextSubmission());
                    if (!frame || frame->phase!=IsoFramePhase::Ready) break;
                    --budget;
                    if (request->cancelled) frame->status=request->status;
                    if (frame->status==kIOReturnSuccess) {
                        // Cancellation and preparation errors still drain in
                        // software while context rebuilding waits for another
                        // pipe's device lease. Only new hardware work waits.
                        if (pipe->isoNeedsFreshContext || !device->power.running()) break;
                        const auto result=hardware([&] {
                            if (!running || stopping || fault || pipe->closing || device->retiring) return kIOReturnAborted;
                            UInt64 current=0;UInt32 raw=0;
                            auto r=isoFrameSnapshot(current,raw);
                            if (r!=USB::Result::Success) return hostResult(r);
                            XHCI::IsoSchedule schedule;
                            r=pipe->isoFlow.plan(pipe->configuration,core.contiguousFrameIDSupported(),request->firstFrame,
                                frame->index,current,raw,core.isochronousSchedulingThreshold(),schedule);
                            if (r==USB::Result::Timeout) return kIOReturnIsoTooOld;
                            if (r==USB::Result::Pending) return kIOReturnNotReady;
                            if (r!=USB::Result::Success) return hostResult(r);
                            r=frame->memory.scheduleIsochronous(pipe->configuration,schedule);
                            if (r!=USB::Result::Success) return kIOReturnIOError;
                            frame->usesSIA=schedule.asap;
                            const auto now=hostTimeUS();
                            if (request->deadline && now>=request->deadline) return kIOReturnTimeout;
                            return hostResult(frame->memory.submit(pipe->transfers,pipe->backend.ring(),
                                reinterpret_cast<uintptr_t>(request->native),epoch,now,
                                request->deadline ? request->deadline-now : 0));
                        });
                        if (frame->memory.ticket()) {
                            frame->phase=IsoFramePhase::Submitted;
                            if (!request->iso.submit(frame->index)) { fail(kIOReturnIOError);return true; }
                            changed=true;
                            if (result!=kIOReturnSuccess) { cancelIsoPipe(*pipe,result);break; }
                            if (!pipe->isoFlow.submitted(pipe->configuration,request->firstFrame,frame->index)) {
                                cancelIsoPipe(*pipe,kIOReturnIOError);break;
                            }
                            continue;
                        }
                        if (result==kIOReturnBusy || result==kIOReturnNotReady) break;
                        frame->status=result;
                        if (result==kIOReturnTimeout) cancelIsoPipe(*pipe,result);
                    }
                    if (!request->iso.submit(frame->index)) { fail(kIOReturnIOError);return true; }
                    frame->phase=IsoFramePhase::Completing;frame->stamp=mach_absolute_time();changed=true;
                }
                if (request->iso.nextSubmission()!=request->iso.count()) break;
            }
        }
        return changed;
    }
    IOReturn publishIsoFrame(Request &request,UInt32 index,IOReturn status,UInt32 bytes,UInt64 stamp) {
        auto &frame=request.frameList[index];
        frame.completeCount=bytes;frame.timeStamp=stamp;
        __atomic_thread_fence(__ATOMIC_RELEASE);frame.status=status;
        // This is the original Family request, not an HCD wrapper. Native
        // update() converts exactly this next frame to the optional legacy
        // layout. The final completer does not perform that conversion.
        return request.native->update();
    }
    bool isoWork() {
        enum class Task { None,Prepare,Finish,Recover,Restart,Skip } task=Task::None;
        Request *request=nullptr;IsoFrame *frame=nullptr;
        unsigned slot=MaximumIsoFrames;UInt32 index=0,skip=0;
        XHCI::EndpointConfiguration configuration;
        bool changed=false;
        gated([&] {
            // Stop a lost ISO flow before spending time on CPU cleanup.
            for (auto *entry:requests) if (entry && entry->native && entry->isochronous &&
                entry->phase==RequestPhase::Isochronous && !entry->busy && entry->pipe->isoRecovering &&
                (!entry->pipe->device->power.transitioning() || stopping || fault)) {
                request=entry;task=Task::Recover;break;
            }
            // CPU copy and legacy conversion are strictly in Native request
            // order, including across requests sharing one endpoint.
            if (!request) for (auto *entry:isoFrames) if (entry && entry->phase==IsoFramePhase::Completing &&
                entry->request->phase==RequestPhase::Isochronous && !entry->request->busy &&
                entry->request->pipe->head==entry->request && entry->index==entry->request->iso.nextPublication()) {
                frame=entry;request=entry->request;task=Task::Finish;break;
            }
            if (!request && running && !stopping && !fault) for (auto *entry:requests) if (entry && entry->native &&
                entry->phase==RequestPhase::Isochronous && !entry->busy && !entry->cancelled && entry->pipe->head==entry &&
                !entry->iso.done() &&
                entry->pipe->isoNeedsFreshContext && !entry->pipe->device->busy && !entry->pipe->closing &&
                !entry->pipe->device->retiring && entry->pipe->device->power.running()) {
                request=entry;task=Task::Restart;entry->pipe->device->busy=true;break;
            }
            if (!request) for (auto *entry:requests) if (entry && entry->native && entry->isochronous &&
                entry->phase==RequestPhase::Isochronous && !entry->busy && entry->pipe->head==entry && !entry->pipe->isoRecovering) {
                if (entry->iso.done()) { entry->phase=RequestPhase::Completing;continue; }
                if (entry->cancelled && !entry->iso.buffered()) {
                    request=entry;task=Task::Skip;index=entry->iso.nextPreparation();
                    skip=entry->iso.count()-index;if (skip>64) skip=64;break;
                }
            }
            if (!request && !stopping && !fault && isoFrameCount<MaximumIsoFrames) {
                for (unsigned n=0;n<MaximumIsoFrames;++n) if (!isoFrames[n]) { slot=n;break; }
                // Prefetch through request boundaries before starting a new
                // hardware batch. A pipe consumes at most 128 of the shared
                // 256 payload slots, regardless of Native frame count.
                for (unsigned n=0;n<256;++n) {
                    const unsigned at=(isoPreparationCursor+n)%256;
                    auto *entry=requests[at];
                    if (!entry || entry->phase!=RequestPhase::Isochronous || entry->busy || entry->cancelled ||
                        entry->iso.nextPreparation()==entry->iso.count()) continue;
                    auto &pipe=*entry->pipe;
                    if (pipe.closing || pipe.device->retiring || !pipe.device->power.running() ||
                        pipe.isoRecovering || pipe.isoFrames==MaximumPipeIsoFrames) continue;
                    bool ordered=true;
                    for (auto *prior=pipe.head;prior && prior!=entry;prior=prior->next)
                        if (prior->iso.nextPreparation()!=prior->iso.count()) { ordered=false;break; }
                    if (!ordered) continue;
                    request=entry;index=entry->iso.nextPreparation();configuration=pipe.configuration;
                    task=Task::Prepare;isoPreparationCursor=(at+1)%256;break;
                }
            }
            if (!request) { changed=submitIsoFrames();return kIOReturnSuccess; }
            request->busy=true;++waiters;return kIOReturnSuccess;
        });
        if (!request) return changed;
        if (task==Task::Recover) recoverRequest(*request);
        else if (task==Task::Restart) {
            gated([&] {
                auto &pipe=*request->pipe;auto &device=*pipe.device;
                // A Drop+Add replacement initializes a fresh Running/Idle
                // endpoint. Do not infer a new ISO flow from Stop/Set DQ.
                // The busy lease is acquired without waiting, so a close of
                // another pipe can still drain on this shared worker.
                if (!running || stopping || fault || pipe.closing || device.retiring || request->cancelled || !device.power.running()) {
                    device.busy=false;gate->commandWakeup(&waiters,false);return kIOReturnAborted;
                }
                auto candidate=pipe.configuration;candidate.dequeue=pipe.backend.ring().nextAddress();
                candidate.cycle=bool(pipe.backend.ring().producerCycle());
                const auto result=configurePipe(device,pipe,candidate,true);
                if (result==kIOReturnSuccess) { pipe.isoNeedsFreshContext=false;pipe.isoFlow.ended(); }
                device.busy=false;gate->commandWakeup(&waiters,false);return result;
            });
        }
        else if (task==Task::Prepare) {
            frame=new IsoFrame(*memory,request,index);
            if (frame) {
                auto result=XHCI::isochronousTarget(configuration,request->firstFrame,index,frame->target);
                if (result==USB::Result::Success) result=frame->memory.prepareIsochronous(request->prepared,
                    configuration,request->frameList[index],core.dmaBits(),{});
                frame->status=hostResult(result);
            }
            gated([&] {
                if (!frame) cancelIsoPipe(*request->pipe,kIOReturnNoMemory);
                else if (slot>=MaximumIsoFrames || isoFrames[slot] || !request->iso.prepare(index)) {
                    request->phase=RequestPhase::Pinned;request->invariantPinned=true;owner->pin();fail(kIOReturnIOError);
                } else {
                    isoFrames[slot]=frame;++isoFrameCount;++request->pipe->isoFrames;
                }
                return kIOReturnSuccess;
            });
        } else if (task==Task::Skip) {
            const auto status=request->status;
            const auto stamp=mach_absolute_time();
            IOReturn updated=kIOReturnSuccess;
            for (UInt32 n=0;n<skip;++n) {
                const auto result=publishIsoFrame(*request,index+n,status,0,stamp);
                if (updated==kIOReturnSuccess) updated=result;
            }
            gated([&] {
                for (UInt32 n=0;n<skip;++n) if (!request->iso.prepare(index+n) ||
                    !request->iso.submit(index+n) || !request->iso.publish(index+n,0)) {
                    request->phase=RequestPhase::Pinned;request->invariantPinned=true;owner->pin();fail(kIOReturnIOError);break;
                }
                if (request->status==kIOReturnSuccess) request->status=updated;
                request->stamp=stamp;return kIOReturnSuccess;
            });
        } else if (task==Task::Finish) {
            const auto clean=frame->memory.finish(frame->memory.validData());
            if (frame->memory.currentState()!=USB::DMAState::Empty) {
                gated([&] {
                    frame->phase=IsoFramePhase::Pinned;request->phase=RequestPhase::Pinned;
                    frame->cleanupRecovery.failed(frame->memory.cleanupRetryable());
                    owner->pin();fail(hostResult(clean));gate->commandWakeup(request->pipe,false);return kIOReturnIOError;
                });
            } else {
                if (clean!=USB::Result::Success) frame->status=hostResult(clean);
                const auto bytes=clean==USB::Result::Success && frame->memory.validData() ? frame->memory.transferred() : 0;
                const auto updated=publishIsoFrame(*request,frame->index,frame->status,bytes,frame->stamp);
                if (frame->queueRetain) request->native->release();
                gated([&] {
                    if (!request->iso.publish(frame->index,bytes)) {
                        request->phase=RequestPhase::Pinned;request->invariantPinned=true;owner->pin();fail(kIOReturnIOError);
                    }
                    if (request->status==kIOReturnSuccess) request->status=frame->status;
                    if (request->status==kIOReturnSuccess) request->status=updated;
                    request->stamp=frame->stamp;
                    for (auto *&entry:isoFrames) if (entry==frame) { entry=nullptr;break; }
                    --isoFrameCount;--request->pipe->isoFrames;return kIOReturnSuccess;
                });
                delete frame;
            }
        }
        gated([&] {
            request->busy=false;--waiters;gate->commandWakeup(&waiters,false);return kIOReturnSuccess;
        });
        return true;
    }
    bool retireRequest(Request &request) {
        if (request.memory.ticket() && !request.queueRetain) {
            uintptr_t released=0;
            const auto r=request.memory.retire(request.pipe->transfers,released);
            if (r!=USB::Result::Success || released!=reinterpret_cast<uintptr_t>(request.native)) return false;
            request.queueRetain=true;
        }
        request.phase=RequestPhase::Completing;request.stamp=mach_absolute_time();scheduleRequests();return true;
    }
    void cancelRequest(Request &request,IOReturn reason) {
        if (!request.native || request.phase==RequestPhase::Completing || request.phase==RequestPhase::Pinned) return;
        if (request.isochronous) {
            if (!isoHardwareComplete(request)) cancelIsoPipe(*request.pipe,reason);
            return;
        }
        if (!request.cancelled) { request.cancelled=true;request.status=reason; }
        if (request.phase==RequestPhase::Submitted) {
            request.pipe->transfers.quiesce();request.memory.quarantine();request.phase=RequestPhase::Recovering;
        } else if (request.phase==RequestPhase::Queued || request.phase==RequestPhase::Ready) request.phase=RequestPhase::Completing;
        scheduleRequests();
    }
    void cancelRequests(Pipe &pipe,IOReturn reason,IOService *client=nullptr) {
        if (pipe.configuration.type==XHCI::EndpointType::Isochronous) { cancelIsoPipe(pipe,reason);return; }
        for (auto *request=pipe.head;request;request=request->next)
            if (!client || request->client==client) cancelRequest(*request,reason);
    }
    bool isolateTransfers() {
        if (!dmaStopped) return false;
        for (auto *device:devices) if (device) for (auto *pipe:device->pipes) if (pipe && pipe->transfers.initialized()) {
            if (pipe->transfers.isolate()!=USB::Result::Success) return false;
            if (pipe->configuration.type==XHCI::EndpointType::Isochronous) {
                if (!retireIsoTransfers(*pipe,true)) return false;
                pipe->isoRecovering=false;pipe->isoNeedsFreshContext=false;pipe->isoFlow.ended();
            }
        }
        for (auto *request:requests) if (request && request->native && !request->isochronous && request->memory.ticket() &&
            request->phase!=RequestPhase::Completing && request->phase!=RequestPhase::Pinned) {
            if (!retireRequest(*request)) return false;
        }
        const bool recovery=nub->dmaRecoveryReady();bool retried=false;
        for (auto *frame:isoFrames) if (frame && frame->phase==IsoFramePhase::Pinned &&
            !frame->request->busy && !frame->request->invariantPinned && frame->cleanupRecovery.retry(recovery)) {
            frame->phase=IsoFramePhase::Completing;retried=true;
        }
        for (auto *request:requests) if (request && request->native && request->phase==RequestPhase::Pinned &&
            !request->busy && !request->invariantPinned) {
            if (!request->isochronous && request->cleanupRecovery.retry(recovery)) {
                request->phase=RequestPhase::Completing;retried=true;
            } else if (request->isochronous) {
                bool pinned=false;
                for (auto *frame:isoFrames) if (frame && frame->request==request && frame->phase==IsoFramePhase::Pinned) pinned=true;
                if (!pinned) {request->phase=RequestPhase::Isochronous;retried=true;}
            }
        }
        if (retried) scheduleRequests();
        return true;
    }
    bool ownedTransferCookie(UInt64 ticket) {
        if (!ticket) return false;
        for (auto *entry:requests) if (entry && entry->native && entry->memory.ticket()==ticket && !entry->queueRetain) return true;
        for (auto *frame:isoFrames) if (frame && frame->memory.ticket()==ticket && !frame->queueRetain) return true;
        return false;
    }
    USB::Result transfer(const XHCI::TRB &event,UInt64 generation) {
        const unsigned slot=event.control>>24,id=(event.control>>16)&31,code=event.status>>24;
        auto *device=slot ? slotDevices[slot] : nullptr;
        auto *pipe=device && id ? device->pipes[id] : nullptr;
        if (pipe && pipe->transfers.pauseEvent(pipe->stopTicket,event,generation)) return USB::Result::Success;
        const bool emptyEvent=generation==epoch && event.type()==XHCI::TransferEvent &&
            !(event.control&~(XHCI::Cycle|(63u<<10)|(31u<<16)|(255u<<24))) &&
            !event.parameter() && !(event.status&0xffffff) && (code==14 || code==15);
        if (pipe && pipe->configuration.type==XHCI::EndpointType::Isochronous && emptyEvent) {
            // This notification often follows the final Native handoff, when
            // pipe.requests is already zero. It still proves flow teardown.
            if (!pipe->transfers.outstanding()) pipe->isoFlow.ended();
            else if (!pipe->isoRecovering) resynchronizeIsoPipe(*pipe);
            scheduleRequests();return USB::Result::Success;
        }
        if (pipe && pipe->configuration.type==XHCI::EndpointType::Isochronous && pipe->requests) {
            if (pipe->isoRecovering || pipe->transfers.faulted()) {
                if (pipe->transfers.stoppedEvent(pipe->stopTicket,event,generation)) return USB::Result::Success;
                for (auto *frame:isoFrames) if (frame && frame->request->pipe==pipe &&
                    frame->phase==IsoFramePhase::Submitted && pipe->transfers.ownsEvent(frame->memory.ticket(),event,generation))
                    return code==1 || (code>=2 && code<=6) || code==13 || code==23 || code==31 || (code>=26 && code<=28) ?
                        USB::Result::Success : USB::Result::HardwareError;
                return USB::Result::HardwareError;
            }
            XHCI::TransferRecord record{};
            const auto result=pipe->transfers.complete(event,generation,record);
            if (result==USB::Result::Pending || result==USB::Result::Aborted) {
                if (result==USB::Result::Aborted && ownedTransferCookie(event.parameter()>>1)) return USB::Result::HardwareError;
                return USB::Result::Success;
            }
            if (result==USB::Result::Success) {
                bool restart=code==23 && !core.contiguousFrameIDSupported();
                if (code==23) for (auto *frame:isoFrames) if (frame && frame->request->pipe==pipe &&
                    frame->phase==IsoFramePhase::Submitted && frame->usesSIA) restart=true;
                if (!retireIsoTransfers(*pipe)) return USB::Result::HardwareError;
                if (restart && device->power.running()) resynchronizeIsoPipe(*pipe);
                return USB::Result::Success;
            }
            return USB::Result::HardwareError;
        }
        if (!pipe || !pipe->active) {
            const auto ticket=event.parameter()>>1;
            if (generation!=epoch || event.type()!=XHCI::TransferEvent || !(event.control&XHCI::EventDataFlag) ||
                (event.control&~(XHCI::Cycle|XHCI::EventDataFlag|(63u<<10)|(31u<<16)|(255u<<24))) ||
                !ticket || ticket>commandQueue.lastTransferTicket()) return USB::Result::HardwareError;
            if (ownedTransferCookie(ticket)) return USB::Result::HardwareError;
            // A cookie already retired before endpoint/slot reuse owns no
            // current DMA. Physical/unrecognized events still fault the HCD.
            return USB::Result::Success;
        }
        auto &request=*pipe->active;
        if (request.phase==RequestPhase::Recovering || request.phase==RequestPhase::TTRecovering) {
            if (pipe->transfers.stoppedEvent(pipe->stopTicket,event,generation)) return USB::Result::Success;
            const bool terminal=code==1 || (code>=2 && code<=6) || code==13 || code==36 || (code>=26 && code<=28);
            return terminal && pipe->transfers.ownsEvent(request.memory.ticket(),event,generation) ?
                USB::Result::Success : USB::Result::HardwareError;
        }
        XHCI::TransferRecord record{};
        auto r=pipe->transfers.complete(event,generation,record);
        if (r==USB::Result::Pending || r==USB::Result::Aborted)
            return r==USB::Result::Aborted && ownedTransferCookie(event.parameter()>>1) ?
                USB::Result::HardwareError : USB::Result::Success;
        if (r==USB::Result::Success)
            return retireRequest(request) ? USB::Result::Success : USB::Result::HardwareError;
        if (pipe->transfers.copy(request.memory.ticket(),record) &&
            (record.completionCode==2 || record.completionCode==3 || record.completionCode==4 || record.completionCode==6 ||
                record.completionCode==36)) {
            const auto status=record.completionCode==6 ? HostPipeStalled : kIOReturnIOError;
            cancelRequest(request,status);
            if (pipe->id!=1) cancelRequests(*pipe,HostPipeStalled);
            return USB::Result::Success;
        }
        return USB::Result::HardwareError;
    }
    void poll() {
        if (!running || stopping || fault) return;
        if (__atomic_load_n(&owner->fFrameReadFailed,__ATOMIC_ACQUIRE)) { fail(kIOReturnIOError);return; }
        XHCI::EventBatch batch;
        const auto result=hardware([&] {
            // The filter may be delayed or the timer may service an event
            // first. Observe before any consumer can release ring slots.
            auto r=owner->observeFrameEvents();
            if (r==USB::Result::Success) r=XHCI::drainEvents(core,events,commandQueue,*this,epoch,128,batch);
            if (r==USB::Result::Success) r=commandQueue.expire(hostTimeUS());
            if (r==USB::Result::Success) for (unsigned n=1;n<=core.maxPorts();++n) {
                auto *p=ports[n];
                if (p && p->busy && p->result==USB::Result::Pending) p->result=p->hardware.advance(hostTimeUS());
            }
            return hostResult(r);
        });
        if (result!=kIOReturnSuccess) { fail(result);return; }
        if (batch.commands) gate->commandWakeup(&commandQueue,false);
        bool requestTimer=false,isoTimer=false;
        const auto now=hostTimeUS();
        bool ttTimer=false;
        for (auto *device:devices) if (device && ttDomainAlive(*device)) {
            if (device->enumerationTT.dirty()) { ttTimer=true;queueEnumerationTT(*device,now); }
            for (auto *pipe:device->pipes) if (pipe && pipe->ttCleanup.dirty()) { ttTimer=true;queueTT(*pipe); }
        }
        for (auto *request:requests) if (request && request->native && request->deadline && request->pipe->device->power.running() &&
            request->phase!=RequestPhase::Completing && request->phase!=RequestPhase::Pinned) {
            if (now>=request->deadline) cancelRequest(*request,kIOReturnTimeout);
            else requestTimer=true;
        }
        for (auto *request:requests) if (request && request->native && request->phase==RequestPhase::Isochronous) isoTimer=true;
        if (isoTimer) scheduleRequests();
        bool pendingPorts=false,notify=false;
        for (unsigned n=1;n<=core.maxPorts();++n) {
            auto *p=ports[n];
            if (p && p->busy) {
                if (p->result==USB::Result::Pending) pendingPorts=true;
                else gate->commandWakeup(p,false);
            }
            notify|=changedPorts[n];
        }
        if (notify && releaseWork) thread_call_enter(releaseWork);
        if (timer) {
            if (batch.more) timer->setTimeoutMS(0);
            else if (pendingPorts || isoTimer) timer->setTimeoutMS(1);
            else if (commandQueue.outstanding() || requestTimer) timer->setTimeoutMS(10);
            else if (ttTimer) timer->setTimeoutMS(100);
            else timer->cancelTimeout();
        }
    }
    IOReturn portStatus(unsigned number,UInt32 &status) {
        status=0;
        return gated([&] {
            if (!running || stopping || fault || !number || number>core.maxPorts() || !ports[number] || !connectorPorts.routed(number))
                return kIOReturnNotReady;
            const auto result=hardware([&] { return hostResult(ports[number]->hardware.status(status)); });
            return result;
        });
    }
    IOReturn operatePort(unsigned number,PortOperation operation) {
        return gated([&] {
            if (!running || stopping || fault || !number || number>core.maxPorts() || !ports[number] || !connectorPorts.routed(number))
                return kIOReturnNotReady;
            auto &p=*ports[number];
            if (p.busy) return kIOReturnBusy;
            if (loop->onThread()) return kIOReturnCannotLock;
            p.busy=true;++waiters;
            const auto begun=hardware([&] {
                const auto now=hostTimeUS();
                switch (operation) {
                case PortOperation::PowerOn:p.result=p.hardware.power(true,now);break;
                case PortOperation::PowerOff:p.result=p.hardware.power(false,now);break;
                case PortOperation::Suspend:p.result=p.hardware.suspend(now);break;
                case PortOperation::Resume:p.result=p.hardware.resume(now);break;
                case PortOperation::Reset:p.result=p.hardware.reset(now);break;
                }
                return kIOReturnSuccess;
            });
            auto result=begun;
            if (begun==kIOReturnSuccess && p.result==USB::Result::Pending) {
                timer->setTimeoutMS(1);
                UInt64 deadline=0;clock_interval_to_deadline(350,kMillisecondScale,&deadline);
                while (p.result==USB::Result::Pending && !stopping && !fault) {
                    if (gate->commandSleep(&p,deadline,THREAD_UNINT)==THREAD_TIMED_OUT) {
                        if (p.result!=USB::Result::Pending) break;
                        fail(kIOReturnTimeout);break;
                    }
                }
            }
            if (result==kIOReturnSuccess) result=stopping ? kIOReturnAborted : fault ? faultReason : hostResult(p.result);
            p.busy=false;--waiters;gate->commandWakeup(&waiters,false);
            return result;
        });
    }
    bool stopDMA() {
        owner->endFrameEvents();
        nub->disarmHostFrameCounter(owner,epoch);
        if (dmaStopped || !published) return true;
        dmaStopped=nub->dmaIsolated();
        if (!dmaStopped) dmaStopped=hardware([&] {
            core.maskInterrupts();
            return hostResult(core.halt());
        })==kIOReturnSuccess;
        running=false;
        if (!dmaStopped) {
            if (arena) arena->quarantine();
            for (auto *page:scratchpads) if (page) page->quarantine();
        }
        return dmaStopped;
    }
    void fail(IOReturn reason) {
        if (!fault) faultReason=reason;
        fault=true;
        nub->reportFault(owner,epoch);
        if (irq) irq->disable();
        if (timer) timer->cancelTimeout();
        commandQueue.quarantine(USB::memoryResult(reason));
        stopDMA();
        for (auto *request:requests) if (request && request->native) cancelRequest(*request,reason);
        if (dmaStopped && !isolateTransfers()) owner->pin();
        if (gate) {
            gate->commandWakeup(&commandQueue,false);
            gate->commandWakeup(&waiters,false);
            for (auto *p:ports) if (p && p->busy) gate->commandWakeup(p,false);
        }
    }
    void deferRelease(uintptr_t object) {
        // The fixed bound includes every supported device/endpoint reference.
        // Normal command submissions still stop at backlog64.
        if (!object) return;
        if (releaseCount==sizeof(releaseObjects)/sizeof(releaseObjects[0])) { owner->pin();return; }
        releaseObjects[releaseCount++]=reinterpret_cast<OSObject *>(object);
        if (!stopping && releaseWork) thread_call_enter(releaseWork);
    }
    IOReturn execute(const XHCI::Command &command,OSObject *payload,XHCI::CommandRecord &out,UInt32 timeoutUS,
        XHCI::InputContext *input=nullptr,bool hold=false,UInt64 *publishedTicket=nullptr) {
        out={};
        if (!payload || !timeoutUS || timeoutUS>5000000) return kIOReturnBadArgument;
        return gated([&] {
            if (publishedTicket) *publishedTicket=0;
            if (!running || stopping || fault || controllerReleases>=64) return kIOReturnNotReady;
            // Sleeping on the Family workloop thread would prevent its IRQ
            // action from ever delivering this command's completion.
            if (loop->onThread()) return kIOReturnCannotLock;
            ++waiters;
            UInt64 ticket=0;
            auto result=hardware([&] {
                return hostResult(input ? input->submit(commandQueue,command.type,command.slot,command.option,
                    reinterpret_cast<uintptr_t>(payload),hostTimeUS(),timeoutUS,ticket) :
                    commandQueue.submit(command,reinterpret_cast<uintptr_t>(payload),hostTimeUS(),timeoutUS,ticket));
            });
            if (publishedTicket) *publishedTicket=ticket;
            if (ticket) {
                if (result!=kIOReturnSuccess) fail(result);
                else timer->setTimeoutMS(1);
                UInt64 deadline=0;clock_interval_to_deadline(timeoutUS,kMicrosecondScale,&deadline);
                for (;;) {
                    if (!commandQueue.copy(ticket,out)) { result=kIOReturnIOError;fail(result);break; }
                    if (out.phase==XHCI::CommandPhase::Completed) { result=hostResult(out.result);break; }
                    if (out.phase==XHCI::CommandPhase::Isolated) { result=kIOReturnAborted;break; }
                    if (stopping || fault || out.phase==XHCI::CommandPhase::Quarantined) {
                        if (result==kIOReturnSuccess) result=fault ? faultReason : kIOReturnAborted;
                        break;
                    }
                    const auto wake=gate->commandSleep(&commandQueue,deadline,THREAD_UNINT);
                    if (wake==THREAD_TIMED_OUT) {
                        // Inspect again under the same gate: completion may
                        // already have won while this thread reacquired it.
                        if (commandQueue.copy(ticket,out) && out.phase==XHCI::CommandPhase::Completed) continue;
                        result=kIOReturnTimeout;fail(result);commandQueue.copy(ticket,out);break;
                    }
                }
                if (out.phase==XHCI::CommandPhase::Completed || out.phase==XHCI::CommandPhase::Isolated) {
                    if (input && (out.command.parameter!=input->address() || input->retire(ticket)!=USB::Result::Success)) {
                        result=kIOReturnIOError;fail(result);
                    }
                    if (!hold) {
                        const auto ack=acknowledgeCommand(out);
                        if (ack!=kIOReturnSuccess) { result=ack;fail(result); }
                    }
                }
            }
            --waiters;gate->commandWakeup(&waiters,false);
            return result;
        });
    }
    IOReturn acknowledgeCommand(const XHCI::CommandRecord &record) {
        uintptr_t released=0;
        const auto ack=commandQueue.acknowledge(record.ticket,record.owner,released);
        if (ack==USB::Result::Success) {
            if (released==reinterpret_cast<uintptr_t>(owner)) {
                ++controllerReleases;if (!stopping && commandReleaseWork) thread_call_enter(commandReleaseWork);
            } else deferRelease(released);
        }
        return hostResult(ack);
    }
    IOUSBHostDevice *createDevice(unsigned nativeSpeed,UInt32 location) {
        IOUSBHostDevice *created=nullptr;
        gated([&] {
            if (!running || stopping || fault || !portsComplete || loop->onThread() || releaseCount>=64) return kIOReturnNotReady;
            auto *base=OSDynamicCast(OSNumber,owner->getProperty("locationID"));
            XHCI::FamilyLocation path;
            if (!base || XHCI::decodeFamilyLocation(location,base->unsigned32BitValue(),core.maxPorts(),path)!=USB::Result::Success)
                return kIOReturnBadArgument;
            if (!connectorPorts.routed(path.rootPort)) return kIOReturnUnsupported;
            const auto speed=XHCI::speedFromFamily(nativeSpeed);
            if (speed==USB::Speed::Unknown) return kIOReturnUnsupported;
            const auto *protocol=protocols.get(path.rootPort);
            if (!protocol) return kIOReturnBadArgument;
            Device *parent=nullptr;
            if (path.depth) {
                const auto locationOfParent=XHCI::parentLocation(location,path.depth);
                for (auto *entry:devices) if (entry && entry->location==locationOfParent) { parent=entry;break; }
                if (!parent) return kIOReturnNotReady;
            }
            unsigned rootSpeedID=0;
            auto rootStatus=[&] {
                UInt32 raw=0;const auto result=core.readPort(path.rootPort,raw);
                if (result!=USB::Result::Success) return hostResult(result);
                const unsigned sid=(raw>>10)&15;
                if (!sid || protocol->speeds[sid]==USB::Speed::Unknown || (rootSpeedID && rootSpeedID!=sid) ||
                    (raw&(XHCI::PortSC::Connected|XHCI::PortSC::Enabled|XHCI::PortSC::Power))!=
                        (XHCI::PortSC::Connected|XHCI::PortSC::Enabled|XHCI::PortSC::Power) ||
                    (raw&(XHCI::PortSC::Reset|XHCI::PortSC::WarmReset|XHCI::PortSC::OverCurrent|XHCI::PortSC::LinkMask)))
                    return kIOReturnNotReady;
                rootSpeedID=sid;return kIOReturnSuccess;
            };
            auto result=hardware(rootStatus);
            if (result!=kIOReturnSuccess) return result;
            if (parent) {
                const auto *root=parent->root();
                if (!root || root->configuration.speedID!=rootSpeedID || !topologyReady(*parent) || !parent->power.running())
                    return kIOReturnNotReady;
            }
            unsigned index=0;
            for (unsigned n=1;n<=core.maxSlots();++n) {
                if (devices[n] && devices[n]->location==location) return kIOReturnBusy;
                if (!devices[n] && !index) index=n;
            }
            if (!index) return kIOReturnNoResources;
            auto *device=new Device(*memory,location);
            if (!device) return kIOReturnNoMemory;
            result=hostResult(device->initialize(path,parent,speed,*protocol,rootSpeedID));
            if (result!=kIOReturnSuccess) { delete device;return result; }
            if (ttEnumerationBlocked(device->configuration)) {
                const auto detached=device->detach();delete device;
                if (detached!=USB::Result::Success) fail(kIOReturnIOError);
                return kIOReturnNotReady;
            }
            devices[index]=device;++waiters;
            thread_call_enter(deviceWork);
            // Preparation runs on a separate worker with no workloop gate.
            // Never return while that worker can still reference this entry.
            while (!device->prepared) gate->commandSleep(device,THREAD_UNINT);
            result=hostResult(device->preparation);
            if (stopping || fault || !topologyReady(*device)) result=kIOReturnAborted;
            if (result==kIOReturnSuccess) result=hardware([&] {
                const auto live=rootStatus();if (live!=kIOReturnSuccess) return live;
                return hostResult(device->memory.configure(device->configuration,*protocol));
            });
            XHCI::CommandRecord record;
            if (result==kIOReturnSuccess) {
                XHCI::Command enable;enable.type=XHCI::EnableSlot;enable.endpoint=protocol->slotType;
                result=execute(enable,owner,record,1000000);
                if (result==kIOReturnSuccess) {
                    if (!record.slot || record.slot>core.maxSlots() || slotDevices[record.slot]) {
                        result=kIOReturnIOError;fail(result);
                    } else {
                        device->slot=record.slot;slotDevices[record.slot]=device;
                    }
                }
            }
            if (result==kIOReturnSuccess && !topologyReady(*device)) result=kIOReturnAborted;
            if (result==kIOReturnSuccess) result=hardware([&] {
                const auto r=device->memory.publish(epoch);
                if (r!=USB::Result::Success) return hostResult(r);
                auto *dcbaa=reinterpret_cast<UInt64 *>(static_cast<UInt8 *>(arena->bytes())+DCBAAOffset);
                dcbaa[device->slot]=device->memory.outputAddress();io.dmaWriteBarrier();
                return kIOReturnSuccess;
            });
            if (result==kIOReturnSuccess) {
                XHCI::Command address;address.type=XHCI::AddressDevice;address.slot=device->slot;
                auto &input=device->memory.inputContext();address.parameter=input.address();
                if (device->configuration.ttHub) result=hostResult(device->enumerationTT.arm(commandQueue,
                    device->configuration,epoch,device->slot,input.address(),reinterpret_cast<uintptr_t>(owner)));
                if (result==kIOReturnSuccess) result=execute(address,owner,record,1000000,&input,true);
            }
            if (result==kIOReturnSuccess) {
                result=hardware([&] {
                    XHCI::SlotSnapshot output;
                    auto r=device->memory.addressed(io,record,device->slot,output);
                    if (r!=USB::Result::Success) return hostResult(r);
                    for (auto *other:devices) if (other && other!=device && other->address==output.address)
                        return kIOReturnIOError;
                    device->address=output.address;
                    return kIOReturnSuccess;
                });
                if (result!=kIOReturnSuccess) fail(result);
            }
            if (record.command.type==XHCI::AddressDevice && record.phase==XHCI::CommandPhase::Completed) {
                // Inspect the actual failed Address record before acknowledge.
                // Native has no child/pipe yet; default-address cleanup must
                // continue after this factory returns and HubPort gates exit.
                const bool tt=device->enumerationTT.failed(commandQueue,record.ticket);
                const auto ack=acknowledgeCommand(record);
                if (ack!=kIOReturnSuccess) { result=ack;fail(result); }
                if (tt) queueEnumerationTT(*device,hostTimeUS());
            }
            if (result==kIOReturnSuccess && !stopping && !fault && topologyReady(*device)) {
                auto *native=owner->AppleUSBHostController::nativeSlot194(UInt8(nativeSpeed),UInt16(device->address));
                if (!native) result=kIOReturnNoMemory;
                else if (!nub->accepts(owner,epoch) || native->getSpeed()!=nativeSpeed || native->getDeviceAddress()!=device->address ||
                    !native->service()->setProperty("locationID",location,32)) {
                    deferRelease(reinterpret_cast<uintptr_t>(native->service()));result=kIOReturnIOError;
                } else {
                    // The caller/Native port owns the original factory ref;
                    // this ledger retain survives a deferred retire callback.
                    native->service()->retain();device->native=native;created=native;
                }
            }
            if (!created) { device->retiring=true;thread_call_enter(deviceWork); }
            device->busy=false;--waiters;gate->commandWakeup(&waiters,false);
            return result;
        });
        return created;
    }
    void retireDevice(IOUSBHostDevice *native) {
        if (!native) return;
        gated([&] {
            for (auto *device:devices) if (device && device->native==native) {
                retireSubtree(*device,true);
                break;
            }
            return kIOReturnSuccess;
        });
    }
    bool topologyReady(const Device &device) {
        if (!device.ancestorsReady()) return false;
        for (auto *node=device.parent;node;node=node->parent) {
            const auto *parent=static_cast<const Device *>(node);
            if (!parent->native || !parent->power.running()) return false;
        }
        return true;
    }
    bool ttDomainAlive(const Device &device) {
        return device.ttDomainAlive();
    }
    bool ttBuffersDirty(const Device &device) {
        if (device.enumerationTT.dirty()) return true;
        for (auto *pipe:device.pipes) if (pipe && pipe->ttCleanup.dirty()) return true;
        return false;
    }
    bool ttEnumerationBlocked(const XHCI::SlotConfiguration &candidate) {
        for (auto *entry:devices) if (entry && entry->enumerationTT.blocks(candidate)) return true;
        return false;
    }
    bool ordinaryReady(const Pipe &pipe) {
        return pipe.device->power.running() && !pipe.ttCleanup.dirty() && !pipe.transfers.requiresNativeClear() &&
            ((pipe.id!=1 && pipe.configuration.type!=XHCI::EndpointType::Bulk) ||
                !ttEnumerationBlocked(pipe.device->configuration));
    }
    bool ttParentsReady(const Device &device) {
        if (!ttDomainAlive(device)) return false;
        for (auto *node=device.ttOwner();node;node=node->parent) {
            const auto *parent=static_cast<const Device *>(node);
            // A disappearing full-speed hub below the TT owner must not
            // discard the HS hub's buffers or prevent their direct cleanup.
            if (!parent->native || !parent->power.running() || parent->retiring || !parent->hubRegistered) return false;
        }
        return true;
    }
    void queueEnumerationTT(Device &device,UInt64 now) {
        if (!running || stopping || fault || !ttWork || !ttDomainAlive(device) || !device.enumerationTT.schedule(now)) return;
        ++device.maintenance;++waiters;thread_call_enter(ttWork);
        if (timer) timer->setTimeoutMS(100);
    }
    void retireSubtree(Device &root,bool includeRoot) {
        for (auto *device:devices) if (device && ((includeRoot && device==&root) || device->descendsFrom(root))) {
            device->retiring=true;device->hubRegistered=false;
            for (auto *pipe:device->pipes) if (pipe) cancelRequests(*pipe,kIOReturnAborted);
        }
        if (deviceWork) thread_call_enter(deviceWork);
    }
    IOReturn registerHub(IOUSBHostDevice *native,const XHCI::HubProfile &profile) {
        return gated([&] {
            auto *device=findDevice(native);
            if (!device) return kIOReturnNotFound;
            if (!running || stopping || fault || !topologyReady(*device) || !device->power.running()) return kIOReturnNotReady;
            if (device->busy || device->maintenance || (!device->hubRegistered && device->children)) return kIOReturnBusy;
            if (loop->onThread()) return kIOReturnCannotLock;
            XHCI::SlotConfiguration candidate;
            auto result=hostResult(device->hubCandidate(profile,candidate));
            if (result!=kIOReturnSuccess) return result;
            if (device->configuration.hub) {
                result=hardware([&] { return deviceOutput(*device,device->configuration.entries); });
                if (result==kIOReturnSuccess) device->hubRegistered=true;else fail(result);
                return result;
            }
            const auto *protocol=protocols.get(candidate.rootPort);
            if (!protocol) return kIOReturnBadArgument;
            device->busy=true;++waiters;
            result=hostResult(device->memory.stageHub(candidate,*protocol,commandQueue));
            XHCI::CommandRecord record{};
            if (result==kIOReturnSuccess) {
                XHCI::Command configure;configure.type=XHCI::ConfigureEndpoint;configure.slot=device->slot;
                auto &input=device->memory.inputContext();configure.parameter=input.address();
                result=execute(configure,owner,record,1000000,&input,true);
                if (result==kIOReturnSuccess) result=hardware([&] {
                    XHCI::SlotSnapshot output;
                    return hostResult(device->memory.hubConfigured(io,commandQueue,record.ticket,
                        reinterpret_cast<uintptr_t>(owner),device->slot,device->address,output));
                });
                if (record.ticket && record.phase==XHCI::CommandPhase::Completed) {
                    const auto ack=acknowledgeCommand(record);if (ack!=kIOReturnSuccess) result=ack;
                }
                if (result==kIOReturnSuccess) {
                    device->configuration=candidate;
                    if (stopping || fault || !topologyReady(*device)) result=kIOReturnAborted;
                    else device->hubRegistered=true;
                } else fail(result);
            }
            device->busy=false;--waiters;gate->commandWakeup(&waiters,false);
            if (device->retiring && deviceWork) thread_call_enter(deviceWork);
            return result;
        });
    }
    IOReturn unregisterHub(IOUSBHostDevice *native) {
        return gated([&] {
            auto *device=findDevice(native);
            if (!device) return kIOReturnNotFound;
            if (device->busy) return kIOReturnBusy;
            bool translated=false;
            for (auto *child:devices) if (child && child!=device && device->slot &&
                child->configuration.ttHub==device->slot && child->descendsFrom(*device)) { translated=true;break; }
            device->unregisterHub(translated);
            // The hardware Hub/TT profile and parent allocation remain live
            // until all descendant Disable Slot/unmap operations complete.
            retireSubtree(*device,false);return kIOReturnSuccess;
        });
    }
    IOReturn registerHubDescriptor(IOUSBHostDevice *native,const StandardUSB::HubDescriptor *descriptor,tRegisterHubFlags flags) {
        return gated([&] {
            auto *device=findDevice(native);if (!device) return kIOReturnNotFound;
            XHCI::HubProfile profile;
            const auto result=XHCI::hubProfile(descriptor,device->configuration.speed,unsigned(flags),profile);
            return result==USB::Result::Success ? registerHub(native,profile) : hostResult(result);
        });
    }
    IOReturn registerHubDescriptor(IOUSBHostDevice *native,const StandardUSB::SuperSpeedHubDescriptor *descriptor) {
        return gated([&] {
            auto *device=findDevice(native);if (!device) return kIOReturnNotFound;
            XHCI::HubProfile profile;
            const auto result=XHCI::hubProfile(descriptor,device->configuration.speed,profile);
            return result==USB::Result::Success ? registerHub(native,profile) : hostResult(result);
        });
    }
    Device *findDevice(IOUSBHostDevice *native) {
        for (auto *device:devices) if (device && device->native==native) return device;
        return nullptr;
    }
    Pipe *findPipe(IOUSBHostPipe *native,Device *&device) {
        device=native ? findDevice(native->getDevice()) : nullptr;
        if (device) for (auto *pipe:device->pipes) if (pipe && pipe->native==native) return pipe;
        return nullptr;
    }
    IOReturn submitRequest(AppleUSBHostRequest *native) {
        return gated([&] {
            if (!native || !running || stopping || fault) return kIOReturnNotReady;
            Device *device=nullptr;auto *pipe=findPipe(native->getPipe(),device);
            if (!pipe || native->getDevice()!=device->native || native->getIOSource()!=pipe->native)
                return kIOReturnBadArgument;
            if (pipe->closing || device->retiring) return kIOReturnAborted;
            if (pipe->transfers.requiresNativeClear()) return HostPipeStalled;
            if (pipe->ttCleanup.dirty()) return kIOReturnNotReady;
            if (!device->power.running()) return kIOReturnNotReady;
            if ((pipe->id==1 || pipe->configuration.type==XHCI::EndpointType::Bulk) &&
                ttEnumerationBlocked(device->configuration)) return kIOReturnNotReady;
            const auto *setup=pipe->id==1 ? native->getRequest() : nullptr;
            const bool ttMaintenance=pipe->id==1 && device->maintenance && native->getClient()==device->native->service() &&
                setup && setup->requestType==0x23 && setup->request==8 && !setup->length;
            // Keep one controller entry and one EP0 entry available for the
            // Native hub request that retires a failed child's TT recovery.
            if (device->busy || pipe->requests>=64 || requestCount==256 || (!ttMaintenance &&
                (requestCount>=255 || (pipe->id==1 && pipe->requests>=63)))) return kIOReturnBusy;
            const bool iso=pipe->configuration.type==XHCI::EndpointType::Isochronous;
            // Bundling selects a Native completion policy. Each ordinary
            // request still owns one prepared MD, TD, timeout and recovery.
            // The Family aggregates completions after our DMA retirement.
            if (iso && native->isBundled()) return kIOReturnBadArgument;
            const UInt64 length=native->getIOLength();
            auto *buffer=native->getPreparedBuffer();auto *completer=native->getRequestCompleter();
            if ((!iso && length>Region::MaximumBytes) || (length && !buffer) || (buffer && buffer->getLength()<length) || !completer)
                return kIOReturnBadArgument;
            IOUSBHostIsochronousTransaction *frameList=nullptr;UInt32 frames=0;UInt64 firstFrame=0;
            if (iso) {
                frameList=native->getFrameList();frames=native->getNumFrames();firstFrame=native->getStartFrame();
                if ((frames && !frameList) || reinterpret_cast<uintptr_t>(frameList)>UINTPTR_MAX-UInt64(frames)*sizeof(*frameList))
                    return kIOReturnBadArgument;
                UInt64 last=0;
                const auto valid=XHCI::isochronousTarget(pipe->configuration,firstFrame,frames ? frames-1 : 0,last);
                if (valid!=USB::Result::Success) return hostResult(valid);
            }
            unsigned index=256;
            for (unsigned n=0;n<256;++n) {
                if (requests[n] && requests[n]->native==native) return kIOReturnExclusiveAccess;
                if (!requests[n] && index==256) index=n;
            }
            if (index==256) return kIOReturnNoResources;
            if (pipe->id==1 && (!setup || setup->length!=length)) return kIOReturnBadArgument;
            auto *request=new Request(*memory,pipe,native,completer,buffer,iso ? 0 : UInt32(length));
            if (!request) return kIOReturnNoMemory;
            if (iso) {
                request->isochronous=true;request->iso=XHCI::IsochronousProgress(frames);
                request->frameList=frameList;request->firstFrame=firstFrame;request->phase=RequestPhase::Isochronous;
            }
            if (setup) request->setup={setup->requestType,setup->request,setup->value,setup->index,setup->length};
            const UInt64 timeout=UInt64(native->getCompletionTimeout())*1000;
            if (timeout) request->deadline=hostTimeUS()+timeout;
            native->retain();completer->retain();requests[index]=request;++requestCount;++device->requests;++pipe->requests;
            if (pipe->tail) pipe->tail->next=request;else pipe->head=request;
            pipe->tail=request;
            owner->AppleUSBHostController::nativeSlot216(native,kIOReturnSuccess,0);
            scheduleRequests();if (request->deadline || iso) timer->setTimeoutMS(1);
            return kIOReturnSuccess;
        });
    }
    bool pinnedRequests(const Pipe *pipe=nullptr) {
        for (auto *request:requests) if (request && request->phase==RequestPhase::Pinned &&
            (!pipe || request->pipe==pipe)) return true;
        return false;
    }
    IOReturn abortRequests(IOUSBHostPipe *native,IOReturn reason,IOService *client) {
        return gated([&] {
            Device *device=nullptr;auto *pipe=findPipe(native,device);
            if (!pipe || !reason || (client && pipe->id!=1)) return kIOReturnBadArgument;
            cancelRequests(*pipe,reason,client);return kIOReturnSuccess;
        });
    }
    IOReturn clearStall(IOUSBHostPipe *native) {
        return gated([&] {
            Device *device=nullptr;auto *pipe=findPipe(native,device);
            if (!pipe || !running || stopping || fault || device->retiring || !device->power.running()) return kIOReturnNotReady;
            if (device->busy || device->maintenance || loop->onThread()) return kIOReturnBusy;
            auto result=hardware([&] { return hostResult(pipe->transfers.beginNativeClear()); });
            if (result!=kIOReturnSuccess) return result;
            device->busy=true;++waiters;
            // Control recovery preserves other clients' unpublished FIFO.
            // Bulk/interrupt clearStall aborts all outstanding Native I/O.
            if (pipe->id==1) {
                if (pipe->active) cancelRequest(*pipe->active,kIOReturnAborted);
            } else cancelRequests(*pipe,kIOReturnAborted);
            // Do not wait under HubPort gates for a TT recovery slot or its
            // Native hub I/O. The asynchronous drain owns this continuation.
            if (device->configuration.ttHub && (pipe->id==1 || pipe->configuration.type==XHCI::EndpointType::Bulk) &&
                pipe->transfers.outstanding()) result=kIOReturnNotReady;
            while (result==kIOReturnSuccess && (pipe->id==1 ? pipe->active!=nullptr : pipe->requests!=0) &&
                !pipe->ttRequest && !pinnedRequests(pipe) &&
                !stopping && !fault && !device->retiring) gate->commandSleep(pipe,THREAD_UNINT);
            if (stopping || fault || device->retiring || pinnedRequests(pipe)) result=kIOReturnAborted;
            else if (pipe->ttRequest) result=kIOReturnNotReady;
            // A failed maintenance clear is never excused by Native HubPort's
            // log-and-continue path. Retry asynchronously after its gate exits.
            if (result==kIOReturnSuccess && pipe->ttCleanup.dirty()) {
                queueTT(*pipe);result=kIOReturnNotReady;
            }
            auto candidate=pipe->configuration;
            candidate.dequeue=pipe->backend.ring().nextAddress();candidate.cycle=bool(pipe->backend.ring().producerCycle());
            // Native sends CLEAR_FEATURE only after this HCD stage succeeds.
            // A non-control Drop+Add resets the host data toggle. EP0 keeps its
            // packet size and current dequeue; each SETUP resets its toggle.
            if (result==kIOReturnSuccess) result=configurePipe(*device,*pipe,candidate,pipe->id!=1);
            if (result==kIOReturnSuccess) result=hardware([&] { return hostResult(pipe->transfers.finishNativeClear()); });
            device->busy=false;--waiters;
            gate->commandWakeup(&waiters,false);scheduleRequests();return result;
        });
    }
    void queueTT(Pipe &pipe) {
        if (!ttWork || !pipe.ttCleanup.schedule(hostTimeUS())) return;
        ++pipe.device->maintenance;++waiters;thread_call_enter(ttWork);
    }
    bool powerTaskBusy(Device &device) {
        if (device.maintenance) return true;
        for (auto *request:requests) if (request && request->native && request->pipe &&
            request->pipe->device==&device && request->busy) return true;
        return false;
    }
    IOReturn powerRecover(Pipe &pipe) {
        for (auto *request=pipe.head;request;request=request->next) if (!request->busy &&
            (request->phase==RequestPhase::Recovering || (request->isochronous && pipe.isoRecovering))) {
            request->busy=true;
            const auto result=recoverRequest(*request);
            request->busy=false;gate->commandWakeup(&waiters,false);
            if (pipe.ttCleanup.queued() && ttWork) thread_call_enter(ttWork);
            return request->ttHeld ? kIOReturnBusy : result;
        }
        return pipe.transfers.faulted() ? kIOReturnBusy : kIOReturnSuccess;
    }
    IOReturn pausePipe(Pipe &pipe,Device &device) {
        if (pipe.transfers.paused()) return kIOReturnSuccess;
        unsigned endpointState=0;UInt64 address=0;
        auto snapshot=[&] {
            const auto result=device.memory.deviceContext().endpoint(io,pipe.id,endpointState,address);
            const auto base=pipe.backend.ring().startAddress(),pointer=address&~UInt64(15);
            return result==USB::Result::Success && !(address&14) && pointer>=base &&
                pointer-base<pipe.backend.ring().allocationBytes() ? kIOReturnSuccess : kIOReturnIOError;
        };
        auto result=hardware(snapshot);
        if (result!=kIOReturnSuccess) return result;
        if (endpointState!=1) {
            if (pipe.transfers.outstanding()) {
                cancelRequests(pipe,kIOReturnIOError);
                result=powerRecover(pipe);
                if (result!=kIOReturnSuccess) return result;
                result=hardware(snapshot);
                if (result!=kIOReturnSuccess) return result;
            }
            if (endpointState!=2 && endpointState!=3) return kIOReturnNotReady;
            result=hardware([&] { return hostResult(pipe.transfers.pauseEmpty(hostTimeUS())); });
        } else {
            result=hardware([&] { return hostResult(pipe.transfers.beginPause()); });
            if (result!=kIOReturnSuccess) return result;
            XHCI::Command stop;stop.type=XHCI::StopEndpoint;stop.slot=device.slot;stop.endpoint=pipe.id;stop.option=true;
            XHCI::CommandRecord record;
            result=execute(stop,owner,record,1000000,nullptr,true,&pipe.stopTicket);
            const bool stopSucceeded=result==kIOReturnSuccess;
            if (result==kIOReturnSuccess) result=hardware([&] {
                const auto valid=snapshot();
                if (valid!=kIOReturnSuccess || endpointState!=3) return kIOReturnIOError;
                return hostResult(pipe.transfers.confirmPause(record.ticket,hostTimeUS()));
            });
            if (record.ticket && record.phase==XHCI::CommandPhase::Completed) {
                const auto ack=acknowledgeCommand(record);if (ack!=kIOReturnSuccess) result=ack;
            }
            pipe.stopTicket=0;
            if (result!=kIOReturnSuccess) {
                // A successful SP Stop followed by an invalid output context
                // cannot safely be rolled back as a rejected command: the
                // endpoint may already be stopped and subject to the 10ms
                // minimum. Isolate the controller instead of reopening it.
                if (stopSucceeded) fail(result);
                else pipe.transfers.abandonPause();
            }
        }
        if (result==kIOReturnSuccess && device.power.stopped(hostTimeUS())!=USB::Result::Success) result=kIOReturnIOError;
        // ISO uses Native's abort-on-suspend policy. The SP Stop proves DMA
        // quiescence; a real Set Dequeue retires the cancelled TDs while the
        // queue remains paused. Ordinary requests retain their opaque state.
        if (result==kIOReturnSuccess && pipe.configuration.type==XHCI::EndpointType::Isochronous &&
            pipe.transfers.faulted()) result=powerRecover(pipe);
        return result;
    }
    IOReturn resumeDeviceQueues(Device &device) {
        for (;;) {
            if (stopping || fault || device.retiring) return kIOReturnAborted;
            const auto remaining=device.power.remaining(hostTimeUS());
            if (!remaining) break;
            UInt64 deadline=0;clock_interval_to_deadline(UInt32(remaining),kMicrosecondScale,&deadline);
            gate->commandSleep(&waiters,deadline,THREAD_UNINT);
        }
        UInt64 duration=0;
        if (!device.power.elapsed(hostTimeUS(),duration)) return kIOReturnIOError;
        for (auto *request:requests) if (request && request->native && request->pipe && request->pipe->device==&device &&
            request->deadline && duration>UINT64_MAX-request->deadline) return kIOReturnIOError;
        for (auto *pipe:device.pipes) if (pipe && pipe->transfers.paused()) {
            if (pipe->transfers.faulted()) {
                const auto recovered=powerRecover(*pipe);if (recovered!=kIOReturnSuccess) return recovered;
            }
            const auto result=hardware([&] { return hostResult(pipe->transfers.resume(hostTimeUS(),duration)); });
            if (result!=kIOReturnSuccess) return result;
        }
        for (auto *request:requests) if (request && request->native && request->pipe && request->pipe->device==&device &&
            request->deadline) request->deadline+=duration;
        return device.power.finishResume(hostTimeUS()) ? kIOReturnSuccess : kIOReturnIOError;
    }
    IOReturn setDeviceSuspended(IOUSBHostDevice *native,bool suspend) {
        return gated([&] {
            auto *device=findDevice(native);
            if (!device) return kIOReturnNotFound;
            if (!running || stopping || fault || device->retiring) return kIOReturnNotReady;
            if (device->busy || (suspend && device->maintenance)) return kIOReturnBusy;
            if (loop->onThread()) return kIOReturnCannotLock;
            if (suspend && device->power.suspended()) return kIOReturnSuccess;
            if (!suspend && device->power.running()) return kIOReturnSuccess;
            if (device->power.transitioning()) return kIOReturnBusy;
            auto result=kIOReturnSuccess;
            if (suspend) result=hostResult(device->power.beginSuspend(hostTimeUS()));
            else if (!device->power.beginResume()) result=kIOReturnNotReady;
            if (result!=kIOReturnSuccess) return result;
            device->busy=true;++waiters;
            if (suspend) for (auto *pipe:device->pipes) if (pipe &&
                pipe->configuration.type==XHCI::EndpointType::Isochronous) {
                cancelIsoPipe(*pipe,kIOReturnAborted);
                // Even an empty ISO queue must start a fresh hardware flow
                // after the pause; no stale Frame ID/SIA state is inherited.
                pipe->isoNeedsFreshContext=true;
            }
            scheduleRequests();
            // An already running prepare/recover/cleanup keeps its lease and
            // wakes this fence on exit. New publication is now prohibited.
            while (powerTaskBusy(*device) && !stopping && !fault && !device->retiring)
                gate->commandSleep(&waiters,THREAD_UNINT);
            if (stopping || fault || device->retiring) result=kIOReturnAborted;
            if (result==kIOReturnSuccess && suspend) {
                for (auto *pipe:device->pipes) if (pipe && pipe->transfers.initialized()) {
                    if (pipe->configuration.type!=XHCI::EndpointType::Isochronous && pipe->transfers.faulted())
                        result=powerRecover(*pipe);
                    if (result==kIOReturnSuccess) result=pausePipe(*pipe,*device);
                    if (result!=kIOReturnSuccess || stopping || fault || device->retiring) break;
                }
                if (stopping || fault || device->retiring) result=kIOReturnAborted;
                if (result==kIOReturnSuccess && !device->power.finishSuspend()) result=kIOReturnIOError;
                if (result!=kIOReturnSuccess && running && !stopping && !fault && !device->retiring) {
                    // A rejected suspend must restore endpoints already
                    // stopped, including the SP minimum and timeout fence.
                    if (!device->power.beginResume() || resumeDeviceQueues(*device)!=kIOReturnSuccess) fail(kIOReturnIOError);
                }
            } else if (result==kIOReturnSuccess) {
                result=resumeDeviceQueues(*device);
                if (result!=kIOReturnSuccess) fail(result);
            }
            device->busy=false;--waiters;gate->commandWakeup(&waiters,false);scheduleRequests();
            if (running && !stopping && !fault && timer) timer->setTimeoutMS(1);
            return result;
        });
    }
    IOReturn recoverRequest(Request &request) {
        auto &pipe=*request.pipe;XHCI::CommandRecord stopped{},dequeue{};
        bool complete=false,paused=false,tt=false;
        auto result=gated([&] {
            if (dmaStopped) {
                complete=isolateTransfers() && (request.isochronous || retireRequest(request));
                return complete ? kIOReturnSuccess : kIOReturnIOError;
            }
            tt=!request.isochronous && XHCI::ttCleanupRequired(pipe.device->configuration,pipe.configuration);
            // Bound retained Stop records so TT maintenance and other endpoint
            // recovery retain space in the shared 64-command pool.
            if (tt && ttRecoveryCount>=16) return kIOReturnBusy;
            paused=pipe.transfers.pausedStopProof();
            if (paused) return kIOReturnSuccess;
            return hardware([&] {
                unsigned state=0;UInt64 address=0;
                const auto r=pipe.device->memory.deviceContext().endpoint(io,pipe.id,state,address);
                if (r!=USB::Result::Success) return hostResult(r);
                stopped.command.type=state==2 || state==4 ? XHCI::ResetEndpoint : XHCI::StopEndpoint;
                return state>=1 && state<=4 ? kIOReturnSuccess : kIOReturnIOError;
            });
        });
        if (complete) return result;
        if (result==kIOReturnBusy) return result;
        if (result==kIOReturnSuccess && !paused) {
            stopped.command.slot=pipe.device->slot;stopped.command.endpoint=pipe.id;
            const auto command=stopped.command;
            result=execute(command,owner,stopped,1000000,nullptr,true,&pipe.stopTicket);
        }
        if (result==kIOReturnSuccess && tt) {
            gated([&] {
                request.ttStop=stopped;request.ttPaused=paused;request.ttHeld=true;
                pipe.ttRequest=&request;pipe.ttCleanup.require();
                request.phase=RequestPhase::TTRecovering;++ttRecoveryCount;queueTT(pipe);
                // Native clearStall may be waiting for its drain. It must
                // leave HubPort gates before this Native TT request can run.
                gate->commandWakeup(&pipe,false);return kIOReturnSuccess;
            });
            return kIOReturnSuccess;
        }
        if (result==kIOReturnSuccess) {
            XHCI::Command command;command.type=XHCI::SetDequeue;command.slot=pipe.device->slot;command.endpoint=pipe.id;
            command.parameter=pipe.backend.ring().nextAddress()|pipe.backend.ring().producerCycle();
            result=execute(command,owner,dequeue,1000000,nullptr,true);
        }
        return gated([&] { return completeRecovery(request,stopped,dequeue,paused,result); });
    }
    IOReturn completeRecovery(Request &request,const XHCI::CommandRecord &stopped,
        const XHCI::CommandRecord &dequeue,bool paused,IOReturn result) {
        auto &pipe=*request.pipe;
        if (result==kIOReturnSuccess && !dmaStopped) result=hardware([&] {
            return hostResult(paused ? pipe.transfers.recoverPaused(dequeue.ticket) :
                pipe.transfers.recover(stopped.ticket,dequeue.ticket));
        });
        // The queue inspected the actual retained command records above.
        // Only then may their controller retains return to the worker.
        if (stopped.ticket && stopped.phase==XHCI::CommandPhase::Completed) {
            const auto ack=acknowledgeCommand(stopped);if (ack!=kIOReturnSuccess) result=ack;
        }
        if (dequeue.ticket && dequeue.phase==XHCI::CommandPhase::Completed) {
            const auto ack=acknowledgeCommand(dequeue);if (ack!=kIOReturnSuccess) result=ack;
        }
        if (result!=kIOReturnSuccess) fail(result);
        if (dmaStopped && !isolateTransfers()) result=kIOReturnIOError;
        const bool retired=request.isochronous ? retireIsoTransfers(pipe,true) :
            request.phase==RequestPhase::Completing || retireRequest(request);
        if (!retired) {
            request.phase=RequestPhase::Pinned;request.invariantPinned=true;owner->pin();result=kIOReturnIOError;
        }
        pipe.stopTicket=0;
        if (request.isochronous && retired) {
            pipe.isoRecovering=false;pipe.isoNeedsFreshContext=!dmaStopped;
            if (dmaStopped) pipe.isoFlow.ended();
        }
        return result;
    }
    void detachRequest(Request &request) {
        auto &pipe=*request.pipe;
        Request **link=&pipe.head;Request *previous=nullptr;
        while (*link && *link!=&request) { previous=*link;link=&(*link)->next; }
        if (*link) *link=request.next;
        if (pipe.tail==&request) pipe.tail=previous;
        if (pipe.active==&request) pipe.active=nullptr;
        --pipe.requests;--pipe.device->requests;
        request.handoff=request.native;request.native=nullptr;request.pipe=nullptr;request.phase=RequestPhase::Enqueuing;
        gate->commandWakeup(&pipe,false);
        if (pipe.closing && closeWork) thread_call_enter(closeWork);
        if (pipe.device->retiring && deviceWork) thread_call_enter(deviceWork);
    }
    IOReturn pipeWork(Pipe &pipe,bool release) {
        pipe.workRelease=release;pipe.workDone=false;pipe.workQueued=true;
        thread_call_enter(deviceWork);
        // The caller holds the device's busy lease and an outer waiter. Stop
        // must let the worker finish before either this pipe or its API dies.
        while (!pipe.workDone) gate->commandSleep(&pipe,THREAD_UNINT);
        return hostResult(pipe.workResult);
    }
    IOReturn deviceOutput(Device &device,unsigned entries) {
        XHCI::SlotSnapshot output{};
        const auto r=device.memory.deviceContext().slot(io,output);
        if (r!=USB::Result::Success) return hostResult(r);
        const auto &c=device.configuration;
        auto expected=c;expected.entries=entries;
        return output.address==device.address && XHCI::sameSlotConfiguration(output,expected) &&
            output.state>=2 ? kIOReturnSuccess : kIOReturnIOError;
    }
    IOReturn configureTransferQueue(Device &device,Pipe &pipe) {
        return hardware([&] {
            const auto result=hostResult(pipe.transfers.initialized() ? pipe.transfers.reconfigure(pipe.configuration) :
                pipe.transfers.initialize(epoch,device.slot,pipe.configuration,core.dmaBits(),reinterpret_cast<uintptr_t>(owner)));
            // Non-control callers have verified a fresh Configure Endpoint
            // output context, including an actual Drop+Add on replacement.
            if (result==kIOReturnSuccess && pipe.configuration.type==XHCI::EndpointType::Isochronous) {
                pipe.isoFlow.ended();pipe.isoNeedsFreshContext=false;
            }
            return result;
        });
    }
    IOReturn configurePipe(Device &device,Pipe &pipe,const XHCI::EndpointConfiguration &candidate,bool replace=false) {
        auto &input=device.memory.inputContext();
        XHCI::CommandRecord record;
        if (pipe.id==1) {
            if (candidate.maxPacket==device.memory.ep0Configuration().maxPacket) {
                pipe.configuration=device.memory.currentControlConfiguration();return configureTransferQueue(device,pipe);
            }
            auto result=hostResult(device.memory.stageControlPacket(candidate.maxPacket,commandQueue.lastTicket()));
            if (result!=kIOReturnSuccess) return result;
            XHCI::Command evaluate;evaluate.type=XHCI::EvaluateContext;evaluate.slot=device.slot;evaluate.parameter=input.address();
            result=execute(evaluate,owner,record,1000000,&input);
            if (result==kIOReturnSuccess) result=hardware([&] {
                auto r=device.memory.evaluatedControl(io,record,device.slot);
                if (r!=USB::Result::Success) return hostResult(r);
                return deviceOutput(device,device.configuration.entries);
            });
            if (result==kIOReturnSuccess) {
                pipe.configuration=device.memory.currentControlConfiguration();result=configureTransferQueue(device,pipe);
            }
            if (result!=kIOReturnSuccess) fail(result);
            return result;
        }
        const auto *protocol=protocols.get(device.configuration.rootPort);
        if (!protocol) return kIOReturnBadArgument;
        auto result=hostResult(XHCI::pipeInput(input,device.configuration,*protocol,device.endpoints,pipe.id,&candidate,replace));
        if (result!=kIOReturnSuccess) return result;
        if (!replace) result=hardware([&] {
            return hostResult(pipe.memory.publish(epoch,device.slot,pipe.id,input.address(),commandQueue.lastTicket()));
        });
        if (result==kIOReturnSuccess) {
            XHCI::Command configure;configure.type=XHCI::ConfigureEndpoint;configure.slot=device.slot;configure.parameter=input.address();
            result=execute(configure,owner,record,1000000,&input);
        }
        const auto endpoints=device.endpoints|(1u<<pipe.id);
        if (result==kIOReturnSuccess) result=hardware([&] {
            auto r=pipe.memory.configured(io,record,input,device.memory.deviceContext(),candidate,replace);
            if (r!=USB::Result::Success) return hostResult(r);
            return deviceOutput(device,XHCI::contextEntries(endpoints));
        });
        if (result==kIOReturnSuccess) {
            pipe.configuration=candidate;device.endpoints=endpoints;
            device.configuration.entries=XHCI::contextEntries(endpoints);
            result=configureTransferQueue(device,pipe);
        }
        if (result!=kIOReturnSuccess) fail(result);
        return result;
    }
    IOReturn removePipe(Device &device,Pipe &pipe) {
        if (!pipe.ttCleanup.canRelease(pipe.requests,ttDomainAlive(device),dmaStopped)) return kIOReturnNotReady;
        auto result=kIOReturnSuccess;
        if (pipe.id!=1 && pipe.memory.currentState()==USB::DMAState::Owned && !dmaStopped) {
            auto &input=device.memory.inputContext();
            const auto *protocol=protocols.get(device.configuration.rootPort);
            result=protocol ? hostResult(XHCI::pipeInput(input,device.configuration,*protocol,
                device.endpoints|(1u<<pipe.id),pipe.id,nullptr)) : kIOReturnBadArgument;
            XHCI::CommandRecord record;
            if (result==kIOReturnSuccess) {
                XHCI::Command drop;drop.type=XHCI::ConfigureEndpoint;drop.slot=device.slot;drop.parameter=input.address();
                result=execute(drop,owner,record,1000000,&input);
            }
            if (result==kIOReturnSuccess) result=hardware([&] {
                const auto r=pipe.memory.removed(io,record,input,device.memory.deviceContext(),epoch);
                if (r!=USB::Result::Success) return hostResult(r);
                return deviceOutput(device,XHCI::contextEntries(device.endpoints&~(1u<<pipe.id)));
            });
            if (result==kIOReturnSuccess) {
                device.endpoints&=~(1u<<pipe.id);device.configuration.entries=XHCI::contextEntries(device.endpoints);
            } else fail(result);
        }
        if (dmaStopped) pipe.memory.retireAfterIsolation();
        if (pipe.memory.currentState()==USB::DMAState::Owned || pipe.memory.currentState()==USB::DMAState::Quarantined) {
            pipe.memory.quarantine();owner->pin();return result==kIOReturnSuccess ? kIOReturnNotReady : result;
        }
        const auto cleanup=pipeWork(pipe,true);
        if (cleanup!=kIOReturnSuccess) { fail(cleanup);owner->pin();return cleanup; }
        device.pipes[pipe.id]=nullptr;
        if (pipe.native) deferRelease(reinterpret_cast<uintptr_t>(pipe.native));
        delete &pipe;
        return result;
    }
    IOReturn createPipe(IOUSBHostPipe::StandardUSBDescriptors &descriptors,IOUSBHostDevice *native,
        IOUSBHostInterface *interface,IOUSBHostPipe *&out) {
        out=nullptr;
        return gated([&] {
            auto *device=findDevice(native);
            if (!running || stopping || fault || !device || device->retiring || loop->onThread()) return kIOReturnNotReady;
            if (!device->power.running()) return kIOReturnNotReady;
            if (device->busy || device->maintenance || releaseCount>=64) return kIOReturnBusy;
            XHCI::EndpointConfiguration candidate;
            // This first pass validates the wire fields before allocating any
            // DMA. The worker binds the real ring address in the second pass.
            auto result=hostResult(XHCI::pipeConfiguration(descriptors,device->configuration.speed,4096,core.dmaBits(),candidate));
            if (result!=kIOReturnSuccess) return result;
            const auto id=XHCI::endpointID(candidate.address,candidate.type);
            if (device->pipes[id]) return kIOReturnExclusiveAccess;
            auto *pipe=new Pipe(*this,*memory,device,descriptors,candidate,&device->memory.ep0Ring());
            if (!pipe) return kIOReturnNoMemory;
            device->pipes[id]=pipe;device->busy=true;++waiters;
            result=pipeWork(*pipe,false);
            if (stopping || fault || device->retiring) result=kIOReturnAborted;
            if (result==kIOReturnSuccess) result=configurePipe(*device,*pipe,pipe->configuration);
            if (result==kIOReturnSuccess && !stopping && !fault && !device->retiring) {
                auto *created=OSTypeAlloc(IOUSBHostPipe);
                if (!created) result=kIOReturnNoMemory;
                else if (!created->initWithDescriptorsAndOwners(descriptors.descriptor,descriptors.ssCompanionDescriptor,
                    owner,native,interface,native->getSpeed(),UInt16(device->address)) ||
                    !nub->accepts(owner,epoch) || created->getDevice()!=native ||
                    created->getSpeed()!=native->getSpeed() || created->getDeviceAddress()!=device->address) {
                    deferRelease(reinterpret_cast<uintptr_t>(created));result=kIOReturnIOError;
                } else {
                    // Native preserves the caller's original descriptor
                    // pointers and its own policy copies. Never pass pointers
                    // into this HCD side table to a longer-lived Native pipe.
                    created->retain();pipe->native=created;out=created;
                }
            } else if (result==kIOReturnSuccess) result=kIOReturnAborted;
            if (!out) {
                const auto cleanup=removePipe(*device,*pipe);
                if (cleanup!=kIOReturnSuccess) result=cleanup;
            }
            device->busy=false;--waiters;gate->commandWakeup(&waiters,false);
            return result;
        });
    }
    IOReturn adjustPipe(IOUSBHostPipe *native,IOUSBHostPipe::StandardUSBDescriptors &descriptors) {
        return gated([&] {
            Device *device=nullptr;auto *pipe=findPipe(native,device);
            if (!running || stopping || fault || !pipe || device->retiring || loop->onThread()) return kIOReturnNotReady;
            if (!device->power.running()) return kIOReturnNotReady;
            if (pipe->closing || pipe->ttCleanup.dirty() || device->busy || device->maintenance ||
                pipe->requests || pipe->transfers.retained() || releaseCount>=64)
                return kIOReturnBusy;
            // The side-table fence also covers DMA cleanup and Native
            // completion handoff, after the final TD left the hardware ring.
            const auto pending=pipe->id==1 ? device->memory.ep0Ring().pending() : pipe->memory.ring().pending();
            if (pending) return kIOReturnBusy;
            XHCI::EndpointConfiguration candidate;
            auto result=hostResult(XHCI::pipeConfiguration(descriptors,device->configuration.speed,
                pipe->id==1 ? pipe->configuration.dequeue : pipe->memory.ring().nextAddress(),core.dmaBits(),candidate));
            if (result!=kIOReturnSuccess) return result;
            if (pipe->id!=1) candidate.cycle=bool(pipe->memory.ring().producerCycle());
            if (candidate.address!=pipe->configuration.address || candidate.type!=pipe->configuration.type)
                return kIOReturnBadArgument;
            device->busy=true;++waiters;
            result=configurePipe(*device,*pipe,candidate,true);
            device->busy=false;--waiters;gate->commandWakeup(&waiters,false);return result;
        });
    }
    IOReturn closePipe(IOUSBHostPipe *native) {
        return gated([&] {
            Device *device=nullptr;auto *pipe=findPipe(native,device);
            if (!pipe) return kIOReturnNotFound;
            if (pipe->closing) return pipe->closeFailed ? kIOReturnIOError : kIOReturnSuccess;
            if (device->busy || !closeWork) return kIOReturnBusy;
            if (pipe->id==1 && device->hubRegistered) {
                // Closing the hub's control pipe ends its registration and
                // child admission. A translated HS hub cannot reregister in
                // this slot lifetime; its old TT state cannot reach a new
                // child after the control pipe has disappeared.
                const auto result=unregisterHub(device->native);
                if (result!=kIOReturnSuccess) return result;
            }
            // The side table owns the Native pipe until the independent
            // close worker drains requests and TT maintenance. Returning
            // here releases Native locks needed by the hub's EP0 request.
            pipe->closing=true;cancelRequests(*pipe,kIOReturnAborted);
            if (pipe->ttCleanup.dirty()) queueTT(*pipe);
            thread_call_enter(closeWork);return kIOReturnSuccess;
        });
    }
    bool destroyDevice(Device &device) {
        bool isolated=false;IOReturn result=kIOReturnSuccess;
        XHCI::CommandRecord record;
        gated([&] { isolated=dmaStopped;return kIOReturnSuccess; });
        if (!isolated && device.slot) {
            XHCI::Command disable;disable.type=XHCI::DisableSlot;disable.slot=device.slot;
            result=execute(disable,owner,record,1000000);
            if (result==kIOReturnSuccess) result=gated([&] {
                return hardware([&] {
                    auto *dcbaa=reinterpret_cast<UInt64 *>(static_cast<UInt8 *>(arena->bytes())+DCBAAOffset);
                    dcbaa[device.slot]=0;io.dmaWriteBarrier();
                    // Even an Enable-only failure has a slot to disable, but
                    // its allocation may not yet have been published to DMA.
                    auto r=device.memory.currentState()==USB::DMAState::Ready ? USB::Result::Success :
                        device.memory.disabled(record,device.slot,epoch);
                    for (auto *pipe:device.pipes) if (r==USB::Result::Success && pipe &&
                        pipe->memory.currentState()==USB::DMAState::Owned) r=pipe->memory.disabledSlot(record,epoch);
                    return hostResult(r);
                });
            });
        }
        if (result!=kIOReturnSuccess) gated([&] { fail(result);isolated=dmaStopped;return kIOReturnSuccess; });
        if (isolated) {
            device.memory.retireAfterIsolation();
            for (auto *pipe:device.pipes) if (pipe) pipe->memory.retireAfterIsolation();
        }
        if (!isolated && result!=kIOReturnSuccess) {
            device.memory.quarantine();for (auto *pipe:device.pipes) if (pipe) pipe->memory.quarantine();return false;
        }
        // Sleeping unmap/complete operations execute on this worker only.
        bool clean=true;
        for (auto *pipe:device.pipes) if (pipe && pipe->memory.release()!=USB::Result::Success) clean=false;
        if (device.memory.release()!=USB::Result::Success) clean=false;
        return clean;
    }
    bool quiesce() {
        if (!gate || !gateAdded) return stopDMA();
        bool clean=false;
        gated([&] {
            stopping=true;
            if (irq) irq->disable();
            if (timer) timer->cancelTimeout();
            commandQueue.quarantine(USB::Result::Aborted);
            clean=stopDMA();
            for (auto *request:requests) if (request && request->native) cancelRequest(*request,kIOReturnAborted);
            if (dmaStopped && !isolateTransfers()) clean=false;
            gate->commandWakeup(&commandQueue,false);
            gate->commandWakeup(&waiters,false);
            for (auto *p:ports) if (p && p->busy) gate->commandWakeup(p,false);
            if (loop->onThread() && (waiters || requestCount)) { clean=false;return kIOReturnCannotLock; }
            while (waiters || requestCount) {
                if (pinnedRequests()) { clean=false;break; }
                gate->commandSleep(&waiters,THREAD_UNINT);
            }
            return kIOReturnSuccess;
        });
        return clean;
    }
    bool removeEvents() {
        if (!quiesce()) return false;
        if (irqAdded) { loop->removeEventSource(irq);irqAdded=false; }
        if (timerAdded) { loop->removeEventSource(timer);timerAdded=false; }
        if (requestWork) { thread_call_cancel_wait(requestWork);thread_call_free(requestWork);requestWork=nullptr; }
        if (ttWork) { thread_call_cancel_wait(ttWork);thread_call_free(ttWork);ttWork=nullptr; }
        if (closeWork) { thread_call_cancel_wait(closeWork);thread_call_free(closeWork);closeWork=nullptr; }
        if (deviceWork) { thread_call_cancel_wait(deviceWork);thread_call_free(deviceWork);deviceWork=nullptr; }
        if (commandReleaseWork) {
            thread_call_cancel_wait(commandReleaseWork);thread_call_free(commandReleaseWork);commandReleaseWork=nullptr;
        }
        if (releaseWork) { thread_call_cancel_wait(releaseWork);thread_call_free(releaseWork);releaseWork=nullptr; }
        if (gate && gateAdded) {
            const auto result=gated([&] {
                if (commandQueue.generation()) {
                    if (commandQueue.isolate()!=USB::Result::Success) return kIOReturnIOError;
                    XHCI::CommandRecord record;
                    for (unsigned i=0;i<64;++i) if (commandQueue.copyRetained(i,record)) {
                        const auto r=acknowledgeCommand(record);
                        if (r!=kIOReturnSuccess) return r;
                    }
                }
                return kIOReturnSuccess;
            });
            if (result!=kIOReturnSuccess) return false;
            loop->removeEventSource(gate);gateAdded=false;
        }
        // No producer remains and the worker is joined. Final releases run
        // on the stop thread, after dropping the controller and role gates.
        while (releaseCount) releaseObjects[--releaseCount]->release();
        while (controllerReleases) { --controllerReleases;owner->release(); }
        OSSafeReleaseNULL(timer);OSSafeReleaseNULL(irq);OSSafeReleaseNULL(gate);OSSafeReleaseNULL(loop);
        return true;
    }
    bool release() {
        if ((loop && loop->inGate()) || (nub->getWorkLoop() && nub->getWorkLoop()->inGate())) return false;
        if (!removeEvents()) return false;
        // Quiesce has joined creators and workers, and halted the controller.
        // No device allocation can now race teardown of the shared DMA policy.
        bool devicesClean=true;
        // Keep the same parent lease rule after global isolation. A failed
        // leaf unmap must not leave a surviving child's parent pointer stale.
        // Each allocation is attempted once per release invocation.
        for (unsigned level=6;level;--level) for (auto *&device:devices) if (device &&
            XHCI::routeDepth(device->configuration.route)==level-1) {
            if (device->children) { devicesClean=false;continue; }
            if (dmaStopped) device->memory.retireAfterIsolation();
            bool clean=true;
            for (auto *pipe:device->pipes) if (pipe) {
                if (dmaStopped) pipe->memory.retireAfterIsolation();
                if (pipe->memory.release()!=USB::Result::Success) clean=false;
            }
            if (device->memory.release()!=USB::Result::Success) clean=false;
            if (!clean || device->detach()!=USB::Result::Success) { devicesClean=false;continue; }
            for (auto *pipe:device->pipes) if (pipe) {
                if (pipe->native) pipe->native->release();
                delete pipe;
            }
            if (device->native) { device->native->service()->release();device->native=nullptr; }
            if (device->slot) slotDevices[device->slot]=nullptr;
            delete device;device=nullptr;
        }
        if (!devicesClean) return false;
        if (published) {
            // Revocation can make runHardwareAction unavailable. Only the
            // provider's completed hardware isolation may replace our halt.
            bool isolated=stopDMA();
            if (!isolated) {
                if (arena) arena->quarantine();
                for (auto *page:scratchpads) if (page) page->quarantine();
                return false;
            }
            if (arena) arena->retireAfterIsolation();
            for (auto *page:scratchpads) if (page) page->retireAfterIsolation();
            published=configured=false;
        }
        // DMA completion/unmapping/release can sleep; no native or role gate
        // may be held here. Failed operations retain their own bookkeeping.
        bool clean=true;
        for (auto *&page:scratchpads) if (page) {
            if (page->release()!=USB::Result::Success) clean=false;
            else { delete page;page=nullptr; }
        }
        if (arena) {
            if (arena->release()!=USB::Result::Success) clean=false;
            else { delete arena;arena=nullptr; }
        }
        if (!clean) return false;
        for (auto *&p:ports) { delete p;p=nullptr; }
        delete memory;memory=nullptr;
        return true;
    }
};

bool QcomUSBHost::start(IOService *provider) {
    if (fState || fFrameProvider || fPinned || fFamilyStarted) return false;
    auto *nub=OSDynamicCast(QcomUSBController,provider);
    if (!nub || nub->role()!=USB::Role::Host || !nub->open(this)) return false;
    fFrameUpdateLock=IOSimpleLockAlloc();
    if (!fFrameUpdateLock) { nub->close(this);return false; }
    nub->retain();__atomic_store_n(&fFrameProvider,nub,__ATOMIC_RELEASE);
    struct Arguments { QcomUSBHost *owner;QcomUSBController *nub;State *state=nullptr; } args{this,nub};
    const auto created=nub->runHardwareAction(this,nub->epoch(),
        [](IOService *,USB::MMIO &io,void *context)->IOReturn {
            auto &a=*static_cast<Arguments *>(context);
            a.state=new State(a.owner,a.nub,io);
            return a.state ? kIOReturnSuccess : kIOReturnNoMemory;
        },&args);
    if (created!=kIOReturnSuccess) { nub->close(this);return false; }
    fState=args.state;fState->opened=true;
    if (fState->probe()!=kIOReturnSuccess || fState->reset()!=kIOReturnSuccess ||
        fState->allocate()!=kIOReturnSuccess || fState->configure()!=kIOReturnSuccess) {
        releaseHardware();return false;
    }
    // Native registration creates the shared AppleUSBHostBusCurrentPool
    // from these OSNumbers before initializing any port allocator. The
    // connector budget belongs to this lease and cold-PM generation.
    if (fState->current.configure(fState->resources)!=USB::Result::Success ||
        !fState->current.controller([&](const char *key,UInt32 value) {
            return setProperty(key,value,32);
        })) { releaseHardware();return false; }
    // The base may call our virtual stop on partial-start failure. Record
    // entry first and let that path perform the one corresponding base stop.
    fFamilyStarted=true;
    if (!AppleUSBHostController::start(provider)) {
        if (fFamilyStarted) stop(provider);
        return false;
    }
    if (!nub->accepts(this,fState->epoch) || fState->installEvents()!=kIOReturnSuccess ||
        fState->run()!=kIOReturnSuccess) { stop(provider);return false; }
    // Verify the actual command/event path before any Native port can expose
    // a device. Neither a delay nor an MMIO read substitutes for completion.
    XHCI::CommandRecord record;
    if (fState->execute({},this,record,100000)!=kIOReturnSuccess || record.completionCode!=1) {
        stop(provider);return false;
    }
    // Controller context retention is not part of this platform's PM
    // contract. Keep the Native controller active while it is published;
    // QcomUSB owns cold system suspend and drains/recreates this entire nub.
    if (!setProperty("USBPowerPolicy","PlatformColdSuspend") || !setProperty("UsbStreamsSupported",false)) {
        stop(provider);return false;
    }
    // Native powerStateDidChangeTo calls createPorts once, under its command
    // gate, then broadcasts kUSBHostMessagePortsCreated to the real ports.
    // PMinit/registration happens after registerService; createPorts submits
    // our active-state vote after Native has reset the driver client to0.
    registerService();
    return true;
}
bool QcomUSBHost::releaseHardware() {
    if (!fState) return true;
    if (!fState->release()) { pin();return false; }
    if (fState->opened) { fState->nub->close(this);fState->opened=false; }
    delete fState;fState=nullptr;
    const auto state=IOSimpleLockLockDisableInterrupt(fFrameUpdateLock);
    const bool unpin=fPinned;fPinned=false;
    IOSimpleLockUnlockEnableInterrupt(fFrameUpdateLock,state);
    if (unpin) release();
    return true;
}
void QcomUSBHost::pin() {
    if (!fFrameUpdateLock) return; // all hardware ownership follows lock creation
    const auto state=IOSimpleLockLockDisableInterrupt(fFrameUpdateLock);
    const bool first=!fPinned;
    if (first) {retain();fPinned=true;}
    IOSimpleLockUnlockEnableInterrupt(fFrameUpdateLock,state);
    if (first) IOLog("QcomUSBHost: retaining controller after DMA cleanup failure\n");
    if (fState) fState->nub->reportFault(this,fState->epoch);
}
void QcomUSBHost::stop(IOService *provider) {
    // Do not relinquish the generation's provider while the Family unwinds
    // its ports, devices and workloops. No hardware allocation is yet released.
    if (fState && !fState->quiesce()) { pin();return; }
    if (fFamilyStarted) { fFamilyStarted=false;AppleUSBHostController::stop(provider); }
    releaseHardware();
}
void QcomUSBHost::free() {
    if (fFamilyStarted || !releaseHardware()) { pin();return; }
    if (fFrameUpdateLock) { IOSimpleLockFree(fFrameUpdateLock);fFrameUpdateLock=nullptr; }
    OSSafeReleaseNULL(fFrameProvider);
    AppleUSBHostController::free();
}
UInt64 QcomUSBHost::getRawMicroFrameCount() {
    auto *provider=__atomic_load_n(&fFrameProvider,__ATOMIC_ACQUIRE);
    UInt32 raw=0;
    if (provider) {
        const auto result=provider->readHostFrameCounter(this,provider->epoch(),raw);
        if (result==kIOReturnSuccess) {
            __atomic_store_n(&fLastMicroFrame,raw,__ATOMIC_RELEASE);return raw;
        }
        if (result==kIOReturnIOError) __atomic_store_n(&fFrameReadFailed,true,__ATOMIC_RELEASE);
    }
    return __atomic_load_n(&fLastMicroFrame,__ATOMIC_ACQUIRE);
}
IOReturn QcomUSBHost::powerDown(unsigned long state) {
    if (state>2) return kIOReturnBadArgument;
    // Audited Native powerDown clears rollover time at state2 and both
    // counters at state0. Serialize those stores with the IRQ observer. The
    // base body only performs scalar stores/CAS and cannot sleep or call us.
    if (!fFrameUpdateLock) return AppleUSBHostController::powerDown(state);
    const auto irq=IOSimpleLockLockDisableInterrupt(fFrameUpdateLock);
    const auto result=AppleUSBHostController::powerDown(state);
    IOSimpleLockUnlockEnableInterrupt(fFrameUpdateLock,irq);return result;
}
IOReturn QcomUSBHost::powerUp(unsigned long state) {
    if (state<1 || state>3) return kIOReturnBadArgument;
    if (!fFrameUpdateLock) return AppleUSBHostController::powerUp(state);
    const auto irq=IOSimpleLockLockDisableInterrupt(fFrameUpdateLock);
    const auto result=AppleUSBHostController::powerUp(state);
    IOSimpleLockUnlockEnableInterrupt(fFrameUpdateLock,irq);return result;
}
UInt64 QcomUSBHost::makeFrameNumber(UInt64 rollovers,UInt64 raw) const {
    return XHCI::frameNumber(rollovers,raw);
}
UInt64 QcomUSBHost::getFrameNumber(UInt64 *stamp) {
    // A level source can remain masked while its action waits for the Family
    // gate. Refresh pending wrap records before Native takes its seqlock
    // snapshot, including when the query itself comes from interrupt context.
    observeFrameEvents();return AppleUSBHostController::getFrameNumber(stamp);
}
UInt64 QcomUSBHost::getFrameRollover(UInt64 *stamp) {
    observeFrameEvents();return AppleUSBHostController::getFrameRollover(stamp);
}
USB::Result QcomUSBHost::recordFrameRollover(UInt32 raw) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
    static_assert(__builtin_offsetof(QcomUSBHost,fFrameRolloverTime)==1168,"Native rollover timestamp");
    static_assert(__builtin_offsetof(QcomUSBHost,fFrameRolloverBegin)==1176,"Native rollover writer count");
    static_assert(__builtin_offsetof(QcomUSBHost,fFrameRolloverEnd)==1184,"Native rollover reader count");
#pragma clang diagnostic pop
    UInt64 now=0,stamp=0,absolute=0;
    absolutetime_to_nanoseconds(mach_absolute_time(),&now);
    if (!XHCI::rolloverNanoseconds(now,raw,stamp) || fFrameRolloverBegin==UINT64_MAX ||
        fFrameRolloverBegin!=fFrameRolloverEnd) return USB::Result::HardwareError;
    nanoseconds_to_absolutetime(stamp,&absolute);
    // Native readers spin when begin!=end. Keep local interrupts disabled
    // across publication so a frame query cannot preempt this writer and
    // wait for a publication that only this CPU can finish.
    const auto next=fFrameRolloverBegin+1;
    __atomic_store_n(&fFrameRolloverBegin,next,__ATOMIC_SEQ_CST);
    __atomic_store_n(&fFrameRolloverTime,absolute,__ATOMIC_SEQ_CST);
    __atomic_store_n(&fFrameRolloverEnd,next,__ATOMIC_SEQ_CST);
    return USB::Result::Success;
}
bool QcomUSBHost::beginFrameEvents(XHCI::TRB *memory,unsigned entries,UInt64 epoch) {
    if (!fFrameUpdateLock) return false;
    const auto state=IOSimpleLockLockDisableInterrupt(fFrameUpdateLock);
    const auto result=fFrameEvents.initialize(memory,entries,epoch);
    IOSimpleLockUnlockEnableInterrupt(fFrameUpdateLock,state);return result;
}
void QcomUSBHost::endFrameEvents() {
    if (!fFrameUpdateLock) return;
    const auto state=IOSimpleLockLockDisableInterrupt(fFrameUpdateLock);
    fFrameEvents.close();
    IOSimpleLockUnlockEnableInterrupt(fFrameUpdateLock,state);
}
bool QcomUSBHost::observedFrameRollover(UInt64 serial) {
    if (!fFrameUpdateLock) return false;
    const auto state=IOSimpleLockLockDisableInterrupt(fFrameUpdateLock);
    const bool observed=fFrameEvents.active() && serial && serial<=fFrameEvents.observedWraps();
    IOSimpleLockUnlockEnableInterrupt(fFrameUpdateLock,state);return observed;
}
USB::Result QcomUSBHost::observeFrameEvents() {
    if (!fFrameUpdateLock) return USB::Result::Success;
    const auto state=IOSimpleLockLockDisableInterrupt(fFrameUpdateLock);
    USB::Result result=USB::Result::Success;
    auto *provider=__atomic_load_n(&fFrameProvider,__ATOMIC_ACQUIRE);
    if (provider && fFrameEvents.active()) {
        USB::DMAOrdering order;
        result=fFrameEvents.observe(provider->epoch(),order,[&] {
            UInt32 raw=0;
            const auto read=provider->readHostFrameCounter(this,provider->epoch(),raw);
            if (read!=kIOReturnSuccess) return read==kIOReturnNotReady ? USB::Result::Aborted : USB::Result::HardwareError;
            __atomic_store_n(&fLastMicroFrame,raw,__ATOMIC_RELEASE);
            return recordFrameRollover(raw);
        });
        if (result!=USB::Result::Success && result!=USB::Result::Aborted)
            __atomic_store_n(&fFrameReadFailed,true,__ATOMIC_RELEASE);
    }
    IOSimpleLockUnlockEnableInterrupt(fFrameUpdateLock,state);return result;
}
IODMACommand *QcomUSBHost::getDMACommand(UInt32 alignment,UInt32) {
    // Native request preparation supplies this command. It describes CPU
    // client memory only; all actual xHCI DMA uses the policy in State.
    if (alignment && (alignment&(alignment-1))) return nullptr;
    return QcomUSBHostDMACommand::withClientSpecification(USB::DMARegion<USB::IOMemoryAPI>::MaximumBytes);
}
bool QcomUSBHost::interruptFilter(OSObject *owner,IOFilterInterruptEventSource *) {
    // Bounded read-only event observation updates the Native clock before
    // the Family action thread runs. No State access or sleeping gate.
    static_cast<QcomUSBHost *>(owner)->observeFrameEvents();
    return true;
}
bool QcomUSBHost::filterOccurred(IOFilterInterruptEventSource *) { observeFrameEvents();return true; }
void QcomUSBHost::interruptAction(OSObject *object,IOInterruptEventSource *source,int count) {
    static_cast<QcomUSBHost *>(object)->interruptOccurred(source,count);
}
void QcomUSBHost::interruptOccurred(IOInterruptEventSource *,int) {
    if (fState) fState->poll();
}
void QcomUSBHost::timerAction(OSObject *object,IOTimerEventSource *) {
    auto *host=static_cast<QcomUSBHost *>(object);
    if (host->fState) host->fState->poll();
}
void QcomUSBHost::releaseWorker(void *context,void *) {
    auto &state=*static_cast<State *>(context);
    for (;;) {
        OSObject *objects[64]{};unsigned count=0;bool changed[16]{};bool notify=false;
        state.gated([&] {
            while (count<64 && state.releaseCount) objects[count++]=state.releaseObjects[--state.releaseCount];
            for (unsigned n=1;n<=state.core.maxPorts();++n) if (state.changedPorts[n]) {
                state.changedPorts[n]=false;changed[n]=true;notify=true;
            }
            return kIOReturnSuccess;
        });
        if (!count && !notify) return;
        for (unsigned i=0;i<count;++i) objects[i]->release();
        for (unsigned n=1;n<=state.core.maxPorts();++n) if (changed[n]) state.owner->signalPort(n);
    }
}
void QcomUSBHost::commandReleaseWorker(void *context,void *) {
    auto &state=*static_cast<State *>(context);
    for (;;) {
        unsigned count=0;
        state.gated([&] { count=state.controllerReleases;state.controllerReleases=0;return kIOReturnSuccess; });
        if (!count) return;
        // A Native pipe/device final release may wait for its own workloop.
        // Command retains use an independent worker so that such a wait
        // cannot prevent the Stop/Set Dequeue recovery needed by that loop.
        while (count--) state.owner->release();
    }
}
void QcomUSBHost::ttWorker(void *context,void *) {
    auto &state=*static_cast<State *>(context);
    for (;;) {
        State::Pipe *pipe=nullptr;State::Device *device=nullptr,*hub=nullptr;
        State::Request *recovery=nullptr;IOUSBHostPipe *retry=nullptr;
        bool enumeration=false;
        IOUSBHostDevice *native=nullptr;XHCI::TTClearPlan plan;
        IOReturn result=kIOReturnAborted;
        state.gated([&] {
            for (auto *entry:state.devices) if (entry && entry->enumerationTT.queued()) {
                if (entry->enumerationTT.start()) { device=entry;enumeration=true;break; }
            }
            if (!device) for (auto *entry:state.devices) if (entry) {
                for (auto *candidate:entry->pipes) if (candidate && candidate->ttCleanup.queued() &&
                    (!candidate->ttRequest || !candidate->ttRequest->busy)) { pipe=candidate;break; }
                if (pipe) { device=entry;break; }
            }
            if (!device) return kIOReturnSuccess;
            if (pipe) {
                if (!pipe->ttCleanup.start()) { state.fail(kIOReturnIOError);return kIOReturnIOError; }
                recovery=pipe->ttRequest;if (recovery) recovery->busy=true;
            }
            auto *ancestor=device->configuration.ttHub<=255 ? state.slotDevices[device->configuration.ttHub] : nullptr;
            if (!state.running || state.stopping || state.fault ||
                !state.ttParentsReady(*device) || !ancestor ||
                !device->descendsFrom(*ancestor) || !ancestor->hubRegistered || ancestor->busy ||
                ancestor->configuration.speed!=USB::Speed::High || !ancestor->native ||
                !state.nub->accepts(state.owner,state.epoch)) return kIOReturnSuccess;
            result=hostResult(enumeration ? XHCI::ttClearDefaultPlan(device->configuration,ancestor->configuration.multiTT,plan) :
                XHCI::ttClearPlan(device->configuration,pipe->configuration,device->address,ancestor->configuration.multiTT,plan));
            if (result==kIOReturnSuccess) {
                hub=ancestor;native=hub->native;
                // This lease prevents parent PM and EP0 reconfiguration, but
                // admits its normal Native I/O. The child already owns the
                // ancestor's slot lifetime through the topology ledger.
                ++hub->maintenance;native->service()->retain();
            }
            return kIOReturnSuccess;
        });
        if (!device) return;
        if (native) {
            // Parent EP0 runs on requestWork. Neither the HCD nor role gate
            // may be held across this synchronous Native device request.
            for (unsigned n=0;n<plan.count;++n) {
                const auto admitted=state.gated([&] {
                    return state.running && !state.stopping && !state.fault &&
                        state.ttParentsReady(*device) &&
                        hub->hubRegistered && state.nub->accepts(state.owner,state.epoch) ?
                            kIOReturnSuccess : kIOReturnAborted;
                });
                if (admitted!=kIOReturnSuccess) { result=admitted;break; }
                UInt32 transferred=0;
                const auto request=native->internalDeviceRequest(native->service(),plan.requests[n],
                    nullptr,nullptr,&transferred,nullptr,500);
                if (result==kIOReturnSuccess && (request!=kIOReturnSuccess || transferred))
                    result=request!=kIOReturnSuccess ? request : kIOReturnIOError;
                // Control has two TT directions. Attempt both even if the
                // first failed, preserving the first error as the result.
            }
            native->service()->release();
        }
        if (recovery) {
            XHCI::CommandRecord dequeue;
            // Reset/Stop preceded TT maintenance. Only this worker continues
            // with Set Dequeue; requestWork stays available for hub EP0.
            XHCI::Command command;command.type=XHCI::SetDequeue;command.slot=device->slot;
            command.endpoint=pipe->id;
            command.parameter=pipe->backend.ring().nextAddress()|pipe->backend.ring().producerCycle();
            const auto resumed=state.execute(command,state.owner,dequeue,1000000,nullptr,true);
            state.gated([&] {
                state.completeRecovery(*recovery,recovery->ttStop,dequeue,recovery->ttPaused,resumed);
                return kIOReturnSuccess;
            });
        }
        state.gated([&] {
            const bool cleared=result==kIOReturnSuccess && state.running && !state.stopping && !state.fault &&
                state.ttParentsReady(*device) && hub &&
                hub->hubRegistered && state.nub->accepts(state.owner,state.epoch);
            if (enumeration) {
                if (!device->enumerationTT.finish(cleared,hostTimeUS())) state.fail(kIOReturnIOError);
                // Keep the failed device's domain fence and parent reference
                // across a failed wire attempt. A later timer retry uses the
                // same Native hub; no child Native object is fabricated.
                if (!cleared && state.running && !state.stopping && !state.fault && state.timer)
                    state.timer->setTimeoutMS(100);
            } else {
                if (!pipe->ttCleanup.finish(cleared,hostTimeUS())) state.fail(kIOReturnIOError);
                if (!cleared) {
                    if (state.running && !state.stopping && !state.fault && state.timer) state.timer->setTimeoutMS(100);
                }
            }
            --device->maintenance;if (hub) --hub->maintenance;
            if (recovery) {
                recovery->ttHeld=false;recovery->busy=false;pipe->ttRequest=nullptr;--state.ttRecoveryCount;
            } else if (pipe && !pipe->closing && !pipe->ttCleanup.dirty() && pipe->transfers.requiresNativeClear() && pipe->id==1 && pipe->native && !device->retiring &&
                state.running && !state.stopping && !state.fault) {
                // A Native automatic control clear may have queued this
                // retry and returned NotReady. Finish its original false
                // clearStall policy after releasing all maintenance leases.
                retry=pipe->native;retry->retain();
            }
            --state.waiters;if (pipe) state.gate->commandWakeup(pipe,false);
            state.gate->commandWakeup(&state.waiters,false);state.scheduleRequests();
            if (state.closeWork) thread_call_enter(state.closeWork);
            if ((device->retiring || (hub && hub->retiring)) && state.deviceWork) thread_call_enter(state.deviceWork);
            return kIOReturnSuccess;
        });
        if (retry) { retry->IOUSBHostPipe::clearStall(false);retry->release(); }
    }
}
void QcomUSBHost::closeWorker(void *context,void *) {
    auto &state=*static_cast<State *>(context);
    for (;;) {
        bool found=false;
        state.gated([&] {
            for (auto *device:state.devices) if (device && !device->retiring && !device->busy && !device->maintenance) {
                for (auto *pipe:device->pipes) if (pipe && pipe->closing && !pipe->closeFailed &&
                    pipe->ttCleanup.canRelease(pipe->requests,state.ttDomainAlive(*device),state.dmaStopped)) {
                    // removePipe may sleep for a real Configure Endpoint and
                    // the independent DMA worker. This lease pins the device
                    // across those sleeps and is included in stop's join.
                    found=true;device->busy=true;++state.waiters;
                    const auto id=pipe->id;
                    const auto result=state.removePipe(*device,*pipe);
                    if (result!=kIOReturnSuccess) {
                        if (device->pipes[id]==pipe) { pipe->closeFailed=true;state.owner->pin(); }
                        state.fail(result);
                    }
                    device->busy=false;--state.waiters;
                    state.gate->commandWakeup(&state.waiters,false);
                    if (device->retiring && state.deviceWork) thread_call_enter(state.deviceWork);
                    return result;
                }
            }
            return kIOReturnSuccess;
        });
        if (!found) return;
    }
}
void QcomUSBHost::deviceWorker(void *context,void *) {
    auto &state=*static_cast<State *>(context);
    for (;;) {
        State::Device *device=nullptr;State::Pipe *pipe=nullptr;bool prepare=false,aborted=false,releasePipe=false;
        state.gated([&] {
            for (auto *entry:state.devices) if (entry && entry->prepareQueued) {
                entry->prepareQueued=false;device=entry;prepare=true;aborted=state.stopping || state.fault || entry->retiring;break;
            }
            if (!device) for (auto *entry:state.devices) {
                if (!entry) continue;
                for (auto *candidate:entry->pipes) if (candidate && candidate->workQueued) {
                    pipe=candidate;pipe->workQueued=false;releasePipe=pipe->workRelease;
                    aborted=state.stopping || state.fault;break;
                }
                if (pipe) break;
            }
            if (!device && !pipe) for (auto *entry:state.devices) if (entry && entry->retiring && !entry->busy &&
                !entry->requests && !entry->maintenance && !entry->children && !entry->cleanupFailed &&
                (!state.ttBuffersDirty(*entry) || state.dmaStopped || !state.ttDomainAlive(*entry))) {
                entry->busy=true;device=entry;++state.waiters;break;
            }
            return kIOReturnSuccess;
        });
        if (pipe) {
            auto result=USB::Result::Success;
            if (releasePipe) result=pipe->memory.release();
            else if (aborted) result=USB::Result::Aborted;
            else {
                if (pipe->id!=1) result=pipe->memory.prepare(state.core.dmaBits());
                if (result==USB::Result::Success) result=XHCI::pipeConfiguration(pipe->descriptors,
                    pipe->device->configuration.speed,pipe->id==1 ? pipe->device->memory.ep0Ring().startAddress() :
                        pipe->memory.ring().startAddress(),state.core.dmaBits(),pipe->configuration);
            }
            state.gated([&] {
                pipe->workResult=result;pipe->workDone=true;state.gate->commandWakeup(pipe,false);return kIOReturnSuccess;
            });
            continue;
        }
        if (!device) return;
        if (prepare) {
            const auto result=aborted ? USB::Result::Aborted : device->memory.prepare(state.core.contextStride(),
                state.core.dmaBits(),state.core.maxSlots(),state.core.maxPorts());
            state.gated([&] {
                device->preparation=result;device->prepared=true;
                state.gate->commandWakeup(device,false);return kIOReturnSuccess;
            });
            continue;
        }
        bool clean=state.destroyDevice(*device);
        state.gated([&] {
            if (clean && device->detach()!=USB::Result::Success) clean=false;
            if (clean) {
                if (device->slot && state.slotDevices[device->slot]==device) state.slotDevices[device->slot]=nullptr;
                for (auto *&entry:state.devices) if (entry==device) { entry=nullptr;break; }
                if (device->native) {
                    state.deferRelease(reinterpret_cast<uintptr_t>(device->native->service()));device->native=nullptr;
                }
                for (auto *pipe:device->pipes) if (pipe && pipe->native) {
                    state.deferRelease(reinterpret_cast<uintptr_t>(pipe->native));pipe->native=nullptr;
                }
            } else {
                device->cleanupFailed=true;device->busy=false;state.fail(kIOReturnIOError);state.owner->pin();
            }
            --state.waiters;state.gate->commandWakeup(&state.waiters,false);
            state.gate->commandWakeup(&state.devices,false);return kIOReturnSuccess;
        });
        if (clean) { for (auto *pipe:device->pipes) delete pipe;delete device; }
    }
}
void QcomUSBHost::requestWorker(void *context,void *) {
    auto &state=*static_cast<State *>(context);
    unsigned operations=0;
    for (;;) {
        // A busy ISO pipe must not starve EP0, final Native handoffs, or other
        // kernel workers. THREAD_CALL_OPTIONS_ONCE serializes continuations.
        if (++operations>128) { state.scheduleRequests();return; }
        const bool isoProgress=state.isoWork();
        State::Request *request=nullptr;
        State::RequestPhase phase=State::RequestPhase::Pinned;
        XHCI::EndpointConfiguration configuration;
        state.gated([&] {
            for (auto *entry:state.requests) {
                if (!entry || entry->busy || entry->ttHeld || !entry->native || entry->phase==State::RequestPhase::Pinned) continue;
                auto &pipe=*entry->pipe;
                if (entry->phase==State::RequestPhase::Queued) {
                    if (pipe.head!=entry || pipe.active || pipe.closing || pipe.device->retiring || state.stopping || state.fault)
                        continue;
                    if (!state.ordinaryReady(pipe)) continue;
                    entry->phase=State::RequestPhase::Preparing;pipe.active=entry;
                }
                if (entry->phase==State::RequestPhase::Ready && !state.ordinaryReady(pipe)) continue;
                if (entry->phase==State::RequestPhase::Recovering && pipe.device->power.transitioning() &&
                    !state.stopping && !state.fault) continue;
                if (entry->phase==State::RequestPhase::Recovering && !entry->isochronous &&
                    XHCI::ttCleanupRequired(pipe.device->configuration,pipe.configuration) && state.ttRecoveryCount>=16)
                    continue;
                if (entry->phase==State::RequestPhase::Preparing || entry->phase==State::RequestPhase::Ready ||
                    entry->phase==State::RequestPhase::Recovering ||
                    entry->phase==State::RequestPhase::Completing) {
                    entry->busy=true;request=entry;phase=entry->phase;configuration=pipe.configuration;++state.waiters;break;
                }
            }
            return kIOReturnSuccess;
        });
        if (!request) { if (isoProgress) continue;return; }
        if (phase==State::RequestPhase::Preparing) {
            const auto prepared=request->memory.prepare(request->prepared,configuration,request->length,state.core.dmaBits(),
                request->pipe->id==1 ? &request->setup : nullptr);
            state.gated([&] {
                auto &pipe=*request->pipe;
                const auto now=hostTimeUS();
                if (prepared!=USB::Result::Success && !request->cancelled) request->status=hostResult(prepared);
                if (pipe.device->power.running() && request->deadline && now>=request->deadline)
                    state.cancelRequest(*request,kIOReturnTimeout);
                if (state.stopping || state.fault || pipe.closing || pipe.device->retiring)
                    state.cancelRequest(*request,kIOReturnAborted);
                request->phase=prepared==USB::Result::Success && !request->cancelled ?
                    State::RequestPhase::Ready : State::RequestPhase::Completing;
                request->busy=false;--state.waiters;state.gate->commandWakeup(&state.waiters,false);
                return kIOReturnSuccess;
            });
            continue;
        }
        if (phase==State::RequestPhase::Ready) {
            state.gated([&] {
                auto &pipe=*request->pipe;const auto now=hostTimeUS();
                if (state.stopping || state.fault || pipe.closing || pipe.device->retiring)
                    state.cancelRequest(*request,kIOReturnAborted);
                if (pipe.device->power.running() && request->deadline && now>=request->deadline)
                    state.cancelRequest(*request,kIOReturnTimeout);
                if (request->cancelled) request->phase=State::RequestPhase::Completing;
                else if (state.ordinaryReady(pipe)) {
                    const auto submitted=state.hardware([&] {
                        return hostResult(request->memory.submit(pipe.transfers,pipe.backend.ring(),
                            reinterpret_cast<uintptr_t>(request->native),state.epoch,now,
                            request->deadline ? request->deadline-now : 0));
                    });
                    if (request->memory.ticket()) {
                        request->phase=State::RequestPhase::Submitted;
                        if (submitted!=kIOReturnSuccess) state.cancelRequest(*request,submitted);
                    } else {
                        request->status=submitted;request->phase=State::RequestPhase::Completing;
                    }
                }
                request->busy=false;--state.waiters;state.gate->commandWakeup(&state.waiters,false);
                return kIOReturnSuccess;
            });
            continue;
        }
        if (phase==State::RequestPhase::Recovering) {
            state.recoverRequest(*request);
            state.gated([&] {
                request->busy=false;--state.waiters;state.gate->commandWakeup(&state.waiters,false);
                if (request->pipe->ttCleanup.queued() && state.ttWork) thread_call_enter(state.ttWork);
                return kIOReturnSuccess;
            });
            continue;
        }
        const auto clean=request->memory.finish(request->status==kIOReturnSuccess);
        bool detached=false;
        state.gated([&] {
            if (request->memory.currentState()!=USB::DMAState::Empty) {
                request->phase=State::RequestPhase::Pinned;request->busy=false;
                request->cleanupRecovery.failed(request->memory.cleanupRetryable());
                state.fail(hostResult(clean));state.owner->pin();
                --state.waiters;state.gate->commandWakeup(&state.waiters,false);
                state.gate->commandWakeup(request->pipe,false);return kIOReturnIOError;
            }
            if (clean!=USB::Result::Success) request->status=hostResult(clean);
            state.detachRequest(*request);detached=true;return kIOReturnSuccess;
        });
        if (!detached) continue;
        auto *native=request->handoff;
        const auto bytes=request->isochronous ? UInt32(request->iso.transferred()>UINT32_MAX ?
            UINT32_MAX : request->iso.transferred()) : request->status==kIOReturnSuccess && request->memory.validData() ?
            request->memory.transferred() : 0;
        // Native complete() owns the stage1 packet-filter notification.
        const auto enqueued=XHCI::handoffRequest(*native,*request->completer,request->status,bytes,
            request->stamp ? request->stamp : mach_absolute_time());
        if (enqueued!=kIOReturnSuccess) {
            // Do not repeat an uncertain enqueue or reinsert a potentially
            // reused Native request pointer into the active lookup table.
            state.gated([&] {
                request->phase=State::RequestPhase::Pinned;request->invariantPinned=true;request->busy=false;state.owner->pin();
                --state.waiters;state.gate->commandWakeup(&state.waiters,false);return kIOReturnIOError;
            });
            continue;
        }
        if (request->queueRetain) native->release();
        native->release();request->completer->release();
        state.gated([&] {
            for (auto *&entry:state.requests) if (entry==request) { entry=nullptr;break; }
            --state.requestCount;--state.waiters;state.gate->commandWakeup(&state.waiters,false);return kIOReturnSuccess;
        });
        delete request;
    }
}
IOUSBHostDevice *QcomUSBHost::createDevice(UInt32 speed,UInt32 location,UInt32) {
    return fState ? fState->createDevice(speed,location) : nullptr;
}
void QcomUSBHost::nativeSlot193(IOUSBHostDevice *device) {
    if (fState) fState->retireDevice(device);
}
IOReturn QcomUSBHost::registerHubGated(IOUSBHostDevice *device,const StandardUSB::HubDescriptor *descriptor,tRegisterHubFlags flags) {
    return fState ? fState->registerHubDescriptor(device,descriptor,flags) : kIOReturnNotReady;
}
IOReturn QcomUSBHost::registerSuperSpeedHubGated(IOUSBHostDevice *device,const StandardUSB::SuperSpeedHubDescriptor *descriptor) {
    return fState ? fState->registerHubDescriptor(device,descriptor) : kIOReturnNotReady;
}
IOReturn QcomUSBHost::unregisterHubGated(IOUSBHostDevice *device) {
    return fState ? fState->unregisterHub(device) : kIOReturnNotReady;
}
IOReturn QcomUSBHost::createPipeGated(IOUSBHostPipe::StandardUSBDescriptors &descriptors,IOUSBHostDevice *device,
    IOUSBHostInterface *interface,IOUSBHostPipe *&pipe) {
    pipe=nullptr;return fState ? fState->createPipe(descriptors,device,interface,pipe) : kIOReturnNotReady;
}
IOReturn QcomUSBHost::adjustPipeGated(IOUSBHostPipe *pipe,IOUSBHostPipe::StandardUSBDescriptors &descriptors) {
    return fState ? fState->adjustPipe(pipe,descriptors) : kIOReturnNotReady;
}
IOReturn QcomUSBHost::closePipeGated(IOUSBHostPipe *pipe) {
    return fState ? fState->closePipe(pipe) : kIOReturnNotReady;
}
IOReturn QcomUSBHost::ioGated(AppleUSBHostRequest *request) {
    const auto result=fState ? fState->submitRequest(request) : kIOReturnNotReady;
    // Like the Native HCD, report a rejected request's stage0 status too.
    // Accepted requests already received this notification at admission.
    if (request && result!=kIOReturnSuccess) AppleUSBHostController::nativeSlot216(request,result,0);
    return result;
}
IOReturn QcomUSBHost::abortPipeGated(IOUSBHostPipe *pipe,IOReturn reason,IOService *client) {
    return fState ? fState->abortRequests(pipe,reason,client) : kIOReturnNotReady;
}
IOReturn QcomUSBHost::clearPipeStallGated(IOUSBHostPipe *pipe) {
    return fState ? fState->clearStall(pipe) : kIOReturnNotReady;
}
IOReturn QcomUSBHost::suspendPipesGated(IOUSBHostDevice *device) {
    return fState ? fState->setDeviceSuspended(device,true) : kIOReturnNotReady;
}
IOReturn QcomUSBHost::resumePipesGated(IOUSBHostDevice *device) {
    return fState ? fState->setDeviceSuspended(device,false) : kIOReturnNotReady;
}
IOReturn QcomUSBHost::enableStreamsGated(IOUSBHostPipe *) { return kIOReturnUnsupported; }
IOReturn QcomUSBHost::disableStreamsGated(IOUSBHostPipe *) { return kIOReturnUnsupported; }
IOReturn QcomUSBHost::createStreamGated(IOUSBHostDevice *,IOUSBHostPipe *,UInt32,IOUSBHostStream *&stream) {
    stream=nullptr;return kIOReturnUnsupported;
}
IOReturn QcomUSBHost::abortStreamGated(IOUSBHostStream *,IOReturn) { return kIOReturnUnsupported; }
IOReturn QcomUSBHost::closeStreamGated(IOUSBHostStream *) { return kIOReturnUnsupported; }
IOReturn QcomUSBHost::portOperation(UInt32 port,PortOperation operation) {
    return fState ? fState->operatePort(port,operation) : kIOReturnNotReady;
}
IOReturn QcomUSBHost::portStatus(UInt32 port,UInt32 &status) {
    return fState ? fState->portStatus(port,status) : kIOReturnNotReady;
}
void QcomUSBHost::signalPort(unsigned number) {
    // Copy a Native port retain under the controller gate, then drop that
    // gate before calling the Native port. Never reverse port->controller.
    QcomUSBHostPort *port=nullptr;
    if (!fState) return;
    fState->gated([&] {
        if (!fState->stopping && fPorts && number && number<=fPortCount) {
            port=OSDynamicCast(QcomUSBHostPort,fPorts[number-1]);if (port) port->retain();
        }
        return kIOReturnSuccess;
    });
    if (port) { port->signalHardwareChange();port->release(); }
}
IOReturn QcomUSBHost::createPorts() {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
    static_assert(__builtin_offsetof(QcomUSBHost,fPorts)==1216,"native port ownership table offset");
    static_assert(__builtin_offsetof(QcomUSBHost,fPortCount)==1224,"native port count offset");
#pragma clang diagnostic pop
    if (!fState || !fState->gate) return kIOReturnNotReady;
    return fState->gated([&] {
        if (fState->stopping || fState->fault || !fState->running || fPorts) return kIOReturnNotReady;
        auto failed=[&](IOReturn error) {
            // Native marks port construction as one-shot before allocating
            // the table. A partial failure cannot be retried as an empty
            // controller; close hardware admission and retain its table for
            // Native teardown instead of leaving a published partial HCD.
            fState->fail(error);return error;
        };
        UInt32 count=fState->core.maxPorts();
        if (!setProperty("port-count",&count,sizeof(count))) return failed(kIOReturnNoMemory);
        auto result=AppleUSBHostController::createPorts();
        if (result!=kIOReturnSuccess) return failed(result);
        if (!fPorts || fPortCount!=count) return failed(kIOReturnBadArgument);
        for (UInt32 n=0;n<count;++n) fPorts[n]=nullptr;
        // Native createPorts is a one-time operation. A partial start remains
        // in its Native ownership table and is drained by Native teardown.
        for (UInt32 n=1;n<=count;++n) {
            const auto *protocol=fState->protocols.get(n);
            if (!protocol) return failed(kIOReturnBadArgument);
            // Native reset/stop loops accept null table entries. Preserve
            // hardware numbering, but give an unrouted logical USB3 port no
            // Native service, power operation or enumeration-current reserve.
            if (!fState->connectorPorts.routed(n)) continue;
            const bool super=protocol->major==3;
            auto *port=QcomUSBHostPort::withController(this,n,super);
            if (!port) return failed(kIOReturnNoMemory);
            if (!fState->current.port(super,[&](const char *key,UInt32 value) {
                return port->setProperty(key,value,32);
            })) { port->release();return failed(kIOReturnNoMemory); }
            if (!port->attach(this)) { port->release();return failed(kIOReturnNotAttached); }
            if (!port->start(this)) { port->detach(this);port->release();return failed(kIOReturnNotReady); }
            fPorts[n-1]=port;
            // Signal outside this gate via the ordinary worker. Native port
            // registration is handled by the Family's publication lifecycle.
            fState->changedPorts[n]=true;
        }
        // controllerRegisteredThreadCall resets the driver vote to0; its
        // post-createPorts action can also lower the separate device vote.
        // At this callback PM is initialized, so keep this cold-PM controller
        // active until the platform synchronously drains and terminates it.
        result=changePowerStateTo(3);
        if (result!=kIOReturnSuccess) return failed(result);
        fState->portsComplete=true;
        thread_call_enter(fState->releaseWork);
        return kIOReturnSuccess;
    });
}
AppleUSBHostPort *QcomUSBHost::getCompanionPort(AppleUSBHostPort *source,UInt32 speed) {
    AppleUSBHostPort *result=nullptr;
    if (!fState || !source) return nullptr;
    fState->gated([&] {
        if (fState->stopping || fState->fault || !fState->running || !fState->portsComplete || !fPorts)
            return kIOReturnNotReady;
        for (UInt32 n=1;n<=fPortCount;++n) if (fPorts[n-1]==source) {
            const auto peer=fState->connectorPorts.companion(n,speed);
            if (peer && peer<=fPortCount) result=fPorts[peer-1];
            break;
        }
        return kIOReturnSuccess;
    });
    // Borrowed Native port; the caller's Native getter owns the cache retain.
    return result;
}
IOReturn QcomUSBHost::resolveCompanion(QcomUSBHostPort *source,UInt32 speed,AppleUSBHostPort *&result) {
    result=nullptr;
    if (!fState || !source) return kIOReturnNotReady;
    return fState->gated([&] {
        if (fState->stopping || fState->fault || !fState->running || !fState->portsComplete || !fPorts)
            return kIOReturnNotReady;
        bool owned=false;
        for (UInt32 n=0;n<fPortCount;++n) if (fPorts[n]==source) { owned=true;break; }
        if (!owned) return kIOReturnBadArgument;
        // Lock order is Native port -> Native controller, with no role/MMIO
        // action. The inherited body checks inactivity and retains its peer
        // before this gate opens. Native handleStopPhase2 clears all6 caches.
        return source->AppleUSBHostPort::getCompanionPortGated(speed,result);
    });
}
IOReturn QcomUSBHost::executeCommand(const XHCI::Command &command,OSObject *payload,
    XHCI::CommandRecord &record,UInt32 timeoutUS) {
    auto *loop=getWorkLoop();
    if (!loop || !loop->inGate() || !fState) return kIOReturnNotPermitted;
    return fState->execute(command,payload,record,timeoutUS);
}
