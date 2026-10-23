// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <GPUNativeProtocol.h>
namespace GPU { namespace MetalUpload {
constexpr unsigned PageBytes=4096,MaxCachedPages=64;
constexpr uint64_t MaxCachedBytes=1024*1024;
// Device-owned native IDs avoid a device -> ObjC buffer -> device retain cycle.
// Caller serializes with transport and supplies ONLY privately owned upload
// roots after physical completion AND the last view/reference has gone away.
template<unsigned Capacity=MaxCachedPages,uint64_t ByteLimit=MaxCachedBytes> class Pool {
    Native::ResourceData entries_[Capacity]{};
    unsigned count_=0;
    uint64_t bytes_=0,hits_=0,misses_=0,returns_=0;
public:
    unsigned count() const{return count_;}
    uint64_t bytes() const{return bytes_;}
    uint64_t hits() const{return hits_;}
    uint64_t misses() const{return misses_;}
    uint64_t returns() const{return returns_;}
    Native::ResourceData take() {
        if(!count_){++misses_;return {};}
        auto value=entries_[--count_];entries_[count_]={};bytes_-=value.bytes;++hits_;return value;
    }
    bool put(Native::ResourceData &value) {
        if(!value.id || !value.cpuAddress || !value.gpuAddress || value.bytes<PageBytes ||
            count_==Capacity || value.bytes>ByteLimit-bytes_)return false;
        for(unsigned i=0;i<count_;++i)if(entries_[i].id==value.id)return false;
        entries_[count_++]=value;bytes_+=value.bytes;value={};++returns_;return true;
    }
};
} }
