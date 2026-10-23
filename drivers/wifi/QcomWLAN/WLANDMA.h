// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <WLANProtocol.h>

namespace WLAN {
enum class DMAState : uint8_t { Empty, Allocated, DeviceOwned, Quarantined, CleanupFailed };

// Commands and descriptors outlive every CE reference. API is bound to one
// explicit Apple IOMapper; a missing mapper never falls back to physical DMA.
// Allocate/release run on a worker, outside the interrupt/command-gate path.
template<class API> class DMABuffer {
    API &api;
    typename API::Memory *memory = nullptr;
    typename API::Command *command = nullptr;
    void *cpu = nullptr;
    uint64_t address = 0, epoch = 0, lastEpoch = 0;
    uint32_t length = 0;
    bool prepared = false;
    DMAState state = DMAState::Empty;
public:
    explicit DMABuffer(API &operations) : api(operations) {}
    DMABuffer(const DMABuffer &) = delete;
    DMABuffer &operator=(const DMABuffer &) = delete;
    DMAState currentState() const { return state; }
    void *bytes() const { return cpu; }
    uint64_t busAddress() const { return address; }
    uint32_t capacity() const { return length; }
    bool ownsMemory() const { return memory || command; }
    Result allocate(uint32_t count,unsigned alignment) {
        if (state != DMAState::Empty) return Result::Busy;
        if (!count || count > 1024 * 1024 || alignment < 16 || alignment > 65536 ||
            (alignment & (alignment - 1))) return Result::Invalid;
        if (!api.ready()) return Result::NotReady;
        state = DMAState::Allocated;
        memory = api.allocate(count,alignment);
        Result result = memory ? api.prepare(memory) : Result::NotReady;
        prepared = memory && result == Result::Success;
        if (result == Result::Success) {
            cpu = api.bytes(memory);
            if (!cpu || (reinterpret_cast<uintptr_t>(cpu) & (alignment - 1))) result = Result::Invalid;
        }
        if (result == Result::Success) {
            command = api.createCommand(count,alignment);
            result = command ? api.bind(command,memory) : Result::NotReady;
        }
        if (result == Result::Success) {
            uint64_t span = 0;
            result = api.range(command,address,span);
            if (result == Result::Success && (span != count || !validDMA(address,count,alignment))) result = Result::Invalid;
        }
        if (result != Result::Success) {
            const auto cleanup = release();
            return cleanup == Result::Success ? result : cleanup;
        }
        length = count;
        auto *data = static_cast<uint8_t *>(cpu);
        for (uint32_t i = 0; i < count; ++i) data[i] = 0;
        return Result::Success;
    }
    Result publish(uint64_t generation) {
        if (state != DMAState::Allocated || !command || !length) return Result::NotReady;
        if (!generation || generation < lastEpoch) return Result::Invalid;
        api.publishBarrier(); epoch = lastEpoch = generation; state = DMAState::DeviceOwned;
        return Result::Success;
    }
    Result retire(uint64_t generation) {
        if (state != DMAState::DeviceOwned || epoch != generation) return Result::NotReady;
        api.consumeBarrier(); state = DMAState::Allocated; return Result::Success;
    }
    void quarantine() { if (state == DMAState::DeviceOwned) state = DMAState::Quarantined; }
    Result isolate(uint64_t generation) {
        if ((state != DMAState::DeviceOwned && state != DMAState::Quarantined) || epoch != generation) return Result::NotReady;
        const auto result = api.isolated(generation);
        if (result != Result::Success) { state = DMAState::Quarantined; return result; }
        api.consumeBarrier(); state = DMAState::Allocated; return Result::Success;
    }
    Result release() {
        if (state == DMAState::DeviceOwned || state == DMAState::Quarantined) return Result::Busy;
        if (command) {
            const auto result = api.clear(command);
            if (result != Result::Success) { state = DMAState::CleanupFailed; return result; }
            api.releaseCommand(command); command = nullptr; address = 0;
        }
        if (prepared) {
            const auto result = api.complete(memory);
            if (result != Result::Success) { state = DMAState::CleanupFailed; return result; }
            prepared = false;
        }
        if (memory) { api.releaseMemory(memory); memory = nullptr; }
        cpu = nullptr; address = epoch = length = 0; state = DMAState::Empty;
        return Result::Success;
    }
};
}
