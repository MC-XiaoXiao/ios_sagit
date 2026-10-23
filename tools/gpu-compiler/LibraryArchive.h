// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace GPU { namespace Compiler { namespace LibraryArchive {
constexpr size_t MaxBytes=16u*1024*1024;
struct Function {
    std::string name;
    uint32_t type=0; // Apple's MTLB TYPE: vertex0, fragment1, kernel2.
    size_t offset=0,size=0; // Absolute spans in the original container.
    std::array<uint8_t,32> hash{};
};
inline bool container(const void *data,size_t size) {
    if(!data || size<4)return false;
    const auto *b=static_cast<const uint8_t *>(data);
    return !std::memcmp(b,"MTLB",4) ||
        (b[0]==0xca && b[1]==0xfe && b[2]==0xba && (b[3]==0xbe || b[3]==0xbf)) ||
        ((b[0]==0xbe || b[0]==0xbf) && b[1]==0xba && b[2]==0xfe && b[3]==0xca);
}
// Shared by the host AOT reader and Metal's in-process loader. This reads
// layout only: callers must verify each returned AIR SHA256 before use.
// FAT headers/records follow Apple's mach-o/fat.h. The AIR64 MTLB slice in
// the actual22H374 QuartzCore library has cputype0x01000017/subtype11;
// Apple GPU MH_EXECUTE slices are never passed to LLVM or the A540 backend.
inline bool parse(const void *data,size_t size,std::vector<Function> &out,std::string &error) {
    out.clear();error.clear();
    auto fail=[&](const char *message){out.clear();error=message;return false;};
    if(!container(data,size) || size>MaxBytes)return fail("Invalid or oversized Metal library container");
    const auto *b=static_cast<const uint8_t *>(data);
    auto word=[&](size_t at,unsigned n,bool big=false){uint64_t v=0;for(unsigned i=0;i<n;++i)v|=uint64_t(b[at+i])<<(8*(big?n-1-i:i));return v;};
    size_t base=0,span=size;
    if(std::memcmp(b,"MTLB",4)) {
        bool big=b[0]==0xca,wide=(big?b[3]:b[0])==0xbf;
        if(size<8)return fail("Truncated FAT Metal library");
        uint64_t count=word(4,4,big),entry=wide?32:20;
        if(!count || count>64 || count>(size-8)/entry)return fail("Invalid FAT architecture table");
        size_t header=8+size_t(count*entry);bool found=false;
        std::set<std::pair<uint64_t,uint64_t>> architectures;
        std::vector<std::pair<size_t,size_t>> ranges;
        for(size_t i=0;i<count;++i) {
            size_t at=8+i*entry;uint64_t cpu=word(at,4,big),sub=word(at+4,4,big);
            uint64_t offset=word(at+8,wide?8:4,big),length=word(at+(wide?16:12),wide?8:4,big);
            uint64_t alignment=word(at+(wide?24:16),4,big);
            if(!architectures.insert({cpu,sub}).second)return fail("Duplicate FAT architecture");
            if((wide && word(at+28,4,big)) || alignment>(wide?63u:31u) || offset<header || offset>size ||
                length<4 || length>size-offset || (offset&((uint64_t(1)<<alignment)-1)))return fail("Invalid FAT slice range/alignment");
            for(const auto &r:ranges)if(offset<r.second && r.first<offset+length)return fail("Overlapping FAT slices");
            ranges.push_back({size_t(offset),size_t(offset+length)});
            if(cpu==0x01000017) {
                if(found)return fail("Ambiguous AIR64 slices in Metal library");
                if(std::memcmp(b+offset,"MTLB",4))return fail("AIR64 slice is not MTLB");
                base=size_t(offset);span=size_t(length);found=true;
            }
        }
        if(!found)return fail("Metal library has no portable AIR64 slice");
    }
    auto le=[&](size_t at,unsigned n){return word(base+at,n);};
    if(span<88 || le(6,2)!=2 || le(16,8)!=span)return fail("Invalid MTLB v2 header");
    struct Section {size_t start,size;};Section sections[4];
    for(unsigned i=0;i<4;++i) {
        uint64_t start=le(24+i*16,8),length=le(32+i*16,8);
        if(start<88 || start>span || length>span-start)return fail("MTLB section outside container");
        sections[i]={size_t(start),size_t(length)};
        for(unsigned j=0;j<i;++j)if(length && sections[j].size && start<sections[j].start+sections[j].size &&
            sections[j].start<start+length)return fail("Overlapping MTLB sections");
    }
    auto list=sections[0],air=sections[3];
    if(list.size<4 || !air.size || list.size>span-list.start-4)return fail("Invalid MTLB function section");
    size_t end=list.start+list.size+4,pos=list.start+4;
    if(end>sections[1].start)return fail("MTLB function list overlaps metadata");
    uint64_t count=le(list.start,4);
    if(!count || count>4096)return fail("Invalid MTLB function count");
    std::set<std::string> names;std::set<size_t> offsets{air.size};
    for(size_t i=0;i<count;++i) {
        if(end-pos<4)return fail("Truncated MTLB function group");
        uint64_t length=le(pos,4);
        if(length<8 || length>end-pos)return fail("Invalid MTLB function group");
        size_t finish=pos+size_t(length);pos+=4;
        std::map<std::string,std::pair<size_t,size_t>> tags;
        while(finish-pos>=4 && std::memcmp(b+base+pos,"ENDT",4)) {
            if(finish-pos<6)return fail("Truncated MTLB tag");
            std::string tag(reinterpret_cast<const char *>(b+base+pos),4);size_t bytes=size_t(le(pos+4,2));pos+=6;
            if(bytes>finish-pos || tags.count(tag))return fail("Invalid or duplicate MTLB tag");
            tags[tag]={pos,bytes};pos+=bytes;
        }
        if(finish-pos!=4 || std::memcmp(b+base+pos,"ENDT",4))return fail("Missing MTLB ENDT");
        pos=finish;auto n=tags["NAME"],t=tags["TYPE"],o=tags["OFFT"],h=tags["HASH"],l=tags["MDSZ"];
        if(n.second<2 || n.second>4096 || b[base+n.first+n.second-1] || std::memchr(b+base+n.first,0,n.second-1) ||
            t.second!=1 || o.second!=24 || h.second!=32 || (l.second && l.second!=8))return fail("Invalid MTLB function descriptor");
        Function f;f.name.assign(reinterpret_cast<const char *>(b+base+n.first),n.second-1);f.type=b[base+t.first];
        if(!names.insert(f.name).second)return fail("Duplicate MTLB function name");
        uint64_t pub=le(o.first,8),priv=le(o.first+8,8),offset=le(o.first+16,8),bytes=l.second?le(l.first,8):0;
        if(pub>=sections[1].size || priv>=sections[2].size || offset>=air.size || bytes>air.size-offset)
            return fail("MTLB AIR span outside section");
        f.offset=size_t(offset);f.size=size_t(bytes);std::memcpy(f.hash.data(),b+base+h.first,32);
        offsets.insert(f.offset);out.push_back(std::move(f));
    }
    if(pos!=end)return fail("Extra data in MTLB function list");
    for(auto &f:out) {
        if(!f.size)f.size=*offsets.upper_bound(f.offset)-f.offset;
        if(!f.size || f.size>air.size-f.offset)return fail("Invalid MTLB AIR size");
        f.offset+=base+air.start;
    }
    return true;
}
} } }
