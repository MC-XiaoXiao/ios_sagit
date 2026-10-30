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
// A library is an index of lazily compiled modules, not one GPU allocation.
// Stock 22H374 MPSCore is 33 MiB with 4,396 functions. Keep separate bounds
// from the 16 MiB per-AIR input and 256 resident JIT programs.
constexpr size_t MaxBytes=64u*1024*1024, MaxFunctions=8192;
struct Function {
    std::string name;
    bool specialized=false; // ALIS names an archived specialization of name.
    uint32_t type=0; // Apple's MTLB TYPE: vertex0, fragment1, kernel2.
    size_t offset=0,size=0; // Absolute spans in the original container.
    std::array<uint8_t,32> hash{};
};
inline bool container(const void *data,size_t size) {
    if(!data || size<4)return false;
    const auto *b=static_cast<const uint8_t *>(data);
    return !std::memcmp(b,"MTLB",4) ||
        (b[0]==0xcf && b[1]==0xfa && b[2]==0xed && b[3]==0xfe) ||
        (b[0]==0xca && b[1]==0xfe && b[2]==0xba && (b[3]==0xbe || b[3]==0xbf)) ||
        ((b[0]==0xbe || b[0]==0xbf) && b[1]==0xba && b[2]==0xfe && b[3]==0xca);
}
// Shared by the host AOT reader and Metal's in-process loader. This reads
// layout only: callers must verify each returned AIR SHA256 before use.
// FAT headers/records follow Apple's mach-o/fat.h. The AIR64 MTLB slice in
// the actual22H374 QuartzCore library has cputype0x01000017/subtype11;
// Apple GPU MH_EXECUTE code is never passed to LLVM or the A540 backend.
// Some archives also retain source AIR in aligned __TEXT,__metallib members.
inline bool parse(const void *data,size_t size,std::vector<Function> &out,std::string &error) {
    out.clear();error.clear();
    auto fail=[&](const char *message){out.clear();error=message;return false;};
    if(!container(data,size) || size>MaxBytes)return fail("Invalid or oversized Metal library container");
    const auto *b=static_cast<const uint8_t *>(data);
    auto word=[&](size_t at,unsigned n,bool big=false){uint64_t v=0;for(unsigned i=0;i<n;++i)v|=uint64_t(b[at+i])<<(8*(big?n-1-i:i));return v;};
    if(b[0]==0xcf && b[1]==0xfa && b[2]==0xed && b[3]==0xfe) {
        if(size<32 || word(4,4)!=0x01000013 || word(12,4)!=2)return fail("Not an Apple GPU executable archive");
        uint64_t count=word(16,4),commands=word(20,4);
        if(!count || count>4096 || commands>size-32)return fail("Invalid Apple GPU load commands");
        size_t at=32,end=32+size_t(commands),start=0,length=0;
        for(size_t n=0;n<count;++n) {
            if(end-at<8)return fail("Truncated Apple GPU load command");
            uint64_t command=word(at,4),bytes=word(at+4,4);
            if(bytes<8 || bytes>end-at || bytes%8)return fail("Invalid Apple GPU load command size");
            if(command==0x19) { // LC_SEGMENT_64, with section_64 records.
                if(bytes<72)return fail("Truncated Apple GPU segment");
                uint64_t sections=word(at+64,4),file=word(at+40,8),span=word(at+48,8);
                if(sections>(bytes-72)/80 || bytes!=72+sections*80 || file>size || span>size-file)
                    return fail("Invalid Apple GPU segment range");
                for(size_t i=0;i<sections;++i) {
                    size_t section=at+72+i*80;
                    if(std::memcmp(b+section,"__metallib\0\0\0\0\0\0",16))continue;
                    uint64_t offset=word(section+48,4),extent=word(section+40,8);
                    if(start || offset<file || offset>file+span || extent>file+span-offset || offset<end)
                        return fail("Invalid or duplicate Apple GPU AIR section");
                    start=size_t(offset);length=size_t(extent);
                }
            }
            at+=size_t(bytes);
        }
        if(at!=end || !start || !length)return fail("Apple GPU archive has no embedded AIR libraries");
        // The section is a sequence of complete MTLBs, each padded to 16 bytes.
        // Do not scan arbitrary machine code for magic strings.
        for(at=0;at<length;) {
            if(length-at<88 || std::memcmp(b+start+at,"MTLB",4))return fail("Invalid embedded AIR library");
            uint64_t bytes=word(start+at+16,8);
            if(bytes<88 || bytes>length-at)return fail("Embedded AIR library exceeds archive section");
            if(word(start+at+80,8)) {
                std::vector<Function> member;std::string diagnostic;
                if(!parse(b+start+at,size_t(bytes),member,diagnostic)){out.clear();error=diagnostic;return false;}
                if(member.size()>MaxFunctions-out.size())return fail("Too many archived AIR functions");
                for(auto &f:member){f.offset+=start+at;out.push_back(std::move(f));}
            }
            size_t next=(at+size_t(bytes)+15)&~size_t(15);
            if(next>length)return fail("Truncated embedded AIR alignment");
            for(size_t p=at+size_t(bytes);p<next;++p)if(b[start+p])return fail("Nonzero embedded AIR alignment padding");
            at=next;
        }
        if(out.empty())return fail("Apple GPU archive contains machine code without portable AIR");
        return true;
    }
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
    if(!count || count>MaxFunctions)return fail("Invalid MTLB function count");
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
        pos=finish;auto n=tags["NAME"],alias=tags["ALIS"],t=tags["TYPE"],o=tags["OFFT"],h=tags["HASH"],l=tags["MDSZ"];
        std::string identity;
        if(alias.second) {
            if(n.second || alias.second<4 || alias.second>8192)return fail("Invalid MTLB alias descriptor");
            auto separator=static_cast<const uint8_t *>(std::memchr(b+base+alias.first,0,alias.second));
            if(!separator || separator==b+base+alias.first)return fail("Invalid MTLB alias name");
            size_t prefix=size_t(separator-(b+base+alias.first));
            identity.assign(reinterpret_cast<const char *>(b+base+alias.first),prefix);
            n={alias.first+prefix+1,alias.second-prefix-1};
        }
        if(n.second<2 || n.second>4096 || b[base+n.first+n.second-1] || std::memchr(b+base+n.first,0,n.second-1) ||
            t.second!=1 || o.second!=24 || h.second!=32 || (l.second && l.second!=8))return fail("Invalid MTLB function descriptor");
        Function f;f.name.assign(reinterpret_cast<const char *>(b+base+n.first),n.second-1);f.type=b[base+t.first];f.specialized=alias.second!=0;
        if(!names.insert(f.specialized?identity:f.name).second)return fail("Duplicate MTLB function name");
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
