// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "GPUUserProtocol.h"

namespace GPU { namespace Native { namespace Memory {
constexpr uint32_t Version=1,ResourcesPerClient=1024,ResourcesPerJob=64;
constexpr uint32_t GlobalResources=2048,MappingSlots=4096,TablePages=512;
constexpr uint64_t ClientBytes=256ULL*1024*1024,GlobalBytes=384ULL*1024*1024;
struct Limits {
    uint32_t version=Version,bytes=64,resourcesPerClient=ResourcesPerClient,resourcesPerJob=ResourcesPerJob;
    uint64_t resourceBytes=MaxBufferBytes,clientBytes=0,globalBytes=0,apertureBytes=0;
    uint32_t globalResources=GlobalResources,mappingSlots=MappingSlots,tablePages=TablePages,reserved=0;
};
inline Limits limitsForAperture(uint64_t aperture) {
    Limits result;result.apertureBytes=aperture;
    if(aperture<8*1024*1024 || aperture>512*1024*1024 || (aperture&4095))return result;
    result.clientBytes=(aperture/2<ClientBytes?aperture/2:ClientBytes)&~uint64_t(16383);
    result.globalBytes=(aperture/4*3<GlobalBytes?aperture/4*3:GlobalBytes)&~uint64_t(16383);
    result.resourceBytes=result.clientBytes<MaxBufferBytes?result.clientBytes:MaxBufferBytes;
    return result;
}
inline bool validLimits(const Limits &v) {
    const auto expected=limitsForAperture(v.apertureBytes);
    return expected.clientBytes && v.version==Version && v.bytes==sizeof(Limits) && !v.reserved &&
        v.resourcesPerClient==expected.resourcesPerClient && v.resourcesPerJob==expected.resourcesPerJob &&
        v.resourceBytes==expected.resourceBytes && v.clientBytes==expected.clientBytes && v.globalBytes==expected.globalBytes &&
        v.globalResources==expected.globalResources && v.mappingSlots==expected.mappingSlots && v.tablePages==expected.tablePages;
}
inline uint64_t writeBit(unsigned index) {return index<ResourcesPerJob?uint64_t(1)<<index:0;}
struct Usage {
    uint64_t clients=0,resources=0,bytes=0;
    bool addClient(){if(clients>=MaxClients)return false;++clients;return true;}
    bool removeClient(){if(!clients)return false;--clients;return true;}
    bool add(uint64_t amount,const Limits &limits) {
        if(!amount || amount>limits.resourceBytes || resources>=limits.globalResources || bytes>limits.globalBytes ||
            amount>limits.globalBytes-bytes)return false;
        ++resources;bytes+=amount;return true;
    }
    bool remove(uint64_t amount) {
        if(!amount || !resources || amount>bytes)return false;
        --resources;bytes-=amount;return true;
    }
};
template<unsigned Capacity=ResourcesPerClient> class ResourceTable {
    struct Entry {uint32_t id=0;uint64_t bytes=0;} entries_[Capacity]{};
    uint32_t count_=0;
    uint64_t bytes_=0;
public:
    uint32_t count() const{return count_;}
    uint64_t bytes() const{return bytes_;}
    bool canAdd(uint64_t amount,uint64_t limit) const {
        return amount && count_<Capacity && bytes_<=limit && amount<=limit-bytes_;
    }
    bool add(uint32_t id,uint64_t amount,uint64_t limit) {
        if(!id || !canAdd(amount,limit))return false;
        Entry *free=nullptr;
        for(auto &entry:entries_) {if(entry.id==id)return false;if(!entry.id && !free)free=&entry;}
        if(!free)return false;
        *free={id,amount};++count_;bytes_+=amount;return true;
    }
    bool remove(uint32_t id) {
        if(!id)return false;
        for(auto &entry:entries_)if(entry.id==id) {bytes_-=entry.bytes;--count_;entry={};return true;}
        return false;
    }
};
struct MappingUsage {uint64_t count=0,bytes=0,spanBytes=0,tablePages=0;};
struct Snapshot {
    uint32_t version=Version,bytes=136,flags=0,reserved=0;
    Limits limits{};Usage native{};MappingUsage mappings{};
};
static_assert(sizeof(Limits)==64 && sizeof(Usage)==24 && sizeof(Snapshot)==136,"native memory registry data ABI");
static_assert(ResourcesPerJob==64,"surface write mask width");
} } }
