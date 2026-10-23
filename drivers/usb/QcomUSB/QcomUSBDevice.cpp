// SPDX-License-Identifier: BSD-2-Clause
#include "QcomUSBDevice.h"
#include "QcomUSB.h"
#include "USBIODMA.h"
#include "DWC3Control.h"
#include "DWC3DeviceReset.h"
#include "USBDeviceRequests.h"
#include "USBDeviceIsochronous.h"
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/usb/IOUSBDeviceInterface.h>
#include <libkern/c++/OSData.h>
#include <kern/clock.h>

OSDefineMetaClassAndStructors(QcomUSBDevice, IOUSBDeviceController);

namespace {
IOReturn ioResult(USB::Result r) {
    switch (r) {
    case USB::Result::Success: case USB::Result::Pending: return kIOReturnSuccess;
    case USB::Result::Invalid: return kIOReturnBadArgument;
    case USB::Result::Unsupported: return kIOReturnUnsupported;
    case USB::Result::Busy: return kIOReturnBusy;
    case USB::Result::Timeout: case USB::Result::BusExpired: return kIOReturnTimeout;
    case USB::Result::MissedService: return kIOReturnUnderrun;
    case USB::Result::Aborted: case USB::Result::SetupPending: return kIOReturnAborted;
    case USB::Result::NotReady: return kIOReturnNotReady;
    case USB::Result::Overflow: return kIOReturnOverrun;
    default: return kIOReturnIOError;
    }
}
unsigned physical(int address) {
    return (unsigned(address) & ~0x8fu) ? 32 : (unsigned(address) & 15) * 2 + ((address >> 7) & 1);
}
UInt64 nowUS() { UInt64 ns = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000; }
}

struct QcomUSBDevice::State {
    static constexpr unsigned TRBs = 64, TRBBytes = TRBs * sizeof(DWC3::TRB);
    static constexpr unsigned EventsOffset = 32 * TRBBytes, EventsBytes = 4096;
    static constexpr unsigned SetupOffset = EventsOffset + EventsBytes, DescriptorBytes = SetupOffset + 64;
    QcomUSBDevice *owner;
    QcomUSBController *nub;
    USB::MMIO &io;
    DWC3::Core<USB::MMIO> &core;
    IOMapper *mapper;
    IOWorkLoop *loop;
    IOCommandGate *gate = nullptr;
    IOInterruptEventSource *irq = nullptr;
    IOTimerEventSource *timer = nullptr;
    thread_call_t work = nullptr, dmaWork = nullptr;
    USB::PlatformContract resources;
    USB::IOMemoryAPI memory;
    USB::DMARegion<USB::IOMemoryAPI> descriptors, receive;
    using Requests = USB::DeviceRequests<USB::IOMemoryAPI>;
    Requests requests;
    USB::DeviceBounce<USB::IOMemoryAPI> *requestDMA[64]{};
    Requests isoDMA;
    using Iso = USB::DeviceIsochronous<USB::IOMemoryAPI>;
    using IsoRing = DWC3::IsochronousRing<DWC3::Core<USB::MMIO>,USB::MMIO>;
    Iso isoRequests;
    struct IsoRequest {
        Iso::Request request;
        _IOUSBDeviceIORequestTag *tag=nullptr;
        Iso::Frame frames[Iso::MaximumFrames];
    };
    USB::DeviceBounce<USB::IOMemoryAPI> *isoFrameDMA[64]{};
    const unsigned endpointCount;
    USB::DeviceSession session;
    DWC3::Control<DWC3::Core<USB::MMIO>, USB::MMIO> control;
    struct Endpoint {
        DWC3::Transfer<DWC3::Core<USB::MMIO>, USB::MMIO> *td = nullptr;
        IsoRing *iso=nullptr;
        DWC3::EndpointConfiguration config{};
        UInt64 stopDeadline = 0;
        bool enabled = false, stalled = false, aborting = false;
    } endpoints[32];
    struct ResetEndpoint {
        State *state=nullptr;unsigned endpoint=0;
        unsigned physicalEndpoint() const {return endpoint;}
        bool ownsBuffers() const {
            const auto &e=state->endpoints[endpoint];
            return (e.td && e.td->ownsBuffers()) || (e.iso && e.iso->ownsHardware());
        }
        DWC3::TransferState currentState() const {
            const auto &e=state->endpoints[endpoint];
            return (e.iso && e.iso->stopping()) || (e.td && e.td->currentState()==DWC3::TransferState::Stopping) ?
                DWC3::TransferState::Stopping:DWC3::TransferState::Idle;
        }
        USB::Result cancel() {
            auto &e=state->endpoints[endpoint];USB::Result result=USB::Result::Success;
            if(e.iso && e.iso->ownsHardware())result=e.iso->cancel(USB::Result::Aborted);
            if(result!=USB::Result::Success && result!=USB::Result::Pending)return result;
            if(e.td && e.td->ownsBuffers())return e.td->cancel();
            return result;
        }
    } resetEndpoints[30];
    DWC3::DeviceReset<decltype(core),decltype(control),ResetEndpoint> busReset;
    enum class Kind { Control, Transfer, Isochronous, Reset, Connect, Suspend, Resume, Disconnect };
    static constexpr unsigned JobCount=256;
    struct Job { Kind kind; DWC3::ControlNotification control{}; Completion completion{}; UInt64 generation=0; } jobs[JobCount];
    unsigned head = 0, tail = 0, count = 0, abortDepth = 0;
    UInt64 epoch, nativeToken = 0, inCookie = 0, deadline = 0;
    UInt32 trackerState = 1;
    bool inStatus = false;
    USB::SetupPacket setup{};
    USB::Speed speed = USB::Speed::Unknown;
    IOUSBDeviceControlRequest *tracker = nullptr;
    bool opened = false, gateAdded = false, irqAdded = false, timerAdded = false;
    IOSimpleLock *pinLock=IOSimpleLockAlloc();
    bool ready = false, running = false, stopping = false, fault = false, disconnect = false, released = false;
    UInt64 busResetCount=0;
    UInt32 controlEvents[12]{};
    bool superSpeedCapability=false;
    bool wanted = false, dispatchingSetup = false, draining = false, quiescing = false, dmaPeriodicTurn=false;
    State(QcomUSBDevice *device, QcomUSBController *provider, USB::MMIO &registers,
          DWC3::Core<USB::MMIO> &controller, IOMapper *deviceMapper) :
        owner(device), nub(provider), io(registers), core(controller), mapper(deviceMapper),
        loop(provider->getWorkLoop()), resources(provider->resourceContract()), memory(mapper, resources),
        descriptors(memory), receive(memory), requests(memory), isoDMA(memory), isoRequests(memory,isoDMA),
        endpointCount(controller.endpointCount()),
        control(core, io), busReset(core,control),epoch(provider->epoch()) {
        nub->retain(); loop->retain();
        for(unsigned ep=2;ep<endpointCount;++ep) {
            resetEndpoints[ep-2]={this,ep};
            if(!busReset.add(resetEndpoints[ep-2]))fault=true;
        }
    }
    template<class F> IOReturn gated(F function) {
        return gate->runAction([](OSObject *, void *arg, void *, void *, void *) -> IOReturn {
            return (*static_cast<F *>(arg))();
        }, &function);
    }
    bool accepts() const { return ready && !stopping && !fault && !owner->pinned() && nub->accepts(owner, epoch); }
    volatile uint32_t *events() const {
        return reinterpret_cast<volatile uint32_t *>(static_cast<UInt8 *>(descriptors.bytes()) + EventsOffset);
    }
    bool postJob(Job job) {
        if (count == JobCount) { fault = true; return false; }
        job.generation=session.generation();
        jobs[tail] = job; tail = (tail + 1) % JobCount; ++count;
        if (work) thread_call_enter(work);
        return true;
    }
    void completed(unsigned ep, const DWC3::TransferCompletion &c) {
        if (requests.completed(c.cookie, epoch, c.result, c.bytes) != USB::Result::Success) fault = true;
        endpoints[ep].stopDeadline = 0;
        notifyRequests();
    }
    void notifyRequests() {
        while (auto cookie = requests.notice()) {
            if (!postJob({Kind::Transfer, {}, {0, cookie, USB::Result::Pending, 0}})) break;
            requests.announce(cookie);
        }
        if (dmaWork && requests.needsWork()) thread_call_enter(dmaWork);
        notifyIsochronous();
        gate->commandWakeup(&requests);
    }
    void notifyIsochronous() {
        if (isoRequests.reap()!=USB::Result::Success) { fault=true;owner->pin(); }
        if (isoDMA.pinned()) { fault=true;owner->pin(true); }
        while (auto serial=isoRequests.notice()) {
            if (!postJob({Kind::Isochronous,{}, {0,serial,USB::Result::Pending,0}})) break;
            isoRequests.announce(serial);
        }
        if (dmaWork && isoDMA.needsWork()) thread_call_enter(dmaWork);
        gate->commandWakeup(&requests);
    }
    void collectIsochronous(unsigned ep,bool isolating=false) {
        auto &endpoint=endpoints[ep];
        if (!endpoint.iso) return;
        UInt32 raw=0;UInt64 stamp=0;
        const bool valid=!isolating && core.currentFrame(raw)==USB::Result::Success;
        absolutetime_to_nanoseconds(mach_absolute_time(),&stamp);
        if (isoRequests.collect(ep,*endpoint.iso,epoch,stamp,raw,
            endpoint.config.speed==USB::Speed::Full ? 1000000:125000,valid,isolating)!=USB::Result::Success)
            fault=true;
        notifyIsochronous();
    }
    void controlEvent(const DWC3::ControlNotification &message) {
        if (message.kind == DWC3::ControlNotificationKind::None) return;
        if(message.kind==DWC3::ControlNotificationKind::Aborted) {
            UInt64 reason[]={message.transaction,UInt64(message.result),message.bytes,UInt64(control.currentState())};
            owner->setProperty("USBDeviceControlAbort",reason,sizeof(reason));
            owner->setProperty("USBDeviceControlEvents",controlEvents,sizeof(controlEvents));
        }
        if (message.kind == DWC3::ControlNotificationKind::Setup) {
            // Cancellation notices precede this SETUP in the policy FIFO,
            // including a previous response still being mapped by dmaWorker.
            requests.cancelEndpoint(1); notifyRequests();
        }
        if (message.kind == DWC3::ControlNotificationKind::Data ||
            message.kind == DWC3::ControlNotificationKind::Aborted ||
            message.kind == DWC3::ControlNotificationKind::Status) {
            if (receive.currentState() == USB::DMAState::Owned) receive.retire(epoch);
            if (inCookie && (message.kind == DWC3::ControlNotificationKind::Aborted ||
                (message.kind == DWC3::ControlNotificationKind::Data && !inStatus) ||
                (message.kind == DWC3::ControlNotificationKind::Status && inStatus))) {
                DWC3::TransferCompletion done{message.result, inCookie, epoch, message.bytes};
                completed(1, done);
                inCookie = 0;
            }
        }
        postJob({Kind::Control, message});
    }
    // Under the shared work-loop gate. Reclamation follows a whole-core halt;
    // no pending native callback is executed inside event-ring consumption.
    IOReturn offline() {
        if (!opened) return kIOReturnNotReady;
        const bool wasRunning=running;
        running = false;
        if (wasRunning && !session.advance()) { fault=true;owner->pin(); }
        if (irq) irq->disable();
        if (timer) timer->cancelTimeout();
        for (unsigned ep = 1; ep < endpointCount; ++ep) {
            requests.cancelEndpoint(ep);isoRequests.cancel(ep);
        }
        // Parent isolation can already have asserted BCR and detached the
        // mapper. Its shared Core view recorded the successful halt first;
        // do not access controller MMIO while that reset is held.
        if (!nub->dmaIsolated() && core.halt() != USB::Result::Success) {
            for (unsigned ep = 1; ep < endpointCount; ++ep) {
                requests.quarantine(ep);isoDMA.quarantine(ep);
            }
            notifyRequests(); fault = true; owner->pin(true); return kIOReturnTimeout;
        }
        IOReturn result = kIOReturnSuccess;
        DWC3::ControlNotification note;
        if (control.currentState() != DWC3::ControlState::Uninitialized) {
            auto r = control.resetAfterHalt(note);
            if (r != USB::Result::Success) { fault = true; owner->pin(); result = ioResult(r); }
            else controlEvent(note);
        }
        for (unsigned ep = 2; ep < endpointCount; ++ep) {
            if (auto *ring=endpoints[ep].iso) {
                if (ring->reclaimAfterHalt(USB::Result::Aborted)!=USB::Result::Success) {
                    fault=true;owner->pin();result=kIOReturnIOError;
                } else collectIsochronous(ep,true);
            }
            auto *td = endpoints[ep].td;
            if (td && td->ownsBuffers()) {
                DWC3::TransferCompletion c{};
                if (td->reclaimAfterHalt(c) != USB::Result::Success) {
                    fault = true; owner->pin(); result = kIOReturnIOError;
                } else completed(ep, c);
            }
            endpoints[ep].enabled = false;
            endpoints[ep].stopDeadline = 0;
        }
        endpoints[1].stopDeadline = 0;
        requests.isolated();isoDMA.isolated();notifyRequests();
        descriptors.retireAfterIsolation(); receive.retireAfterIsolation();
        busReset.isolated();gate->commandWakeup(&requests);
        inCookie = 0; deadline = 0;
        return result;
    }
    IOReturn online() {
        owner->setProperty("USBDeviceStartStage","Admission");
        wanted = true;
        if (!accepts()) return kIOReturnNotReady;
        if (quiescing) return kIOReturnBusy;
        if (running) return kIOReturnSuccess;
        if (count || !requests.empty() || !isoRequests.empty()) return kIOReturnBusy;
        if (!session.advance()) { fault=true;owner->pin();return kIOReturnNoResources; }
        auto rollback=[&](IOReturn result) { wanted=false;offline();return result; };
        owner->setProperty("USBDeviceStartStage","SelectDevice");
        if (!core.stopped() || core.select(USB::Role::Device) != USB::Result::Success) return rollback(kIOReturnNotReady);
        owner->setProperty("USBDeviceStartStage","StartConfiguration");
        auto status=core.beginEndpointConfiguration();
        if (status!=USB::Result::Success) return rollback(ioResult(status));
        speed = USB::Speed::Unknown;
        const auto maximum=resources.maximumSpeed==4 ? USB::Speed::Super:USB::Speed::High;
        if (maximum==USB::Speed::Super && !superSpeedCapability) return rollback(kIOReturnNotReady);
        for (unsigned ep = 0; ep < 2; ++ep) {
            owner->setProperty("USBDeviceStartStage",ep ? "ConfigureEP0In":"ConfigureEP0Out");
            endpoints[ep].config = {ep, DWC3::EndpointType::Control, maximum,
                maximum == USB::Speed::Super ? 512u : 64u, 0, 0};
            status=core.configureEndpoint(endpoints[ep].config);
            if (status!=USB::Result::Success) return rollback(ioResult(status));
            endpoints[ep].enabled = true; endpoints[ep].stalled = false;
        }
        owner->setProperty("USBDeviceStartStage","ControlNegotiate");
        status=control.negotiate(maximum);if(status!=USB::Result::Success)return rollback(ioResult(status));
        owner->setProperty("USBDeviceStartStage","InstallEvents");
        status=core.installEvents(descriptors.busAddress() + EventsOffset, EventsBytes);
        if(status!=USB::Result::Success)return rollback(ioResult(status));
        status=descriptors.publish(epoch);if(status!=USB::Result::Success)return rollback(ioResult(status));
        owner->setProperty("USBDeviceStartStage","ControlStart");
        status=control.start();if(status!=USB::Result::Success)return rollback(ioResult(status));
        owner->setProperty("USBDeviceStartStage","RunDevice");
        status=core.runDevice(maximum);if(status!=USB::Result::Success)return rollback(ioResult(status));
        running = true; irq->enable(); timer->setTimeoutMS(5);
        owner->setProperty("USBDeviceStartStage","Running");
        return kIOReturnSuccess;
    }
    void beginBusReset() {
        if(!session.advance()) {fault=true;return;}
        UInt32 stalled=0;
        for(unsigned ep=1;ep<endpointCount;++ep) {
            if(endpoints[ep].stalled)stalled|=1u<<ep;
            requests.cancelEndpoint(ep);isoRequests.cancel(ep);
        }
        deadline=0;
        const auto result=busReset.begin(nowUS(),stalled);
        if(result!=USB::Result::Success && result!=USB::Result::Pending) {fault=true;return;}
        owner->setProperty("USBDeviceBusResetCount",++busResetCount,64);
        postJob({Kind::Reset});
    }
    void event(DWC3::Event event) {
        if (fault || disconnect) return;
        if (event.device()) {
            if (!event.deviceSpecific()) { fault = true; return; }
            switch (event.type()) {
            case 0: disconnect = true; break;
            case 1: beginBusReset(); break;
            case 2: {
                const auto status = io.read(DWC3::DSTS);
                const auto maximum=superSpeedCapability && resources.maximumSpeed==4 ? USB::Speed::Super:USB::Speed::High;
                if (USB::deviceConnectionSpeed(maximum,status,speed)!=USB::Result::Success ||
                    (!busReset.active() && control.negotiate(speed)!=USB::Result::Success)) { fault = true; return; }
                for (unsigned ep = 0; ep < 2; ++ep) endpoints[ep].config = {ep, DWC3::EndpointType::Control,
                    speed, speed == USB::Speed::Super ? 512u : 64u, 0, 0};
                postJob({Kind::Connect}); break;
            }
            case 3: {
                const unsigned link = event.parameters() & 15;
                if (link == 3 && session.suspend(true)) suspend();
                if (link == 0 && session.suspend(false)) postJob({Kind::Resume});
                break;
            }
            case 4: if (session.suspend(false)) postJob({Kind::Resume}); break;
            case 6: if (session.suspend(true)) suspend(); break;
            case 9: case 11: fault = true; break;
            default: break;
            }
            return;
        }
        const unsigned ep = event.endpointNumber();
        if (ep < 2) {
            for(unsigned n=11;n>1;--n)controlEvents[n]=controlEvents[n-2];
            controlEvents[0]=event.word;controlEvents[1]=UInt32(control.currentState());
            if(busReset.stoppingControl() && control.currentState()==DWC3::ControlState::Stopped)return;
            DWC3::ControlNotification note;
            auto r = control.handle(event, epoch, note);
            if (r != USB::Result::Success && r != USB::Result::Pending) { fault = true; return; }
            controlEvent(note);
        } else if (endpoints[ep].config.type==DWC3::EndpointType::Isochronous && endpoints[ep].iso) {
            auto *ring=endpoints[ep].iso;
            const auto result=ring->handle(event,epoch,nowUS());
            if (result!=USB::Result::Success && result!=USB::Result::Pending && result!=USB::Result::NotReady) fault=true;
            if (!ring->stopping()) endpoints[ep].stopDeadline=0;
            collectIsochronous(ep);
        } else if (endpoints[ep].td) {
            DWC3::TransferCompletion c{};
            auto r = endpoints[ep].td->handle(event, epoch, c);
            if (r == USB::Result::Success) completed(ep, c);
            else if (r != USB::Result::Pending && r != USB::Result::NotReady) fault = true;
        }
    }
    void suspend() {
        // Old periodic frame numbers cannot cross a suspended bus. Cancel
        // data now; END commands run after the event snapshot is acknowledged.
        for (unsigned ep=2;ep<endpointCount;++ep) isoRequests.cancel(ep);
        postJob({Kind::Suspend});
    }
    void poll() {
        if (!running) return;
        if (!nub->accepts(owner, epoch)) { wanted = false; offline(); return; }
        draining = true;
        auto handler = [this](DWC3::Event e) { event(e); };
        if (core.drainEvents(events(), handler) != USB::Result::Success) fault = true;
        draining = false;
        if(!fault && busReset.active()) {
            const auto result=busReset.finish(nowUS(),speed==USB::Speed::Unknown ? USB::Speed::High:speed);
            if(result!=USB::Result::Success && result!=USB::Result::Pending)fault=true;
            if(result==USB::Result::Success) {
                for(unsigned ep=0;ep<endpointCount;++ep)endpoints[ep].stalled=false;
                gate->commandWakeup(&requests);
            }
        }
        // The old ring snapshot has now been acknowledged; reset/stop can
        // safely discard the incarnation without violating drainEvents.
        if (fault || disconnect) {
            const bool removed = disconnect;
            disconnect = false;
            if (fault) wanted = false;
            if (removed) wanted = false;
            if (offline() != kIOReturnSuccess) return;
            if (removed || fault) postJob({Kind::Disconnect});
        }
        serviceRequests();
    }
    // DMA work never executes policy/callbacks. A policy callback may cancel
    // another endpoint and sleep with the shared recursive gate released.
    // The independent DMA worker can then finish that endpoint's unmap.
    void sleepRequests() {
        AbsoluteTime until;
        clock_interval_to_deadline(1, kMillisecondScale, &until);
        gate->commandSleep(&requests, until, THREAD_UNINT);
    }
    void deliver(UInt64 cookie) {
        Requests::Completion completion;
        for (;;) {
            auto result = requests.takeCompletion(cookie, completion);
            if (result == USB::Result::Invalid) return; // already drained by abort
            if (result == USB::Result::Success) break;
            serviceRequests();
            if (running && !draining) poll();
            sleepRequests();
        }
        if (requests.pinned()) owner->pin(true);
        auto *tag = static_cast<_IOUSBDeviceIORequestTag *>(completion.context);
        if (physical(tag->endpoint)==1 || completion.result!=USB::Result::Success) {
            UInt64 diagnostic[]={UInt64(tag->endpoint),cookie,UInt64(completion.result),completion.bytes,
                tag->lengthOrFrameCount};
            owner->setProperty(physical(tag->endpoint)==1 ? "USBDeviceEP0Completion":"USBDeviceLastIOError",
                diagnostic,sizeof(diagnostic));
        }
        tag->dma = nullptr; tag->next = nullptr;
        tag->status = ioResult(completion.result); tag->result.bytesTransferred = completion.bytes;
        // takeCompletion detached the tag before reentrant native callbacks.
        owner->completeEndpointIO(tag);
        notifyRequests();
    }
    void deliverIsochronous(UInt64 serial) {
        USB::Result result=USB::Result::NotReady;Iso::Request *request=nullptr;
        while (isoRequests.contains(serial)) {
            request=isoRequests.take(serial,result);
            if (request) break;
            serviceRequests();
            if (running && !draining) poll();
            sleepRequests();
        }
        if (!request) return; // a reentrant abort already detached it
        auto *job=static_cast<IsoRequest *>(request->context);
        auto *tag=job->tag;
        for (unsigned n=0;n<request->count;++n) {
            auto &frame=tag->result.frames[n];const auto &done=job->frames[n];
            frame.status=ioResult(done.result);frame.actual=UInt16(done.actual);
            nanoseconds_to_absolutetime(done.timestampNS,&frame.timestamp);
        }
        tag->dma=nullptr;tag->next=nullptr;tag->status=ioResult(result);
        // The union at72 must continue to contain the original frame list.
        // Detach and free private metadata before any reentrant Family call.
        delete job;owner->completeEndpointIO(tag);notifyRequests();
    }
    void abortEndpoint(unsigned ep) {
        const bool wasAborting = endpoints[ep].aborting;
        endpoints[ep].aborting = true; ++abortDepth;
        requests.cancelEndpoint(ep);
        isoRequests.cancel(ep);
        notifyRequests();
        while (auto cookie = requests.pending(ep)) deliver(cookie);
        while (auto serial=isoRequests.pending(ep)) deliverIsochronous(serial);
        while (running && endpoints[ep].iso && endpoints[ep].iso->ownsHardware()) {
            serviceRequests();if (running && !draining) poll();sleepRequests();
        }
        endpoints[ep].aborting = wasAborting; --abortDepth;
        gate->commandWakeup(&requests);
    }
    void resetTracker() {
        if (!tracker) return;
        const bool dispatching = dispatchingSetup;
        dispatchingSetup = true; nativeToken = 0; deadline = 0;
        tracker->reset();
        dispatchingSetup = dispatching;
    }
    void serviceRequests() {
        if (!running || draining) return;
        requests.expire(nowUS());
        notifyRequests();
        serviceIsochronous();
        for (unsigned ep = 1; ep < endpointCount; ++ep) {
            USB::Result reason;
            if (requests.stopping(ep, reason)) {
                auto &endpoint = endpoints[ep];
                if (!endpoint.stopDeadline) {
                    endpoint.stopDeadline = nowUS() + 10000;
                    USB::Result r;
                    if (ep == 1) {
                        if(busReset.active())continue; // the reset helper already issued END
                        DWC3::ControlNotification note;
                        r = control.reject(nativeToken, note); controlEvent(note);
                    } else r = endpoint.td->cancel(reason == USB::Result::Timeout ? reason : USB::Result::Aborted);
                    if (r != USB::Result::Success && r != USB::Result::Pending) fault = true;
                } else if (nowUS() >= endpoint.stopDeadline) fault = true;
            }
        }
        if (fault) { wanted = false; offline(); return; }
        if (!accepts() || session.suspended() || busReset.active()) return;
        for (unsigned ep = 1; ep < endpointCount; ++ep) {
            Requests::View ready;
            if (!endpoints[ep].enabled || endpoints[ep].stalled || endpoints[ep].aborting ||
                endpoints[ep].config.type==DWC3::EndpointType::Isochronous || !requests.prepared(ep, ready)) continue;
            if (ready.scope != (ep == 1 ? nativeToken : session.generation())) { requests.cancel(ready.cookie); continue; }
            if (requests.publish(ready.cookie, epoch) != USB::Result::Success) { fault = true; break; }
            DWC3::DMASegment segment{ready.bus, ready.capacity};
            if (owner->submitPrepared(ep, ready.capacity ? &segment : nullptr,
                ready.capacity ? 1 : 0, ready.length, ready.cookie) != kIOReturnSuccess) { fault = true; break; }
        }
        if (fault) { wanted = false; offline(); }
    }
    void serviceIsochronous() {
        isoRequests.expire(nowUS());
        if (accepts() && !session.suspended() && !busReset.active() && isoRequests.prefetch()!=USB::Result::Success) fault=true;
        for (unsigned ep=2;ep<endpointCount && !fault;++ep) {
            auto &endpoint=endpoints[ep];auto *ring=endpoint.iso;
            if (!endpoint.enabled || endpoint.config.type!=DWC3::EndpointType::Isochronous || !ring) continue;
            USB::Result reason=USB::Result::Aborted;
            const bool cancel=endpoint.aborting || session.suspended() || busReset.active() || isoRequests.cancelled(ep,reason);
            USB::Result result=USB::Result::Success;
            if (cancel && (ring->pending() || ring->ownsHardware())) result=ring->cancel(reason);
            else if (!cancel && accepts()) {
                result=isoRequests.publish(ep,*ring,epoch);
                if (result==USB::Result::Success) {
                    result=ring->start(nowUS());
                    if (result==USB::Result::Timeout) {
                        isoRequests.cancel(ep,USB::Result::Timeout);result=ring->cancel(USB::Result::Timeout);
                    }
                    if (!ring->pending() && ring->active()) result=ring->idle();
                }
            }
            if (result!=USB::Result::Success && result!=USB::Result::Pending && result!=USB::Result::NotReady) fault=true;
            if (ring->faulted()) fault=true;
            if (ring->stopping()) {
                if (!endpoint.stopDeadline) endpoint.stopDeadline=nowUS()+10000;
                else if (nowUS()>=endpoint.stopDeadline) fault=true;
            }
            collectIsochronous(ep);
        }
        notifyIsochronous();
    }
    void policy(const Job &job) {
        if (!tracker || stopping || !session.accepts(job.generation)) return;
        switch (job.kind) {
        case Kind::Control: {
            const auto &n = job.control;
            if (n.kind == DWC3::ControlNotificationKind::Setup) {
                if (n.transaction != control.currentTransaction()) return;
                nativeToken = n.transaction; setup = n.setup; deadline = nowUS() + 5000000;
                endpoints[0].stalled = endpoints[1].stalled = false;
                dispatchingSetup = true;
                tracker->reset();
                tracker->inputSetupPacket(reinterpret_cast<UInt8 *>(&setup));
                dispatchingSetup = false;
                owner->setProperty("USBDeviceLastSetup",&setup,sizeof(setup));
                const auto status=tracker->processSetupPacket();
                owner->setProperty("USBDeviceLastSetupStatus",static_cast<UInt32>(status),32);
                if (status != kIOReturnSuccess) fault = true;
            } else if (n.transaction == nativeToken) {
                if (n.kind == DWC3::ControlNotificationKind::Data && !n.setup.in()) {
                    if (trackerState != 5 && trackerState != 6) { fault = true; return; }
                    tracker->inputDataPacket(static_cast<UInt8 *>(receive.bytes()), n.bytes);
                } else if (n.kind == DWC3::ControlNotificationKind::Status) {
                    deadline = 0;
                    if (trackerState == 8) tracker->inputDataPacket(nullptr, 0);
                } else if (n.kind == DWC3::ControlNotificationKind::Aborted) { deadline = 0; tracker->reset(); }
            }
            break;
        }
        case Kind::Reset: resetTracker(); owner->IOUSBDeviceController::handleUSBReset(); break;
        case Kind::Connect: owner->IOUSBDeviceController::handleUSBConnectionDone(); break;
        case Kind::Suspend: owner->IOUSBDeviceController::handleUSBSuspend(); break;
        case Kind::Resume: owner->IOUSBDeviceController::handleUSBResume(); break;
        case Kind::Disconnect: resetTracker(); owner->IOUSBDeviceController::handleUSBCableDisconnect(); break;
        default: break;
        }
    }
};

bool QcomUSBDevice::start(IOService *provider) {
    if (fState || fFamilyStarted) return false;
    auto *nub = OSDynamicCast(QcomUSBController, provider);
    if (!nub || nub->role() != USB::Role::Device || !nub->getWorkLoop() || !nub->open(this)) return false;
    USB::MMIO *io = nullptr; DWC3::Core<USB::MMIO> *core = nullptr;
    auto *loop = nub->getWorkLoop(); auto *setupGate = IOCommandGate::commandGate(this);
    if (!setupGate) { nub->close(this); return false; }
    if (loop->addEventSource(setupGate) != kIOReturnSuccess) { setupGate->release(); nub->close(this); return false; }
    setupGate->runAction([](OSObject *o, void *a, void *b, void *c, void *) -> IOReturn {
        auto *n = static_cast<QcomUSBController *>(a);
        *static_cast<USB::MMIO **>(b) = n->registers(static_cast<IOService *>(o), n->epoch());
        *static_cast<DWC3::Core<USB::MMIO> **>(c) = n->deviceCore(static_cast<IOService *>(o), n->epoch());
        return kIOReturnSuccess;
    }, nub, &io, &core);
    loop->removeEventSource(setupGate); setupGate->release();
    if (!io || !core) { nub->close(this); return false; }
    auto *mapper = nub->copyDeviceMapper();
    fState = new State(this, nub, *io, *core, mapper);
    if (!fState) { OSSafeReleaseNULL(mapper); nub->close(this); return false; }
    auto &s = *fState; s.opened = true;
    setProperty("USBDeviceSystemMapperPresent",IOMapper::gSystem!=nullptr);
    s.gate = IOCommandGate::commandGate(this);
    s.irq = IOInterruptEventSource::interruptEventSource(this, interrupt, nub, 0);
    s.timer = IOTimerEventSource::timerEventSource(this, timer);
    s.work = thread_call_allocate_with_options(worker, this, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
    s.dmaWork = thread_call_allocate_with_options(dmaWorker, this, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
    if (!s.pinLock || !s.gate || !s.irq || !s.timer || !s.work || !s.dmaWork) goto fail;
    if (s.loop->addEventSource(s.gate) != kIOReturnSuccess) goto fail;
    s.gateAdded = true;
    if (s.loop->addEventSource(s.irq) != kIOReturnSuccess) goto fail;
    s.irqAdded = true; s.irq->disable();
    if (s.loop->addEventSource(s.timer) != kIOReturnSuccess) goto fail;
    s.timerAdded = true;
    if (s.descriptors.allocate(State::DescriptorBytes, 64) != USB::Result::Success ||
        s.receive.allocate(65536, 64) != USB::Result::Success) goto fail;
    for (unsigned i = 0; i < 64; ++i) {
        s.requestDMA[i] = new USB::DeviceBounce<USB::IOMemoryAPI>(s.memory);
        if (!s.requestDMA[i] || !s.requests.attach(i, s.requestDMA[i])) goto fail;
        s.isoFrameDMA[i]=new USB::DeviceBounce<USB::IOMemoryAPI>(s.memory);
        if (!s.isoFrameDMA[i] || !s.isoDMA.attach(i,s.isoFrameDMA[i])) goto fail;
    }
    {
        auto *bytes = static_cast<UInt8 *>(s.descriptors.bytes());
        DWC3::ControlMemory memory{reinterpret_cast<DWC3::TRB *>(bytes),
            reinterpret_cast<DWC3::TRB *>(bytes + State::TRBBytes), s.descriptors.busAddress(),
            s.descriptors.busAddress() + State::TRBBytes, State::TRBs,
            bytes + State::SetupOffset, s.descriptors.busAddress() + State::SetupOffset};
        if (s.control.initialize(memory,64,s.epoch)!=USB::Result::Success) goto fail;
        for (unsigned ep = 2; ep < s.endpointCount; ++ep) {
            auto *td = new DWC3::Transfer<DWC3::Core<USB::MMIO>, USB::MMIO>(s.core, s.io);
            s.endpoints[ep].td = td;
            if (!td || td->initialize(ep, reinterpret_cast<DWC3::TRB *>(bytes + ep * State::TRBBytes),
                s.descriptors.busAddress() + ep * State::TRBBytes, State::TRBs, s.epoch) != USB::Result::Success) goto fail;
        }
    }
    // Native start creates its own command gate and mapper/pool using the
    // already-installed shared workloop. Do not reverse this order.
    fFamilyStarted = true; // Native start may invoke our virtual stop on failure.
    if (!IOUSBDeviceController::start(provider)) goto fail;
    if (s.resources.maximumSpeed==4) {
        // Append to the actual Family list before publishing the controller
        // or going on bus. No private descriptor responder bypasses Native
        // SETUP policy. An existing unknown SS capability is not overwritten.
        const auto installed=s.gated([&] {
            auto *capabilities=nativeDeviceCapabilities();
            if (!capabilities || capabilities->getCount()>64) return kIOReturnBadArgument;
            for (unsigned i=0;i<capabilities->getCount();++i) {
                auto *data=OSDynamicCast(OSData,capabilities->getObject(i));
                if (!data || data->getLength()<3 || data->getLength()>255) return kIOReturnBadArgument;
                const auto *p=static_cast<const UInt8 *>(data->getBytesNoCopy());
                if (p[0]!=data->getLength() || p[1]!=16 || p[2]==3) return kIOReturnBadArgument;
            }
            auto *descriptor=OSData::withBytes(USB::DeviceSuperSpeedCapability,sizeof(USB::DeviceSuperSpeedCapability));
            if (!descriptor) return kIOReturnNoMemory;
            const bool appended=capabilities->setObject(descriptor);descriptor->release();
            if (!appended) return kIOReturnNoMemory;
            s.superSpeedCapability=true;return kIOReturnSuccess;
        });
        if (installed!=kIOReturnSuccess) goto fail;
    }
    s.ready = true;
    setProperty("USBNativeStartupStage","ReadyForHost");
    readyForHost();
    setProperty("USBNativeStartupStage","CableConnect");
    handleUSBCableConnect();
    setProperty("USBNativeStartupStage","Ready");
    return true;
fail:
    stop(provider); return false;
}

IOWorkLoop *QcomUSBDevice::getWorkLoop() const { return fState ? fState->loop : nullptr; }
bool QcomUSBDevice::requestTerminate(IOService *, IOOptionBits) {
    const bool allowed=!pinned() || (!pinned(true) && fState && fState->nub->dmaRecoveryReady());
    if (!allowed) return false;
    if (fFamilyStarted && fState && fState->gateAdded && fState->tracker && !fState->released) {
        auto &s=*fState;
        const auto drained=s.gated([&] {
            // Phase1 asks us before marking the interface/Mux descendants
            // inactive. Native cable-disconnect deactivates the selected
            // configuration and sends WasDeactivated to each function. In
            // particular AppleUSBDeviceMux cancels both of its timers there;
            // its free() alone does not. Waiting until our stop() is too late:
            // IOKit has already stopped/freed the function descendants.
            // Close admission first so callback-driven I/O cannot restart DMA.
            s.ready=false;s.wanted=false;
            // The role parent may already have isolated DMA under BCR. Mark
            // the adapter offline and retire halted TDs before Native's
            // configuration-deactivation callbacks try to abort endpoints.
            const auto result=s.offline();
            if (result!=kIOReturnSuccess) return result;
            IOUSBDeviceController::handleUSBCableDisconnect();
            s.stopping=true;
            return kIOReturnSuccess;
        });
        if (drained!=kIOReturnSuccess) {pin();return false;}
        // didTerminate schedules stop only after the provider has closed.
        // The native off-bus hook above has halted and drained endpoint I/O;
        // close now so synchronous nub termination can finish. Keep opened
        // as the startup/cleanup flag until stop() releases our event sources.
        // Phase1 runs outside the shared workloop gate. IOKit's later stop
        // callout holds the provider's (same) gate, so join these workers now
        // while they can still acquire it and observe stopping.
        if (s.work) thread_call_cancel_wait(s.work);
        if (s.dmaWork) thread_call_cancel_wait(s.dmaWork);
        s.nub->close(this);
    }
    return true;
}
bool QcomUSBDevice::pinned(bool permanentOnly) const {
    if (!fState || !fState->pinLock) return false; // no DMA exists before lock creation
    const auto state=IOSimpleLockLockDisableInterrupt(fState->pinLock);
    const bool value=permanentOnly ? fPermanentPin:fPinned;
    IOSimpleLockUnlockEnableInterrupt(fState->pinLock,state);return value;
}
void QcomUSBDevice::pin(bool recoverable) {
    if (!fState || !fState->pinLock) return;
    const auto state=IOSimpleLockLockDisableInterrupt(fState->pinLock);
    if (!recoverable) fPermanentPin=true;
    if (!fPinned) { retain(); fPinned = true; }
    IOSimpleLockUnlockEnableInterrupt(fState->pinLock,state);
    if (fState) fState->nub->reportFault(this,fState->epoch);
    setProperty("USBDeviceControllerFault", true);
}
void QcomUSBDevice::unpin() {
    const auto state=IOSimpleLockLockDisableInterrupt(fState->pinLock);
    const bool drop=fPinned && !fPermanentPin;
    if (drop) fPinned=false;
    IOSimpleLockUnlockEnableInterrupt(fState->pinLock,state);
    if (drop) {setProperty("USBDeviceControllerFault",false);release();}
}
void QcomUSBDevice::stop(IOService *provider) {
    if (!fState || fState->released) return;
    auto &s = *fState;
    const bool recovery=s.nub->dmaRecoveryReady();
    IOReturn drained=kIOReturnSuccess;
    if (s.gateAdded) drained=s.gated([&] {
        s.stopping = true; s.ready = false; s.wanted = false;
        auto result = s.offline();
        if (result!=kIOReturnSuccess) return result;
        for (;;) {
            // Retry only cleanup failures, once, after the retained mapper's
            // actual root-removal proof. Contextless requests still own DMA
            // after their Native error callback and must drain as well.
            s.requests.retryIsolatedCleanup(recovery);s.isoDMA.retryIsolatedCleanup(recovery);
            s.notifyRequests();
            for (unsigned ep=1;ep<s.endpointCount;++ep)
                while (auto cookie=s.requests.pending(ep)) s.deliver(cookie);
            for (unsigned ep=2;ep<s.endpointCount;++ep)
                while (auto serial=s.isoRequests.pending(ep)) s.deliverIsochronous(serial);
            if (s.requests.empty() && s.isoRequests.empty()) break;
            if (s.requests.pinned() || s.isoDMA.pinned() ||
                (!s.requests.workPending() && !s.isoDMA.workPending())) return kIOReturnIOError;
            s.notifyRequests();s.sleepRequests();
        }
        s.resetTracker();
        return result;
    });
    if (drained!=kIOReturnSuccess || (pinned() && (pinned(true) || !recovery))) {pin();return;}
    if (s.work) thread_call_cancel_wait(s.work);
    if (s.dmaWork) thread_call_cancel_wait(s.dmaWork);
    if (pinned(true)) return; // an in-flight worker may have latched an invariant failure
    if (!s.requests.empty() || !s.isoRequests.empty()) { pin(); return; }
    if (fFamilyStarted) { IOUSBDeviceController::stop(provider); fFamilyStarted = false; }
    if (s.timerAdded) { s.loop->removeEventSource(s.timer); s.timerAdded = false; }
    if (s.irqAdded) { s.loop->removeEventSource(s.irq); s.irqAdded = false; }
    if (s.gateAdded) { s.loop->removeEventSource(s.gate); s.gateAdded = false; }
    if (s.descriptors.release() != USB::Result::Success || s.receive.release() != USB::Result::Success) {
        pin(s.descriptors.cleanupRetryable() || s.receive.cleanupRetryable());return;
    }
    if (pinned(true)) return;
    if (s.opened) { s.nub->close(this); s.opened = false; }
    // A Family reference can outlive the role nub. Later free()/stop() must
    // not dereference its retired parent Core while repeating offline().
    s.released=true;
    if (s.pinLock) unpin();
}
void QcomUSBDevice::free() {
    if (fState) {
        stop(fState->nub);
        if (pinned()) return;
        auto *s = fState; fState = nullptr;
        if (s->work) thread_call_free(s->work);
        if (s->dmaWork) thread_call_free(s->dmaWork);
        OSSafeReleaseNULL(s->tracker); OSSafeReleaseNULL(s->timer); OSSafeReleaseNULL(s->irq);
        OSSafeReleaseNULL(s->gate);
        for (auto &ep : s->endpoints) { delete ep.td;delete ep.iso; }
        for (auto *dma : s->requestDMA) delete dma;
        for (auto *dma:s->isoFrameDMA) delete dma;
        OSSafeReleaseNULL(s->mapper); OSSafeReleaseNULL(s->nub); OSSafeReleaseNULL(s->loop);
        if (s->pinLock) IOSimpleLockFree(s->pinLock);
        delete s;
    }
    IOUSBDeviceController::free();
}
void QcomUSBDevice::interrupt(OSObject *owner, IOInterruptEventSource *, int) {
    auto *self = static_cast<QcomUSBDevice *>(owner);
    if (self->fState) self->fState->poll();
}
void QcomUSBDevice::timer(OSObject *owner, IOTimerEventSource *) {
    auto *self = static_cast<QcomUSBDevice *>(owner); auto *s = self->fState;
    if (!s || s->stopping || !s->running) return;
    s->poll();
    if (s->deadline && nowUS() >= s->deadline) { s->fault = true; s->wanted = false; s->offline(); }
    if (s->running) s->timer->setTimeoutMS(5);
}
void QcomUSBDevice::worker(void *arg, void *) {
    auto *self = static_cast<QcomUSBDevice *>(arg); auto &s = *self->fState;
    for (;;) {
        State::Job job{}; bool found = false;
        s.gated([&] {
            if (s.count) { job = s.jobs[s.head]; s.head = (s.head + 1) % State::JobCount; --s.count; found = true; }
            return kIOReturnSuccess;
        });
        if (!found) break;
        if (job.kind == State::Kind::Transfer) {
            s.gated([&] { s.deliver(job.completion.cookie); s.serviceRequests(); return kIOReturnSuccess; });
        } else if (job.kind==State::Kind::Isochronous) {
            s.gated([&] { s.deliverIsochronous(job.completion.cookie);s.serviceRequests();return kIOReturnSuccess; });
        } else s.gated([&] {
            // Another thread may be draining the preceding request while
            // sleeping with this same gate released. Wait before dispatching
            // a new SETUP that could submit to its still-aborting endpoint.
            while ((s.abortDepth || s.quiescing || s.busReset.active()) && !s.stopping && !s.fault) s.sleepRequests();
            s.policy(job); return kIOReturnSuccess;
        });
    }
    s.gated([&] { if (s.wanted && !s.running && !s.stopping && !s.fault) return s.online(); return kIOReturnSuccess; });
}
void QcomUSBDevice::dmaWorker(void *arg, void *) {
    auto *self = static_cast<QcomUSBDevice *>(arg); auto &s = *self->fState;
    for (;;) {
        State::Requests::Work work;bool found=false,periodic=false;
        s.gated([&] {
            periodic=s.isoDMA.needsWork() && (s.dmaPeriodicTurn || !s.requests.needsWork());
            found=periodic ? s.isoDMA.beginWork(work):s.requests.beginWork(work);
            s.dmaPeriodicTurn=!periodic;return kIOReturnSuccess;
        });
        if (!found) return;
        auto &queue=periodic ? s.isoDMA:s.requests;
        // execute() can release the retained client on cleanup. Inspect it
        // only for a preparation ticket, before executing that ticket.
        const UInt64 clientLength=work.preparing && work.buffer ? work.buffer->getLength():0;
        const UInt64 clientDirection=work.preparing && work.buffer ? work.buffer->getDirection():0;
        auto result=queue.execute(work);
        s.gated([&] {
            if (result.result!=USB::Result::Success) {
                UInt64 diagnostic[]={work.endpoint,work.cookie,UInt64(result.result),UInt64(work.preparing),
                    work.length,clientLength,clientDirection,UInt64(result.empty)};
                self->setProperty("USBDeviceLastDMAError",diagnostic,sizeof(diagnostic));
                self->setProperty("USBDeviceLastDMAPreparation",s.memory.preparationDiagnostic,
                    sizeof(s.memory.preparationDiagnostic));
            }
            if (work.endpoint==1) {
                UInt64 diagnostic[]={work.cookie,UInt64(work.preparing),UInt64(result.result),
                    work.length,UInt64(result.empty),UInt64(result.released)};
                self->setProperty("USBDeviceEP0DMAWork",diagnostic,sizeof(diagnostic));
            }
            if (!queue.commit(work,result)) { s.fault=true;self->pin(); }
            if (queue.pinned()) { s.fault=true;self->pin(true); }
            s.notifyRequests(); s.serviceRequests();
            return kIOReturnSuccess;
        });
    }
}

IOReturn QcomUSBDevice::goOnBus(bool enabled) {
    if (!fState || !fState->gateAdded) return kIOReturnNotReady;
    auto &s = *fState;
    const auto result=s.gated([&] {
        if (enabled) return s.online();
        const bool wasQuiescing = s.quiescing;
        s.quiescing = true; s.wanted = false; auto result = s.offline();
        for (unsigned ep = 1; ep < s.endpointCount; ++ep)
            while (auto cookie = s.requests.pending(ep)) s.deliver(cookie);
        for (unsigned ep=2;ep<s.endpointCount;++ep)
            while (auto serial=s.isoRequests.pending(ep)) s.deliverIsochronous(serial);
        s.resetTracker();
        s.wanted = false; s.quiescing = wasQuiescing;
        s.gate->commandWakeup(&s.requests); return result;
    });
    setProperty("USBDeviceBusRequested",enabled);
    setProperty("USBDeviceBusStatus",static_cast<UInt32>(result),32);
    s.gated([&] {
        if(!s.nub->dmaIsolated()) {
            UInt32 registers[]={s.io.read(DWC3::GCTL),s.io.read(DWC3::GUSB2PHYCFG),s.io.read(DWC3::DCFG),
                s.io.read(DWC3::DCTL),s.io.read(DWC3::DSTS),s.io.read(DWC3::DALEPENA),
                s.io.read(DWC3::eventAddressLo(0)),s.io.read(DWC3::eventSize(0)),s.io.read(DWC3::eventCount(0)),
                s.io.read(DWC3::endpoint(0)+12),s.io.read(DWC3::endpoint(1)+12)};
            setProperty("USBDeviceBusRegisters",registers,sizeof(registers));
        }
        return kIOReturnSuccess;
    });
    return result;
}
bool QcomUSBDevice::connectedWithAccessoryCable() {
    // Native means USBHostAltConfig / USB3HostAltConfig here, not the
    // presence of an ordinary host cable. This Type-C backend supplies no
    // authenticated accessory/alternate-configuration route.
    return false;
}
void QcomUSBDevice::nativeSlot175(UInt32) {
    // Panic path: no allocation, scheduler sleep, command gate or PMIC calls.
    if (fState && fState->opened) { fState->running = false; fState->core.halt(); }
}
void QcomUSBDevice::nativeSlot199() { goOnBus(false); }
void QcomUSBDevice::handleSetAddressPreStatusPhase(int address) {
    if (fState && fState->gateAdded) fState->gated([&] {
        return fState->accepts() && fState->running ? ioResult(fState->core.setAddress(unsigned(address))) : kIOReturnNotReady;
    });
}
void QcomUSBDevice::handleSetAddressPostStatusPhase(int) {}
UInt32 QcomUSBDevice::getConnectionSpeed() {
    if (!fState) return UInt32(IOUSBDeviceConnectionSpeed::Unknown);
    UInt32 result = UInt32(IOUSBDeviceConnectionSpeed::Full);
    fState->gated([&] {
        if (fState->speed == USB::Speed::Super) result = UInt32(IOUSBDeviceConnectionSpeed::Super);
        else if (fState->speed == USB::Speed::High) result = UInt32(IOUSBDeviceConnectionSpeed::High);
        return kIOReturnSuccess;
    });
    return result;
}
UInt32 QcomUSBDevice::getMaxSpeedCapability() {
    return UInt32(fState && fState->resources.maximumSpeed==4 ?
        IOUSBDeviceConnectionSpeed::Super:IOUSBDeviceConnectionSpeed::High);
}
uintptr_t QcomUSBDevice::nativeSlot208() {
    // Pinned AppleSynopsysOTGDevice reports the minimum supported speed as
    // Full=1. The Family default0 is not a USB speed and feeds pipe/config
    // minimum-speed calculation before CONNECT_DONE.
    return UInt32(IOUSBDeviceConnectionSpeed::Full);
}
IOReturn QcomUSBDevice::createPipeGated(tCreatePipeParameters &parameters,IOUSBDevicePipe *&pipe) {
    pipe=nullptr;
    if (!fState || !fState->opened || fState->stopping || fState->released ||
        !fState->nub->accepts(this,fState->epoch)) return kIOReturnNotReady;
    auto checked=parameters;
    const auto maximum=getMaxSpeedCapability()==UInt32(IOUSBDeviceConnectionSpeed::Super) ?
        USB::Speed::Super:USB::Speed::High;
    const auto result=USB::devicePipeParameters(maximum,checked.type,checked.direction,
        checked.maximumPacketSize,checked.interval,checked.synchronization);
    setProperty("USBDevicePipeParameters",&parameters,sizeof(parameters));
    const auto status=result==USB::Result::Success ? IOUSBDeviceController::createPipeGated(checked,pipe):ioResult(result);
    setProperty("USBDevicePipeCreateStatus",static_cast<UInt32>(status),32);
    return status;
}
IOUSBDeviceControlRequest *QcomUSBDevice::createControlRequestTracker() {
    if (!fState || fState->tracker) return nullptr;
    auto *tracker = IOUSBDeviceControlRequest::withControllerOptions(this, 128, 0, 1);
    if (tracker && !fState->tracker) { tracker->retain(); fState->tracker = tracker; }
    return tracker;
}
void QcomUSBDevice::handleControlRequestStateChange(IOUSBDeviceControlRequest::tControlRequestState,
                                                   IOUSBDeviceControlRequest::tControlRequestState next) {
    if (!fState) return;
    auto &s = *fState;
    s.gated([&] {
        s.trackerState = UInt32(next);
        if (!s.running || s.stopping || s.dispatchingSetup || s.nativeToken != s.control.currentTransaction())
            return kIOReturnSuccess;
        // State4 waits for a function's OUT receiver. States5/6 have accepted
        // that receiver. Starting DATA in state4 would panic the native tracker.
        if ((s.trackerState == 5 || s.trackerState == 6) && !s.setup.in() && s.setup.length &&
            s.control.currentState() == DWC3::ControlState::Dispatch) {
            const unsigned packet = s.endpoints[0].config.maximumPacketSize;
            DWC3::DMASegment segment{s.receive.busAddress(),
                (unsigned(s.setup.length) + packet - 1) / packet * packet};
            if (s.receive.publish(s.epoch) != USB::Result::Success) return kIOReturnIOError;
            auto r = s.control.supplyData(s.nativeToken, &segment, 1, s.setup.length);
            if (r != USB::Result::Success) { s.fault = true; return ioResult(r); }
        } else if (s.trackerState == 8 && s.setup.in() && s.setup.length) {
            return ioResult(s.control.allowStatus(s.nativeToken));
        }
        return kIOReturnSuccess;
    });
}

IOReturn QcomUSBDevice::provideEndpointIDsForConfiguration(OSArray *interfaces, UInt8 *ids) {
    if (!fState || !interfaces) return kIOReturnBadArgument;
    setProperty("USBDeviceInterfaceCount",interfaces->getCount(),32);
    unsigned firstIn = 1, firstOut = 1, index = 0;
    const unsigned limit = fState->endpointCount;
    for (unsigned i = 0; i < interfaces->getCount(); ++i) {
        auto *interface = IOUSBDeviceInterface::fromObject(interfaces->getObject(i));
        if (!interface) return kIOReturnBadArgument;
        const int alternatives = interface->getAlternateSettingCount();
        setProperty("USBDeviceAlternateSettingCount",static_cast<UInt32>(alternatives),32);
        if (alternatives < 0 || alternatives > 256) return kIOReturnUnsupported;
        unsigned reserveIn = 0, reserveOut = 0;
        for (int alt = 0; alt < alternatives; ++alt) {
            const int pipes = interface->getPipeCountForAlternateSetting(UInt8(alt));
            if (pipes < 0 || pipes > 30 || (pipes && !ids)) return kIOReturnNoResources;
            unsigned usedIn = 0, usedOut = 0;
            for (int p = 0; p < pipes; ++p) {
                auto *pipe = interface->getPipeForAlternateSetting(p, UInt8(alt));
                if (!pipe) return kIOReturnBadArgument;
                const int direction = pipe->getDirection();
                if (direction != 0 && direction != 1) return kIOReturnBadArgument;
                const unsigned number = direction ? firstIn + usedIn++ : firstOut + usedOut++;
                if (number > 15 || number * 2 + unsigned(direction) >= limit) return kIOReturnNoResources;
                ids[index++] = UInt8(number | (direction ? 0x80 : 0));
            }
            if (usedIn > reserveIn) reserveIn = usedIn;
            if (usedOut > reserveOut) reserveOut = usedOut;
        }
        firstIn += reserveIn; firstOut += reserveOut;
    }
    setProperty("USBDeviceEndpointIDs",ids,index);
    return kIOReturnSuccess;
}
void QcomUSBDevice::prepareConfiguration(OSArray *) {
    if (!fState || !fState->opened || !fState->gateAdded) return;
    for (unsigned ep = 2; ep < fState->endpointCount; ++ep)
        if (fState->endpoints[ep].enabled) deactivateEndpoint(int(ep / 2 | ((ep & 1) ? 0x80 : 0)));
    fState->gated([&] {
        if (!fState->accepts() || !fState->running || fState->abortDepth) return kIOReturnNotReady;
        const auto result=fState->core.beginEndpointConfiguration(2);
        if (result!=USB::Result::Success) { fState->fault=true;fState->wanted=false;fState->offline(); }
        return ioResult(result);
    });
}
IOReturn QcomUSBDevice::activateEndpoint(int address, int type, int packet, int interval,
                                       tIOUSBDeviceInterfacePipeProperty property) {
    if (!fState || physical(address) >= fState->endpointCount) return kIOReturnBadArgument;
    auto &s = *fState; const unsigned ep = physical(address);
    return s.gated([&] {
        if (!s.accepts() || !s.running || ep < 2) return kIOReturnNotReady;
        if (s.endpoints[ep].enabled || s.endpoints[ep].aborting) return kIOReturnBusy;
        DWC3::EndpointConfiguration config{};
        const auto configured=USB::deviceEndpointConfiguration(ep,type,s.speed,unsigned(packet),unsigned(interval),
            config,UInt32(property));
        if (configured!=USB::Result::Success) return ioResult(configured);
        delete s.endpoints[ep].iso;s.endpoints[ep].iso=nullptr;
        if (config.type==DWC3::EndpointType::Isochronous) {
            auto *ring=new State::IsoRing(s.core,s.io);s.endpoints[ep].iso=ring;
            auto *bytes=static_cast<UInt8 *>(s.descriptors.bytes());
            if (!ring) return kIOReturnNoMemory;
            const auto result=ring->initialize(config,reinterpret_cast<DWC3::TRB *>(bytes+ep*State::TRBBytes),
                s.descriptors.busAddress()+ep*State::TRBBytes,State::TRBs,s.epoch);
            if (result!=USB::Result::Success) return ioResult(result);
        }
        auto r = s.core.configureEndpoint(config);
        if (r == USB::Result::Success) {
            s.endpoints[ep].config = config;s.endpoints[ep].enabled = true;s.endpoints[ep].stalled=false;
        }
        return ioResult(r);
    });
}
void QcomUSBDevice::deactivateEndpoint(int address) {
    if (!fState || !fState->opened || !fState->gateAdded || physical(address) >= fState->endpointCount) return;
    handleAbortEndpoint(address);
    auto &s = *fState; const unsigned ep = physical(address);
    s.gated([&] {
        s.endpoints[ep].enabled = false;
        if (!s.running) return kIOReturnSuccess;
        auto result = s.core.disableEndpoint(ep);
        if (result != USB::Result::Success) { s.fault = true; s.wanted = false; s.offline(); }
        return ioResult(result);
    });
}
void QcomUSBDevice::handleAbortEndpoint(int address) {
    if (!fState || !fState->opened || !fState->gateAdded || physical(address) >= fState->endpointCount) return;
    auto &s = *fState; const unsigned ep = physical(address);
    s.gated([&] {
        s.abortEndpoint(ep);
        if (!s.opened || !s.running || s.stopping) return kIOReturnSuccess;
        if (ep < 2) {
            if (!s.dispatchingSetup && s.nativeToken && s.nativeToken == s.control.currentTransaction()) {
                DWC3::ControlNotification note;
                auto r = s.control.reject(s.nativeToken, note); s.controlEvent(note); return ioResult(r);
            }
            return kIOReturnSuccess;
        }
        return kIOReturnSuccess;
    });
}
void QcomUSBDevice::handleStallEndpoint(int address, bool stalled) {
    if (!fState || !fState->opened || !fState->gateAdded || physical(address) >= fState->endpointCount) return;
    auto &s = *fState; const unsigned ep = physical(address);
    s.gated([&] {
        if (s.endpoints[ep].config.type==DWC3::EndpointType::Isochronous) return kIOReturnSuccess;
        s.endpoints[ep].stalled = stalled;
        if (!s.running) return kIOReturnSuccess;
        if (ep < 2) {
            if(stalled) {
                UInt64 snapshot[]={s.nativeToken,s.trackerState,UInt64(s.control.currentState())};
                setProperty("USBDeviceControlStall",snapshot,sizeof(snapshot));
            }
            if (stalled && s.nativeToken == s.control.currentTransaction()) {
                DWC3::ControlNotification n; auto r = s.control.reject(s.nativeToken, n); s.controlEvent(n); return ioResult(r);
            }
            return kIOReturnSuccess;
        }
        if (stalled) s.abortEndpoint(ep);
        if (!s.opened || !s.running || s.stopping) return kIOReturnSuccess;
        UInt32 reply = 0;
        auto result = s.core.command(ep, stalled ? DWC3::Command::SetStall : DWC3::Command::ClearStall,
            0, 0, 0, 0, reply);
        if (result != USB::Result::Success) { s.fault = true; s.wanted = false; s.offline(); }
        else s.serviceRequests();
        return ioResult(result);
    });
}
bool QcomUSBDevice::isEndpointStalled(int address) {
    if (!fState || physical(address) >= fState->endpointCount) return false;
    bool result = false;
    fState->gated([&] { result = fState->endpoints[physical(address)].stalled; return kIOReturnSuccess; });
    return result;
}
void QcomUSBDevice::resetEndpointDataToggle(int address) { handleStallEndpoint(address, false); }
IOBufferMemoryDescriptor *QcomUSBDevice::createIOBuffer(UInt32 options, UInt64 capacity) {
    if (!fState || !capacity || capacity > 1024 * 1024 || (options & ~(kIODirectionInOut | kIOMemoryPageable))) return nullptr;
    // These are Family's CPU/client buffers. Hardware accesses the separate,
    // mapper-bound bounce owned by the request engine, never this allocation.
    return IOKitMemory::allocateBuffer(options,capacity,64);
}
IOReturn QcomUSBDevice::doRemoteWakeGated() {
    if (!fState) return kIOReturnNotReady;
    auto &s = *fState;
    return s.gated([&] {
        if (!s.accepts() || !s.running || !s.session.suspended()) return kIOReturnNotReady;
        const auto result = s.core.wakeUSB2();
        if (result == USB::Result::Success) {
            s.session.suspend(false);
            IOUSBDeviceController::handleUSBResume();
        }
        return ioResult(result);
    });
}
IOReturn QcomUSBDevice::nativeSlot203(UInt32 interfaceNumber) {
    if (!fState) return kIOReturnNotReady;
    auto &s = *fState;
    return s.gated([&] {
        if (!s.accepts() || !s.running) return kIOReturnNotReady;
        // tryRemoteWakeThreadCallGated owns function policy and resumes that
        // function after success. Bus resume still follows the real U0 event.
        return ioResult(s.core.wakeUSB3(interfaceNumber));
    });
}
IOReturn QcomUSBDevice::nativeSlot233(UInt32 speed, UInt32 selector) {
    if (selector<1 || selector>2 || speed<1 || speed>3) return kIOReturnBadArgument;
    if (!fState) return kIOReturnNotReady;
    auto &s=*fState;
    return s.gated([&] {
        USB3LinkPolicy policy{};
        if (!copyUSB3LinkPolicy(policy)) return kIOReturnBadArgument;
        const bool super=speed==UInt32(IOUSBDeviceConnectionSpeed::Super);
        const bool u1=super && policy.u1Enabled,u2=super && policy.u2Enabled;
        // Native reset calls this hook for FS and SS even while the adapter
        // is offline. A proven halt already prevents either transition.
        if (!s.running && !u1 && !u2 && s.core.stopped()) return kIOReturnSuccess;
        if (!s.accepts() || !s.running) return kIOReturnNotReady;
        const auto result=s.core.setUSB3LinkPolicy(u1,u2,policy.u1PEL,policy.u2PEL);
        if (result!=USB::Result::Success) {
            // Family has already updated its policy bytes. Do not continue
            // with a successful GET_STATUS and an unapplied hardware state.
            s.fault=true;s.wanted=false;
        }
        return ioResult(result);
    });
}
IOReturn QcomUSBDevice::ioGated(_IOUSBDeviceIORequestTag *tag) {
    if (!fState || !tag) return kIOReturnBadArgument;
    auto &s = *fState; const unsigned ep = physical(tag->endpoint);
    if(ep==1) {
        setProperty("USBDeviceNativeIOEndpoint",static_cast<UInt32>(tag->endpoint),32);
        setProperty("USBDeviceNativeIOStatus",static_cast<UInt32>(kIOReturnNotReady),32);
        setProperty("USBDeviceNativeIOHasDMA",tag->dma!=nullptr);
    }
    if (ep >= s.endpointCount || !ep || tag->dma) return kIOReturnUnsupported;
    return s.gated([&] {
        if(ep==1) {
            const UInt32 flags=UInt32(s.ready) | UInt32(s.running)<<1 | UInt32(s.endpoints[ep].enabled)<<2 |
                UInt32(s.endpoints[ep].aborting)<<3 | UInt32(s.quiescing)<<4 | UInt32(s.fault)<<5;
            setProperty("USBDeviceNativeIOAdmission",flags,32);
        }
        if (!s.accepts() || !s.running || !s.endpoints[ep].enabled || s.endpoints[ep].aborting)
            return kIOReturnNotReady;
        const auto &config=s.endpoints[ep].config;
        const UInt64 deadline=tag->timeoutMS ? nowUS()+UInt64(tag->timeoutMS)*1000:0;
        if (tag->isochronous) {
            USB::Result oldReason;
            if (s.isoRequests.cancelled(ep,oldReason)) return kIOReturnBusy;
            if (config.type!=DWC3::EndpointType::Isochronous || !s.endpoints[ep].iso ||
                !tag->result.frames || !tag->lengthOrFrameCount || tag->lengthOrFrameCount>State::Iso::MaximumFrames)
                return kIOReturnBadArgument;
            auto *job=new State::IsoRequest;
            if (!job) return kIOReturnNoMemory;
            job->tag=tag;
            for (unsigned n=0;n<tag->lengthOrFrameCount;++n) job->frames[n].requested=tag->result.frames[n].requested;
            const auto result=s.isoRequests.admit(job->request,job->frames,unsigned(tag->lengthOrFrameCount),tag->buffer,
                ep,config.maximumPacketSize,config.bytesPerInterval(),s.session.generation(),deadline,job);
            if (result!=USB::Result::Success) { delete job;return ioResult(result); }
            s.serviceRequests();s.notifyRequests();return kIOReturnSuccess;
        }
        if (config.type==DWC3::EndpointType::Isochronous ||
            tag->lengthOrFrameCount>USB::DMARegion<USB::IOMemoryAPI>::MaximumBytes) return kIOReturnBadArgument;
        if (ep == 1 && (!s.nativeToken || s.nativeToken != s.control.currentTransaction())) return kIOReturnNotReady;
        if(ep==1 && s.setup.in() && s.setup.request==6 && (s.setup.value>>8)==1 &&
            tag->buffer && tag->lengthOrFrameCount==18) {
            UInt8 descriptor[18]{};
            if(tag->buffer->readBytes(0,descriptor,sizeof(descriptor))==sizeof(descriptor))
                setProperty("USBDeviceLastDeviceDescriptor",descriptor,sizeof(descriptor));
        }
        UInt64 cookie = 0;
        const auto result = s.requests.admit(ep, tag->buffer, UInt32(tag->lengthOrFrameCount),
            s.endpoints[ep].config.maximumPacketSize, ep == 1 ? s.nativeToken : s.session.generation(),
            deadline, tag, cookie);
        if(ep==1)setProperty("USBDeviceNativeIOStatus",static_cast<UInt32>(ioResult(result)),32);
        if(ep==1) {
            UInt64 lengths[]={s.setup.length,tag->lengthOrFrameCount};
            setProperty("USBDeviceControlLengths",lengths,sizeof(lengths));
        }
        if (result == USB::Result::Success) s.notifyRequests();
        return ioResult(result);
    });
}
IOReturn QcomUSBDevice::submitPrepared(unsigned ep, const DWC3::DMASegment *segments,
                                     unsigned count, UInt32 length, UInt64 cookie) {
    if (!fState || !cookie || ep >= fState->endpointCount) return kIOReturnBadArgument;
    auto &s = *fState;
    return s.gated([&] {
        if (!s.accepts() || !s.running || s.session.suspended() || !s.endpoints[ep].enabled) return kIOReturnNotReady;
        if (ep == 0) return kIOReturnUnsupported;
        if (ep == 1) {
            if (s.inCookie || s.nativeToken != s.control.currentTransaction()) return kIOReturnBusy;
            const bool status = !s.setup.in() || !s.setup.length;
            if (status && s.trackerState != 7 && !(s.setup.in() && !s.setup.length && s.trackerState == 3))
                return kIOReturnNotReady;
            auto r = !status ?
                s.control.supplyData(s.nativeToken, segments, count, length) :
                (length ? USB::Result::Invalid : s.control.allowStatus(s.nativeToken));
            UInt64 submitted[]={s.nativeToken,length,UInt64(r),UInt64(s.control.currentState())};
            setProperty("USBDeviceControlSubmitted",submitted,sizeof(submitted));
            if (r == USB::Result::Success || r == USB::Result::Pending) { s.inCookie = cookie; s.inStatus = status; }
            return ioResult(r);
        }
        return ioResult(s.endpoints[ep].td->submit(segments, count, length,
            s.endpoints[ep].config.maximumPacketSize, DWC3::TRBType::Normal, cookie));
    });
}
