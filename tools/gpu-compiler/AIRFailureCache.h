// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace GPU { namespace Compiler {
// Process-local only, with the same complete build/library/constants key as
// successful compiled functions. Callers serialize access. Never remember a
// backend, registration, memory, transport or hardware failure here.
class AIRFailureCache {
public:
    using Key=std::array<uint8_t,32>;
    static constexpr size_t Capacity=64,ReasonLimit=4096;
private:
    struct Entry {Key key{};char reason[ReasonLimit+1]{};bool valid=false;};
    std::array<Entry,Capacity> entries{};
    size_t next=0,count=0;
public:
    bool lookup(const Key &key,std::string &reason) const {
        for(const auto &at:entries)if(at.valid && at.key==key){reason=at.reason;return true;}
        return false;
    }
    bool remember(const Key &key,const std::string &entry,const std::string &reason) {
        const auto prefix=entry+": AIR JIT: unsupported ";
        if(entry.empty() || entry.size()>256 || reason.size()>ReasonLimit ||
            reason.size()<=prefix.size() || reason.compare(0,prefix.size(),prefix))return false;
        for(const auto &at:entries)if(at.valid && at.key==key)return reason==at.reason;
        auto &at=entries[next];at.key=key;
        std::memcpy(at.reason,reason.c_str(),reason.size()+1);at.valid=true;
        next=(next+1)%Capacity;if(count<Capacity)++count;
        return true;
    }
    size_t size() const {return count;}
};
} }
