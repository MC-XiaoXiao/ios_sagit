// SPDX-License-Identifier: BSD-2-Clause
#include "AIRFailureCache.h"
#include <cassert>
#include <cstdio>
int main() {
    using Cache=GPU::Compiler::AIRFailureCache;
    Cache cache;Cache::Key key{};std::string reason;
    const std::string entry="VTMTSComputeFunction",unsupported=entry+": AIR JIT: unsupported texture metadata";
    assert(!cache.lookup(key,reason));
    for(const auto &transient:{"out of memory","backend failed","device registration failed","AIR JIT: unsupported texture metadata"})
        assert(!cache.remember(key,entry,transient));
    assert(!cache.remember(key,"OtherFunction",unsupported));
    assert(!cache.remember(key,entry,unsupported+std::string(Cache::ReasonLimit,'x')));
    assert(cache.size()==0);
    assert(cache.remember(key,entry,unsupported));
    for(unsigned i=0;i<10;++i)assert(cache.lookup(key,reason) && reason==unsupported && cache.remember(key,entry,unsupported));
    assert(cache.size()==1);
    auto other=key;other[31]=1;assert(!cache.lookup(other,reason));
    for(unsigned i=1;i<=Cache::Capacity;++i){other=key;other[0]=uint8_t(i);assert(cache.remember(other,entry,unsupported));}
    assert(cache.size()==Cache::Capacity && !cache.lookup(key,reason));
    assert(cache.lookup(other,reason) && reason==unsupported);
    std::puts("AIR failure cache: exact variant keys, deterministic front-end failures only, unchanged diagnostics, bounded FIFO eviction passed");
}
