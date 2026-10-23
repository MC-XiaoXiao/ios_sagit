// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <GPUUserProtocol.h>

namespace GPU {
enum class Result { Success, Invalid, NoResources, Busy, Unsupported, Offline, Timeout, Fault };
enum class Kind : uint32_t { Context = 1, Queue = 2, Buffer = 3 };
struct Resource {
    uint32_t handle = 0, parent = 0;
    uint64_t bytes = 0;
    uint64_t issued = 0, completed = 0;
    uint32_t pending = 0;
    bool faulted = false;
};
// Connection-local handles include type and a nonwrapping generation. No user
// pointer, CPU physical address or GPU VA crosses this resource namespace.
template <unsigned N, Kind K> class Table {
    static_assert(N && N <= 256, "resource slot must fit in the handle");
    Resource entries[N] = {};
    uint32_t generation = 0;
public:
    static constexpr uint32_t MaxGeneration = 0x003fffff;
    Resource *find(uint64_t handle) {
        if (!handle || handle > UINT32_MAX) return nullptr;
        unsigned slot = static_cast<uint32_t>(handle) & 0xff;
        return slot < N && entries[slot].handle == handle ? &entries[slot] : nullptr;
    }
    Resource *at(unsigned i) { return i < N ? &entries[i] : nullptr; }
    Resource *create(uint32_t parent = 0, uint64_t bytes = 0) {
        if (generation == MaxGeneration) return nullptr;
        for (unsigned i = 0; i < N; ++i) if (!entries[i].handle) {
            entries[i] = {(static_cast<uint32_t>(K) << 30) | (++generation << 8) | i, parent, bytes};
            return &entries[i];
        }
        return nullptr;
    }
    bool hasParent(uint32_t parent) const {
        for (const auto &e : entries) if (e.handle && e.parent == parent) return true;
        return false;
    }
    void clear() { for (auto &e : entries) e = {}; } // generation never resets
};

class Session {
public:
    Table<MaxContexts, Kind::Context> contexts;
    Table<MaxQueues, Kind::Queue> queues;
    Table<MaxBuffers, Kind::Buffer> buffers;
    uint64_t allocatedBytes = 0;
    Result createContext(uint64_t &handle) {
        auto *e = contexts.create();
        if (!e) return Result::NoResources;
        handle = e->handle; return Result::Success;
    }
    Result destroyContext(uint64_t handle) {
        auto *e = contexts.find(handle);
        if (!e) return Result::Invalid;
        if (queues.hasParent(e->handle) || buffers.hasParent(e->handle)) return Result::Busy;
        *e = {}; return Result::Success;
    }
    Result createQueue(uint64_t context, uint64_t &handle) {
        auto *parent = contexts.find(context);
        if (!parent) return Result::Invalid;
        auto *e = queues.create(parent->handle);
        if (!e) return Result::NoResources;
        handle = e->handle; return Result::Success;
    }
    Result destroyQueue(uint64_t handle) {
        auto *e = queues.find(handle);
        if (!e) return Result::Invalid;
        if (e->pending) return Result::Busy;
        *e = {}; return Result::Success;
    }
    Result queryFence(uint64_t queue, uint64_t fence, bool &done) {
        auto *q = queues.find(queue);
        if (!q || !fence || fence > q->issued) return Result::Invalid;
        if (fence <= q->completed) { done = true; return Result::Success; }
        if (q->faulted) return Result::Fault;
        done = false; return Result::Success;
    }
    Result createBuffer(uint64_t context, uint64_t bytes, uint64_t flags, uint64_t &handle, uint64_t &rounded) {
        auto *parent = contexts.find(context);
        if (!parent || flags || !allocationSize(bytes, rounded)) return Result::Invalid;
        if (rounded > MaxSessionBytes - allocatedBytes) return Result::NoResources;
        auto *e = buffers.create(parent->handle, rounded);
        if (!e) return Result::NoResources;
        allocatedBytes += rounded; handle = e->handle; return Result::Success;
    }
    Result destroyBuffer(uint64_t handle) {
        auto *e = buffers.find(handle);
        if (!e) return Result::Invalid;
        allocatedBytes -= e->bytes; *e = {}; return Result::Success;
    }
    Result validateCopy(uint64_t buffer, uint64_t offset, uint64_t bytes) {
        auto *e = buffers.find(buffer);
        return e && bytes <= MaxCopyBytes && range(e->bytes, offset, bytes) ? Result::Success : Result::Invalid;
    }
    Result validateSubmit(uint64_t queue, uint64_t buffer, uint64_t offset, uint64_t bytes) {
        auto *q = queues.find(queue); auto *b = buffers.find(buffer);
        return q && b && q->parent == b->parent && !(offset & 3) && !(bytes & 3) &&
            range(b->bytes, offset, bytes) ? Result::Success : Result::Invalid;
    }
    void clear() { queues.clear(); buffers.clear(); contexts.clear(); allocatedBytes = 0; }
};
}
