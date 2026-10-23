// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>
namespace Display {
struct PreparedSwapKey {
    uintptr_t request=0,surface=0,fence=0;
    uint32_t token=0;
    bool valid() const {return request && surface && fence;}
    bool operator==(const PreparedSwapKey &o) const {
        return request==o.request && surface==o.surface && fence==o.fence && token==o.token;
    }
};
// One owned, UNEXPOSED mapping under one retained native read lease. Retaining
// the lease prevents pointer ABA even when Family recycles a request/token.
// All calls belong to the display gate; no producer readiness is fabricated.
template<class Mapping,class Lease> class PreparedSwapSlot {
    PreparedSwapKey key{};
    Mapping *mapping=nullptr;
    Lease *lease=nullptr;
public:
    bool empty() const {return !mapping;}
    bool matches(const PreparedSwapKey &candidate,bool ready,bool completed) const {
        return mapping && candidate.valid() && candidate==key && ready && !completed;
    }
    bool adopt(const PreparedSwapKey &candidate,Mapping *owned,Lease *read) {
        if(mapping || !owned || !read || !candidate.valid() || candidate.fence!=reinterpret_cast<uintptr_t>(read))return false;
        read->retain();key=candidate;mapping=owned;lease=read;return true;
    }
    Mapping *take(const PreparedSwapKey &candidate,bool ready,bool completed) {
        if(!matches(candidate,ready,completed))return nullptr;
        auto *result=mapping;auto *read=lease;
        mapping=nullptr;lease=nullptr;key={};read->release();return result;
    }
    template<class Retire> bool discard(const Retire &retire) {
        auto *old=mapping;auto *read=lease;
        // Reset before external cleanup, which can reenter native transport.
        mapping=nullptr;lease=nullptr;key={};
        const bool ok=!old || retire(old);
        if(read)read->release();
        return ok;
    }
};
}
