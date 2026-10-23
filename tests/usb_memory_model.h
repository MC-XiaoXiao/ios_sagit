// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "USBDMA.h"
#include "DMACommandState.h"
using USB::Result;
using USB::DMAState;

struct MemoryAPI {
    static constexpr bool CompleteFailurePreservesPreparation=true;
    struct Memory { void *data; unsigned size, references = 1, prepares = 0; };
    struct Command { Memory *memory = nullptr; USB::DMACommandState completion; };
    bool coherentFlag = true, mapperFlag = true, bypassFlag = false;
    unsigned bits = 36, allocationCalls = 0, releases = 0, copies = 0;
    int fail = 0, completeFailures = 0, clearFailures = 0;
    Memory *failComplete = nullptr;
    uint64_t bus = 0x100000;
    std::vector<Memory *> memories;
    std::vector<Command *> commands;
    ~MemoryAPI() { assert(memories.empty() && commands.empty()); }
    bool coherent() const { return coherentFlag; }
    bool mapped() const { return mapperFlag; }
    bool bypass() const { return bypassFlag; }
    unsigned addressBits() const { return bits; }
    Memory *allocate(uint32_t n, unsigned align) {
        ++allocationCalls;
        if (fail == 1) return nullptr;
        auto *m = new Memory{std::aligned_alloc(align, (n + align - 1) / align * align), n};
        assert(m->data); memories.push_back(m); return m;
    }
    void retainMemory(Memory *m) { ++m->references; }
    void releaseMemory(Memory *m) {
        assert(m && m->references);
        if (--m->references) return;
        assert(!m->prepares);
        for (auto i = memories.begin(); i != memories.end(); ++i) if (*i == m) {
            memories.erase(i); std::free(m->data); delete m; ++releases; return;
        }
        assert(false);
    }
    uint64_t length(Memory *m) { return m->size; }
    Result prepare(Memory *m, bool, bool) {
        if (fail == 2) return Result::NotReady;
        ++m->prepares; return Result::Success;
    }
    Result complete(Memory *m) {
        assert(m && m->prepares);
        if (failComplete == m) { failComplete = nullptr; return Result::HardwareError; }
        if (completeFailures) { --completeFailures; return Result::HardwareError; }
        --m->prepares; return Result::Success;
    }
    void *bytes(Memory *m) { return fail == 3 ? nullptr : m->data; }
    uint64_t read(Memory *m, void *p, unsigned n, uint64_t offset=0) {
        if (fail == 7) return 0;
        assert(m->prepares && offset<=m->size && n<=m->size-offset);
        std::memcpy(p,static_cast<uint8_t *>(m->data)+offset,n);return n;
    }
    uint64_t write(Memory *m, const void *p, unsigned n, uint64_t offset=0) {
        ++copies;
        if (fail == 8) return 0;
        assert(m->prepares && p && offset<=m->size && n<=m->size-offset);
        std::memcpy(static_cast<uint8_t *>(m->data)+offset,p,n);return n;
    }
    Command *createCommand(uint32_t, unsigned) {
        if (fail == 4) return nullptr;
        auto *c = new Command; commands.push_back(c); return c;
    }
    Result bind(Command *c, Memory *m) {
        if (fail == 5) return Result::HardwareError;
        c->memory = m; c->completion.prepared(Result::Success,mapperFlag); return Result::Success;
    }
    Result range(Command *c, uint64_t &address, uint64_t &size) {
        address = bus; size = c->memory->size;
        if (fail == 6) --size;
        return Result::Success;
    }
    Result clear(Command *c) {
        return c->completion.clear([]{return Result::Success;},[&] {
            if (clearFailures) { --clearFailures; return Result::HardwareError; }
            return Result::Success;
        });
    }
    void releaseCommand(Command *c) {
        for (auto i = commands.begin(); i != commands.end(); ++i) if (*i == c) {
            commands.erase(i); delete c; return;
        }
        assert(false);
    }
};

