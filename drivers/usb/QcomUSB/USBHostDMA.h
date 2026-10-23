// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "USBDMA.h"

namespace USB {
// AppleUSBHostRequest::getPreparedBuffer supplies the Family-owned prepared
// MD. The HCD retains it for copies but never prepares/completes it again.
// Family preparation must outlive finish(); native completion is enqueued
// only afterwards. Allocation, copies and cleanup run outside workloop gates.
template<class API> class HostBounce {
    API &api;
    DMARegion<API> hardware;
    typename API::Memory *client=nullptr;
    uint32_t length=0, finishedBytes=0;
    uint64_t epoch=0,clientOffset=0,clientLength=0;
    DMAState state=DMAState::Empty;
    Result copied=Result::Success;
    bool live=false, receive=false, finishing=false, copyBack=false, copiedBack=false;
    bool copyClient(bool input,uint32_t count) {
        uint64_t at=clientOffset;
        uint32_t copied=0;
        while (copied<count) {
            const auto available=clientLength-at;
            const auto remaining=count-copied;
            const auto bytes=available<remaining ? uint32_t(available) : remaining;
            auto *data=static_cast<uint8_t *>(hardware.bytes())+copied;
            if (!bytes || (input ? api.write(client,data,bytes,at) : api.read(client,data,bytes,at))!=bytes) return false;
            copied+=bytes;at+=bytes;
            if (at==clientLength) at=0;
        }
        return true;
    }
public:
    explicit HostBounce(API &operations) : api(operations),hardware(api) {}
    HostBounce(const HostBounce &)=delete;
    HostBounce &operator=(const HostBounce &)=delete;
    DMAState currentState() const { return state; }
    bool cleanupRetryable() const {return state==DMAState::CleanupFailed && hardware.cleanupRetryable();}
    void *bytes() const { return hardware.bytes(); }
    uint64_t busAddress() const { return hardware.busAddress(); }
    uint32_t capacity() const { return hardware.capacity(); }
    Result prepareFamilyBuffer(typename API::Memory *prepared,uint32_t requested,bool in,unsigned alignment=64,
        uint64_t offset=0,bool wrap=false) {
        if (live) return Result::Busy;
        const auto bytes=prepared ? api.length(prepared) : 0;
        if (requested>DMARegion<API>::MaximumBytes ||
            ((requested || offset || wrap) && (!prepared || offset>=bytes)) ||
            (!wrap && requested>bytes-offset)) return Result::Invalid;
        client=prepared;length=requested;clientOffset=offset;clientLength=bytes;receive=in;live=true;state=DMAState::Ready;
        finishing=copyBack=copiedBack=false;finishedBytes=0;copied=Result::Success;
        if (client) api.retainMemory(client);
        auto result=requested ? hardware.allocate(requested,alignment) : Result::Success;
        if (result==Result::Success && !receive && requested && !copyClient(false,requested))
            result=Result::HardwareError;
        if (result!=Result::Success) {
            const auto clean=finish(0,false);
            return clean==Result::Success ? result : clean;
        }
        return Result::Success;
    }
    Result publish(uint64_t generation) {
        if (!live || state!=DMAState::Ready || finishing || !generation) return Result::NotReady;
        const auto result=length ? hardware.publish(generation) : Result::Success;
        if (result==Result::Success) { epoch=generation;state=DMAState::Owned; }
        return result;
    }
    Result retire(uint64_t generation) {
        if (state!=DMAState::Owned || epoch!=generation) return Result::NotReady;
        const auto result=length ? hardware.retire(generation) : Result::Success;
        if (result==Result::Success) state=DMAState::Ready;
        return result;
    }
    void quarantine() {
        if (state==DMAState::Owned) { hardware.quarantine();state=DMAState::Quarantined; }
    }
    void retireAfterIsolation() {
        if (state==DMAState::Owned || state==DMAState::Quarantined) {
            hardware.retireAfterIsolation();state=DMAState::Ready;
        }
    }
    Result retireAfterEndpointStop(uint64_t generation) {
        if ((state!=DMAState::Owned && state!=DMAState::Quarantined) || generation!=epoch) return Result::NotReady;
        const auto result=length ? hardware.retireAfterEndpointStop(generation) : Result::Success;
        if (result==Result::Success) state=DMAState::Ready;
        return result;
    }
    Result finish(uint32_t actual,bool validData) {
        if (!live) return Result::NotReady;
        if (state!=DMAState::Ready && state!=DMAState::CleanupFailed) return Result::Busy;
        if (actual>length) return Result::Overflow;
        if (finishing && (finishedBytes!=actual || copyBack!=validData)) return Result::Invalid;
        finishing=true;finishedBytes=actual;copyBack=validData;
        // Clear the hardware DMA command before accessing its CPU allocation.
        // A failed unmap pins both hardware and the original client retain.
        auto result=hardware.unmap();
        if (result!=Result::Success) { state=DMAState::CleanupFailed;return result; }
        if (!copiedBack) {
            if (receive && copyBack && actual && !copyClient(true,actual))
                copied=Result::HardwareError;
            copiedBack=true;
        }
        result=hardware.release();
        if (result!=Result::Success) { state=DMAState::CleanupFailed;return result; }
        if (client) { api.releaseMemory(client);client=nullptr; }
        length=finishedBytes=0;epoch=clientOffset=clientLength=0;live=false;state=DMAState::Empty;
        return copied;
    }
};
}
