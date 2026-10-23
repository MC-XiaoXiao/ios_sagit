// SPDX-License-Identifier: BSD-2-Clause
#include "AdrenoBackend.h"
#include "NativePosterIdentity.h"
#include <cassert>
#include <cstdio>
#include <random>
using GPU::Result;

static void resourceGraph()
{
    GPU::Session s;
    uint64_t c = 0, other = 0, q = 0, b = 0, bytes = 0;
    assert(s.createContext(c) == Result::Success);
    assert(s.createContext(other) == Result::Success);
    assert(s.createQueue(c, q) == Result::Success);
    assert(s.createBuffer(c, 4097, 0, b, bytes) == Result::Success && bytes == 8192);
    assert(s.destroyContext(c) == Result::Busy);
    assert(s.destroyBuffer(q) == Result::Invalid);
    assert(s.validateCopy(b, 8191, 1) == Result::Success);
    assert(s.validateCopy(b, 8191, 2) == Result::Invalid);
    assert(s.validateCopy(b, UINT64_MAX, 1) == Result::Invalid);
    assert(s.validateCopy(b, 0, 0) == Result::Invalid);
    assert(s.validateCopy(b, 0, GPU::MaxCopyBytes + 1) == Result::Invalid);
    assert(s.validateSubmit(q, b, 0, 8192) == Result::Success);
    assert(s.validateSubmit(q, b, 1, 4) == Result::Invalid);
    assert(s.validateSubmit(q, b, 0, 3) == Result::Invalid);
    assert(s.validateSubmit(q, b, 8192, 4) == Result::Invalid);
    assert(s.validateSubmit(q, b, UINT64_MAX - 3, 8) == Result::Invalid);
    uint64_t otherBuffer = 0;
    assert(s.createBuffer(other, 4096, 0, otherBuffer, bytes) == Result::Success);
    assert(s.validateSubmit(q, otherBuffer, 0, 4) == Result::Invalid);
    assert(s.createBuffer(c, UINT64_MAX, 0, otherBuffer, bytes) == Result::Invalid);
    assert(s.createBuffer(c, 1, 1, otherBuffer, bytes) == Result::Invalid);
    uint64_t stale = b;
    assert(s.destroyBuffer(b) == Result::Success);
    assert(s.createBuffer(c, 1, 0, b, bytes) == Result::Success && b != stale);
    assert(s.destroyBuffer(stale) == Result::Invalid);
    assert(s.validateCopy(b | (uint64_t(1) << 32), 0, 1) == Result::Invalid);
    s.clear();
    assert(!s.allocatedBytes && s.destroyContext(c) == Result::Invalid);
    assert(s.createContext(other) == Result::Success && other != c);
    assert(s.createQueue(c, q) == Result::Invalid);
}

static void limits()
{
    GPU::Session s;
    uint64_t contexts[GPU::MaxContexts] = {}, extra = 0, bytes = 0;
    for (auto &c : contexts) assert(s.createContext(c) == Result::Success);
    assert(s.createContext(extra) == Result::NoResources);
    for (unsigned i = 0; i < GPU::MaxQueues; ++i) assert(s.createQueue(contexts[0], extra) == Result::Success);
    assert(s.createQueue(contexts[0], extra) == Result::NoResources);
    uint64_t buffers[4] = {};
    for (auto &b : buffers) assert(s.createBuffer(contexts[0], GPU::MaxBufferBytes, 0, b, bytes) == Result::Success);
    assert(s.allocatedBytes == GPU::MaxSessionBytes);
    assert(s.createBuffer(contexts[1], 1, 0, extra, bytes) == Result::NoResources);
    assert(s.destroyBuffer(buffers[0]) == Result::Success);
    assert(s.createBuffer(contexts[1], 1, 0, extra, bytes) == Result::Success);
    s.clear(); assert(!s.allocatedBytes);
    assert(s.createContext(extra) == Result::Success);
    for (unsigned i = 0; i < GPU::MaxBuffers; ++i) {
        uint64_t b = 0; assert(s.createBuffer(extra, 1, 0, b, bytes) == Result::Success);
    }
    assert(s.createBuffer(extra, 1, 0, contexts[0], bytes) == Result::NoResources);
    // Exercise the actual generation boundary with a one-slot namespace.
    GPU::Table<1, GPU::Kind::Queue> t;
    for (uint32_t i = 0; i < decltype(t)::MaxGeneration; ++i) {
        auto *e = t.create(); assert(e && (e->handle >> 30) == 2); *e = {};
    }
    assert(!t.create()); t.clear(); assert(!t.create());
}

static void abi()
{
    for (uint32_t selector = 0; selector < GPU::SelectorCount; ++selector) {
        GPU::Shape shape{}; assert(GPU::shape(selector, shape));
        size_t in = selector == GPU::WriteBuffer ? 1 : 0, out = selector == GPU::ReadBuffer ? 1 : 0;
        assert(GPU::validCall(selector, shape.inputs, shape.outputs, in, out));
        assert(!GPU::validCall(selector, shape.inputs + 1, shape.outputs, in, out));
        assert(!GPU::validCall(selector, shape.inputs, shape.outputs + 1, in, out));
    }
    assert(!GPU::validCall(GPU::SelectorCount, 0, 0, 0, 0));
    assert(!GPU::validCall(GPU::GetInfo, 0, 6, 1, 0));
    assert(!GPU::validCall(GPU::WriteBuffer, 2, 0, 0, 0));
    assert(!GPU::validCall(GPU::ReadBuffer, 2, 0, 0, GPU::MaxCopyBytes + 1));
    uint64_t bytes = 0;
    assert(!GPU::allocationSize(0, bytes) && !GPU::allocationSize(UINT64_MAX, bytes));
    assert(GPU::allocationSize(1, bytes) && bytes == GPU::PageBytes);
}

int main()
{
    using namespace GPU::NativePoster;
    assert(allowed(true,true,MercuryHash));
    assert(!allowed(false,true,MercuryHash));
    assert(!allowed(true,false,MercuryHash));
    assert(!allowed(true,true,nullptr));
    uint8_t other[20];
    for(unsigned i=0;i<20;++i)other[i]=MercuryHash[i];
    for(unsigned i=0;i<20;++i){other[i]^=1;assert(!allowed(true,true,other));other[i]^=1;}
    resourceGraph(); limits(); abi();
    puts("GPU: resource isolation/lifetime, quotas, ABI, resource graph and protocol v2 passed");
}
