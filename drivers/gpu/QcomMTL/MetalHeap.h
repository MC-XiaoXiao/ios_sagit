// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <GPUUserProtocol.h>
#include <algorithm>
#include <vector>

namespace GPU { namespace MetalHeap {
constexpr uint64_t Alignment=64;
inline bool align(uint64_t value,uint64_t alignment,uint64_t &out) {
    if(!alignment)alignment=1;
    if((alignment&(alignment-1)) || value>UINT64_MAX-(alignment-1))return false;
    out=(value+alignment-1)&~(alignment-1);return true;
}
inline bool bufferSize(uint64_t length,uint64_t &size) {
    size=0;return length && length<=MaxBufferBytes && align(length,Alignment,size);
}
struct Range {uint64_t token=0,offset=0,bytes=0;};
// Caller serializes access. Tokens are never recycled, so making an old
// resource aliasable and later destroying it cannot release a new occupant.
class Allocator {
    std::vector<Range> live_;
    uint64_t bytes_=0,serial_=0;
    bool placement_=false;
public:
    bool initialize(uint64_t bytes,bool placement) {
        if(bytes_ || !bytes || bytes>MaxBufferBytes)return false;
        bytes_=bytes;placement_=placement;return true;
    }
    uint64_t size()const{return bytes_;}
    uint64_t used()const {
        uint64_t result=0,end=0;
        for(const auto &r:live_) {uint64_t start=r.offset>end?r.offset:end,next=r.offset+r.bytes;
            if(next>start)result+=next-start;
            if(next>end)end=next;}
        return result;
    }
    uint64_t available(uint64_t alignment)const {
        uint64_t start=0;if(!align(0,alignment,start))return 0;
        if(placement_)return bytes_;
        uint64_t result=0;
        for(const auto &r:live_) {
            if(start<r.offset && r.offset-start>result)result=r.offset-start;
            if(start<r.offset+r.bytes && !align(r.offset+r.bytes,alignment,start))return result;
        }
        if(start<bytes_ && bytes_-start>result)result=bytes_-start;
        return result;
    }
    bool allocate(uint64_t bytes,uint64_t alignment,Range &out,uint64_t offset=0) {
        out={};uint64_t start=0;
        if(!bytes_ || !bytes || serial_==UINT64_MAX || !align(offset,alignment,start) ||
            (placement_ && start!=offset))return false;
        if(!placement_) {
            if(offset)return false;
            for(const auto &r:live_) {
                if(start<=r.offset && bytes<=r.offset-start)break;
                if(start<r.offset+r.bytes && !align(r.offset+r.bytes,alignment,start))return false;
            }
        }
        if(start>bytes_ || bytes>bytes_-start)return false;
        Range range{++serial_,start,bytes};
        auto at=std::upper_bound(live_.begin(),live_.end(),range,[](const Range&a,const Range&b){return a.offset<b.offset;});
        live_.insert(at,range);out=range;return true;
    }
    bool release(uint64_t token) {
        auto at=std::find_if(live_.begin(),live_.end(),[&](const Range &r){return token && r.token==token;});
        if(at==live_.end())return false;
        live_.erase(at);return true;
    }
};
} }
