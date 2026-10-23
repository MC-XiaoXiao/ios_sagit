// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "USBProtocol.h"
#include "DMACommandState.h"

namespace USB {
enum class DMAState : uint8_t { Empty, Ready, Owned, Quarantined, CleanupFailed };

// API supplies the memory descriptor and DMA command operations. The kernel
// binding is IOMemoryAPI; the same lifetime code is exercised with injected
// allocation, mapping and completion failures in the offline test. Allocation,
// unmapping, copying and release may sleep: use a preparation/completion worker,
// never a controller interrupt action or a command-gate action.
// CompleteFailurePreservesPreparation is true only when an API explicitly
// guarantees that a failed MD complete consumed no part of its preparation.
template<class API> class DMARegion {
    API &api;
    typename API::Memory *memory = nullptr;
    typename API::Command *command = nullptr;
    void *cpu = nullptr;
    uint64_t address = 0, generation = 0;
    uint32_t length = 0;
    MemoryCompletionState preparation;
    DMAState state = DMAState::Empty;
public:
    static constexpr uint32_t MaximumBytes = 1024 * 1024;
    explicit DMARegion(API &operations) : api(operations) {}
    DMARegion(const DMARegion &) = delete;
    DMARegion &operator=(const DMARegion &) = delete;
    DMAState currentState() const { return state; }
    void *bytes() const { return cpu; }
    uint64_t busAddress() const { return address; }
    uint32_t capacity() const { return length; }
    typename API::Command *dmaCommand() const { return command; }
    bool cleanupRetryable() const {return state==DMAState::CleanupFailed && !preparation.faulted();}

    Result allocate(uint32_t bytes, unsigned alignment) {
        if (state != DMAState::Empty) return Result::Busy;
        if (!bytes || bytes > MaximumBytes || alignment < 16 || alignment > 65536 ||
            (alignment & (alignment - 1))) return Result::Invalid;
        // Barriers cannot provide coherency. The API must supply hardware-
        // coherent or uncached storage. A missing mapper also requires a live
        // platform lease proving the USB stream is in bypass.
        if (!api.coherent() || (!api.mapped() && !api.bypass())) return Result::NotReady;
        const unsigned bits = api.addressBits();
        if (bits < 32 || bits > 40) return Result::Invalid;
        state = DMAState::Ready;
        memory = api.allocate(bytes, alignment);
        Result result = memory ? Result::Success : Result::NotReady;
        if (result == Result::Success) {
            result = api.prepare(memory, true, true);
            preparation.prepared(result);
        }
        if (result == Result::Success) {
            cpu = api.bytes(memory);
            if (!cpu || (reinterpret_cast<uintptr_t>(cpu) & (alignment - 1))) result = Result::Invalid;
        }
        if (result == Result::Success) {
            command = api.createCommand(bytes, alignment);
            result = command ? api.bind(command, memory) : Result::NotReady;
        }
        if (result == Result::Success) {
            uint64_t mappedBytes = 0;
            result = api.range(command, address, mappedBytes);
            if (result == Result::Success && (mappedBytes != bytes ||
                !dmaRange(address, bytes, bits, alignment))) result = Result::Invalid;
        }
        if (result != Result::Success) {
            // A failed clear/complete retains the remaining objects. The
            // caller must retry release and keep this owner alive meanwhile.
            const auto cleanup = release();
            return cleanup == Result::Success ? result : cleanup;
        }
        length = bytes;
        auto *p = static_cast<uint8_t *>(cpu);
        for (unsigned i = 0; i < bytes; ++i) p[i] = 0;
        return Result::Success;
    }
    Result publish(uint64_t epoch) {
        if (state != DMAState::Ready || !command || !length || !epoch) return Result::NotReady;
        generation = epoch; state = DMAState::Owned;
        return Result::Success;
    }
    // Called only after the matching TD completion or completed END command
    // proves DMA has stopped. A command timeout cannot use this operation.
    Result retire(uint64_t epoch) {
        if (state != DMAState::Owned || epoch != generation) return Result::NotReady;
        state = DMAState::Ready; return Result::Success;
    }
    // For a quarantined transfer only: the owner has inspected a fresh
    // endpoint stop/dequeue completion covering every access to this buffer.
    Result retireAfterEndpointStop(uint64_t epoch) {
        if ((state!=DMAState::Owned && state!=DMAState::Quarantined) || epoch!=generation) return Result::NotReady;
        state=DMAState::Ready;return Result::Success;
    }
    void quarantine() { if (state == DMAState::Owned) state = DMAState::Quarantined; }
    // The caller must have a whole-controller halt/reset + isolation proof.
    // Old events must be discarded before these buffers are published again.
    void retireAfterIsolation() {
        if (state == DMAState::Owned || state == DMAState::Quarantined) state = DMAState::Ready;
    }
    Result unmap() {
        if (state != DMAState::Ready && state != DMAState::CleanupFailed && state != DMAState::Empty)
            return Result::Busy;
        if (command) {
            const auto result = api.clear(command);
            if (result != Result::Success) { state = DMAState::CleanupFailed; return result; }
            api.releaseCommand(command); command = nullptr; address = 0;
        }
        if (state == DMAState::CleanupFailed) state = DMAState::Ready;
        return Result::Success;
    }
    Result release() {
        auto result = unmap();
        if (result != Result::Success) return result;
        result=preparation.complete([&] {return api.complete(memory);},API::CompleteFailurePreservesPreparation);
        if (result!=Result::Success) {state=DMAState::CleanupFailed;return result;}
        if (memory) { api.releaseMemory(memory); memory = nullptr; }
        cpu = nullptr; address = generation = length = 0; state = DMAState::Empty;
        return Result::Success;
    }
};

// A bounded initial request path. Dedicated bounce storage keeps hardware's
// rounded OUT tail packet out of adjacent client memory; IN copies prevent
// changing client mappings after a TD is handed to the controller. The Family
// retains its own buffer; this helper takes a separate retain and prepare pair.
template<class API> class DeviceBounce {
    API &api;
    DMARegion<API> region;
    typename API::Memory *client = nullptr;
    uint32_t requested = 0;
    uint64_t clientOffset = 0;
    uint64_t generation = 0;
    MemoryCompletionState preparation;
    bool deviceWrites = false, live = false, copiedBack = false;
    Result copyResult = Result::Success;
    DMAState state = DMAState::Empty;
public:
    explicit DeviceBounce(API &operations) : api(operations), region(operations) {}
    DeviceBounce(const DeviceBounce &) = delete;
    DeviceBounce &operator=(const DeviceBounce &) = delete;
    DMAState currentState() const { return state; }
    void *bytes() const { return region.bytes(); }
    uint64_t busAddress() const { return region.busAddress(); }
    uint32_t capacity() const { return region.capacity(); }
    typename API::Command *dmaCommand() const { return region.dmaCommand(); }
    bool cleanupRetryable() const {
        return state==DMAState::CleanupFailed && !preparation.faulted() &&
            (region.currentState()==DMAState::Empty || region.cleanupRetryable());
    }

    Result prepare(typename API::Memory *buffer, uint32_t length, bool receive, unsigned packetSize,uint64_t offset=0) {
        if (live) return Result::Busy;
        // Interrupt endpoints may use any maximum packet size in their
        // speed's allowed range. EndpointConfiguration checks that range;
        // rounding bounce storage does not require a power of two.
        if (!packetSize || packetSize > 1024) return Result::Invalid;
        if (length > DMARegion<API>::MaximumBytes || ((length || offset) && !buffer) ||
            (buffer && (offset>api.length(buffer) || length>api.length(buffer)-offset))) return Result::Invalid;
        const uint64_t capacity = receive ?
            (length ? (uint64_t(length) + packetSize - 1) / packetSize * packetSize : packetSize) : length;
        if (capacity > DMARegion<API>::MaximumBytes) return Result::Invalid;
        live = true; copiedBack = false; copyResult = Result::Success; state = DMAState::Ready;
        client = buffer; requested = length; deviceWrites = receive;clientOffset=offset;
        Result result = Result::Success;
        if (client) {
            api.retainMemory(client);
            // Native Mux uses a non-null, zero-length MD for terminating
            // ZLPs. No client bytes are accessed for that request, and the
            // pinned Apple controller also skips its DMA preparation. Keep
            // the retain until completion without wiring an empty range.
            if (length) {
                result = api.prepare(client, receive, !receive);
                preparation.prepared(result);
            }
        }
        if (result == Result::Success && capacity) result = region.allocate(uint32_t(capacity), 64);
        if (result == Result::Success && !receive && length &&
            api.read(client, region.bytes(), length,clientOffset) != length) result = Result::HardwareError;
        if (result != Result::Success) {
            const auto cleanup = finish(0, false);
            return cleanup == Result::Success ? result : cleanup;
        }
        return Result::Success;
    }
    Result publish(uint64_t epoch) {
        if (!live || state != DMAState::Ready || !epoch) return Result::NotReady;
        auto result = region.capacity() ? region.publish(epoch) : Result::Success;
        if (result == Result::Success) { generation = epoch; state = DMAState::Owned; }
        return result;
    }
    Result retire(uint64_t epoch) {
        if (state != DMAState::Owned || epoch != generation) return Result::NotReady;
        auto result = region.capacity() ? region.retire(epoch) : Result::Success;
        if (result == Result::Success) state = DMAState::Ready;
        return result;
    }
    void quarantine() {
        if (state == DMAState::Owned) { region.quarantine(); state = DMAState::Quarantined; }
    }
    void retireAfterIsolation() {
        if (state == DMAState::Owned || state == DMAState::Quarantined) {
            region.retireAfterIsolation(); state = DMAState::Ready;
        }
    }
    // copyBack is true only for data the controller reported as valid. Aborts
    // and SETUP replacement use false. No Apple callback runs in this helper.
    Result finish(uint32_t actual, bool copyBack) {
        if (!live) return Result::NotReady;
        if (state != DMAState::Ready && state != DMAState::CleanupFailed) return Result::Busy;
        if (actual > requested) return Result::Overflow;
        auto result = region.unmap(); // completes any IODMACommand bounce first
        if (result != Result::Success) { state = DMAState::CleanupFailed; return result; }
        if (!copiedBack) {
            if (copyBack && deviceWrites && actual &&
                api.write(client, region.bytes(), actual,clientOffset) != actual) copyResult = Result::HardwareError;
            copiedBack = true;
        }
        result = region.release();
        if (result != Result::Success) { state = DMAState::CleanupFailed; return result; }
        result=preparation.complete([&] {return api.complete(client);},API::CompleteFailurePreservesPreparation);
        if (result!=Result::Success) {state=DMAState::CleanupFailed;return result;}
        if (client) { api.releaseMemory(client); client = nullptr; }
        requested = 0;clientOffset=0; generation = 0; live = false; state = DMAState::Empty;
        return copyResult;
    }
};
}
