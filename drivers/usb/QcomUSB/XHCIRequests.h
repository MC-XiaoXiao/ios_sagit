// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "XHCITransfers.h"
#include "XHCIIsochronous.h"
#include "USBHostDMA.h"

namespace XHCI {
// Called only after DMA retirement and removal from the HCD's Native lookup.
// The Family may batch kind3 completions or reuse the object synchronously.
// Do not inspect it, complete its MD, or invoke its client callback here.
template<class NativeRequest,class Completer,class Status>
auto handoffRequest(NativeRequest &request,Completer &completer,Status status,uint32_t bytes,uint64_t stamp) {
    request.setStatus(status);
    request.setBytesTransferred(bytes);
    request.setTimeStamp(stamp);
    return completer.enqueueRequest(&request);
}

// Native MD preparation belongs to the Family. This owner only prepares its
// private HCD allocation, and keeps it until an actual queue terminal record
// has been acknowledged. All prepare/finish calls execute on a worker.
template<class API> class RequestMemory {
    USB::HostBounce<API> payload;
    TransferDescriptor<128> descriptor;
    uint64_t epoch=0,command=0;
    uintptr_t owner=0;
    const void *queueIdentity=nullptr;
    uint32_t actual=0;
    bool ready=false,acknowledged=false,valid=false;
public:
    explicit RequestMemory(API &api) : payload(api) {}
    RequestMemory(const RequestMemory &)=delete;
    RequestMemory &operator=(const RequestMemory &)=delete;
    USB::DMAState currentState() const { return payload.currentState(); }
    bool cleanupRetryable() const {return payload.cleanupRetryable();}
    uint64_t ticket() const { return command; }
    uint32_t transferred() const { return actual; }
    bool validData() const { return valid; }
    const auto &transferDescriptor() const { return descriptor; }
    USB::Result scheduleIsochronous(const EndpointConfiguration &configuration,IsoSchedule schedule) {
        if (!ready || command || payload.currentState()!=USB::DMAState::Ready || schedule.extendedTBC)
            return USB::Result::NotReady;
        return descriptor.scheduleIsochronous(configuration,schedule.frame,schedule.asap);
    }
    USB::Result prepare(typename API::Memory *native,const EndpointConfiguration &configuration,
        uint32_t bytes,unsigned bits,const USB::SetupPacket *setup=nullptr) {
        if (ready || payload.currentState()!=USB::DMAState::Empty) return USB::Result::Busy;
        const bool input=setup ? setup->in() : bool(configuration.address&128);
        auto r=payload.prepareFamilyBuffer(native,bytes,input,65536);
        if (r!=USB::Result::Success) return r;
        const TransferSegment segment{payload.busAddress(),bytes};
        r=descriptor.build(configuration,bytes ? &segment : nullptr,bytes ? 1 : 0,bytes,bits,setup);
        if (r==USB::Result::Success) ready=true;
        return r;
    }
    USB::Result prepareIsochronous(typename API::Memory *native,const EndpointConfiguration &configuration,
        const IOUSBHostIsochronousTransaction &transaction,unsigned bits,IsoSchedule schedule) {
        if (ready || payload.currentState()!=USB::DMAState::Empty) return USB::Result::Busy;
        if (configuration.type!=EndpointType::Isochronous) return USB::Result::Invalid;
        if (transaction.options&~uint32_t(kIsochronousTransactionOptionsWrap)) return USB::Result::Unsupported;
        if (transaction.requestCount>UINT32_MAX-transaction.offset) return USB::Result::Invalid;
        // Validate the TD before retaining client memory or allocating DMA.
        // A suitably aligned private payload needs at most one payload TRB.
        const TransferSegment probe{65536,transaction.requestCount};
        auto r=descriptor.build(configuration,transaction.requestCount ? &probe : nullptr,
            transaction.requestCount ? 1 : 0,transaction.requestCount,bits,nullptr,schedule);
        if (r!=USB::Result::Success) return r;
        r=payload.prepareFamilyBuffer(native,transaction.requestCount,bool(configuration.address&128),65536,
            transaction.offset,bool(transaction.options&kIsochronousTransactionOptionsWrap));
        if (r!=USB::Result::Success) return r;
        const TransferSegment segment{payload.busAddress(),transaction.requestCount};
        r=descriptor.build(configuration,transaction.requestCount ? &segment : nullptr,
            transaction.requestCount ? 1 : 0,transaction.requestCount,bits,nullptr,schedule);
        if (r==USB::Result::Success) ready=true;
        return r;
    }
    template<class Queue,class Ring> USB::Result submit(Queue &queue,const Ring &ring,uintptr_t retainedOwner,
        uint64_t generation,uint64_t now,uint64_t timeout) {
        if (!ready || command || !generation || !retainedOwner) return USB::Result::Invalid;
        auto r=descriptor.configuration().type==EndpointType::Isochronous ?
            descriptor.alignForRing(ring) : descriptor.alignForEmptyRing(ring);
        if (r!=USB::Result::Success) return r;
        r=payload.publish(generation);
        if (r!=USB::Result::Success) return r;
        epoch=generation;owner=retainedOwner;queueIdentity=&queue;
        r=queue.submit(descriptor,owner,now,timeout,command);
        // The real queue returns ticket0 only before retaining/publishing a
        // transfer. Undo this software ownership mark on that exact path.
        if (!command) {
            const auto retired=payload.retire(epoch);
            if (retired!=USB::Result::Success) { payload.quarantine();return retired; }
            if (r==USB::Result::Success) return USB::Result::HardwareError;
        }
        return r;
    }
    template<class Queue> USB::Result retire(Queue &queue,uintptr_t &releasedOwner) {
        releasedOwner=0;TransferRecord record{};
        if (!command || acknowledged || queueIdentity!=&queue || queue.generation()!=epoch ||
            !queue.copy(command,record) || record.owner!=owner)
            return USB::Result::Invalid;
        if (record.phase!=TransferPhase::Completed && record.phase!=TransferPhase::Isolated) {
            return USB::Result::Busy;
        }
        // Isolated records are produced only by this real queue's backend
        // isolation proof. Endpoint recovery produces Completed records.
        if (record.phase==TransferPhase::Completed) {
            const auto retired=payload.currentState()==USB::DMAState::Owned ? payload.retire(epoch) :
                payload.retireAfterEndpointStop(epoch);
            if (retired!=USB::Result::Success) return retired;
        } else {
            payload.retireAfterIsolation();
        }
        if (payload.currentState()!=USB::DMAState::Ready) return USB::Result::NotReady;
        const auto r=queue.acknowledge(command,owner,releasedOwner);
        if (r!=USB::Result::Success) return r;
        acknowledged=true;actual=record.transferred;valid=record.dataValid;
        return USB::Result::Success;
    }
    void quarantine() { payload.quarantine(); }
    USB::Result finish(bool copyData) {
        if (command && !acknowledged) return USB::Result::Busy;
        if (payload.currentState()==USB::DMAState::Empty) return USB::Result::Success;
        return payload.finish(copyData && valid ? actual : 0,copyData && valid);
    }
};
}
