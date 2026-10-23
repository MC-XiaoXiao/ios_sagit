// SPDX-License-Identifier: GPL-2.0-only
#include "BasebandRMTFSStorage.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
using namespace Baseband;
namespace R = Baseband::RMTFS;
struct Disk {
    std::vector<uint8_t> data = std::vector<uint8_t>(65536);
    uint32_t block = 4096;
    bool writable = true;
    unsigned reads = 0, writes = 0, fail = 0;
    size_t shortBy = 0;
    Result error = Result::IO;
    Disk() { for (size_t i=0;i<data.size();++i)data[i]=uint8_t(i^(i>>8)); }
    Result geometry(unsigned p,uint64_t &n,uint32_t &b,bool &w) {
        assert(p==0); n=data.size();b=block;w=writable;return Result::OK;
    }
    Result transfer(unsigned p,bool write,uint64_t offset,void *buffer,size_t bytes,size_t &actual) {
        assert(p==0 && !(offset%block) && !(bytes%block) && bytes<=R::TransferBytes && range(offset,bytes,data.size()));
        if(write)++writes;else ++reads;
        actual=0;if(reads+writes==fail)return error;
        actual=bytes-shortBy;
        if(write)memcpy(data.data()+offset,buffer,actual);else memcpy(buffer,data.data()+offset,actual);
        return Result::OK;
    }
};
struct Shared {
    unsigned calls = 0, fail = 0;
    bool mapped = false;
    Ownership owner = Ownership::Unknown;
    Result error = Result::IO;
    Result step() { return ++calls == fail ? error : Result::OK; }
    Result confirm() { const auto r=step();if(r==Result::OK)owner=Ownership::Host;return r; }
    Result map() { assert(owner==Ownership::Host);const auto r=step();if(r==Result::OK)mapped=true;return r; }
    Result assign(Ownership from,Ownership to) {
        assert(owner==from && mapped);const auto r=step();if(r==Result::OK)owner=to;return r;
    }
    bool unmap() { assert(owner==Ownership::Host);mapped=false;return true; }
};
static std::vector<uint8_t> policy() {
    std::vector<uint8_t> p(R::StoragePolicy::Bytes);
    write32(p.data(),R::StoragePolicy::Magic);write16(p.data()+4,1);write16(p.data()+6,uint16_t(p.size()));
    write64(p.data()+8,1);write32(p.data()+16,4);
    for(unsigned i=0;i<4;++i) {
        auto *e=p.data()+32+i*48;e[0]=uint8_t(i+1);
        write64(e+16,0x100000+i*0x10000);write64(e+24,0x10000);write32(e+32,0);write32(e+36,3);
    }
    return p;
}
int main() {
    Shared shared;R::SharedRegion<Shared> region(shared);
    assert(region.acquire()==Result::OK && region.ready() && shared.owner==Ownership::Shared);
    assert(region.retire(false)==Result::NotReady && shared.calls==3);
    assert(region.retire(true)==Result::OK && region.canClose() && !shared.mapped && shared.owner==Ownership::Host);
    for(unsigned fail=1;fail<=3;++fail) {
        Shared known;known.fail=fail;R::SharedRegion<Shared> lease(known);
        assert(lease.acquire()==Result::IO && lease.canClose() && !known.mapped);
        Shared unknown;unknown.fail=fail;unknown.error=Result::Quarantined;R::SharedRegion<Shared> pinned(unknown);
        assert(pinned.acquire()==Result::Quarantined && pinned.quarantined() && !pinned.canClose());
        const unsigned calls=unknown.calls;
        assert(pinned.retire(true)==Result::Quarantined && unknown.calls==calls);
    }
    Shared failedReturn;R::SharedRegion<Shared> pinned(failedReturn);
    assert(pinned.acquire()==Result::OK);failedReturn.fail=4;
    assert(pinned.retire(true)==Result::Quarantined && failedReturn.mapped && failedReturn.owner==Ownership::Shared);
    uint8_t a[16],b[16];
    assert(R::uuid("00112233-4455-6677-8899-aabbccddeeff",36,a));
    assert(R::uuid("00112233-4455-6677-8899-AABBCCDDEEFF",36,b) && !memcmp(a,b,16));
    assert(!R::uuid("00112233-4455-6677-8899-aabbccddeefg",36,b));
    auto raw=policy();R::StoragePolicy parsed;
    assert(parsed.decode(raw.data(),raw.size())==Result::OK && parsed.valid());
    for(unsigned off:{0u,4u,6u,16u,20u,24u,72u}) {
        auto bad=raw;bad[off]^=128;
        assert(parsed.decode(bad.data(),bad.size())==Result::Invalid && !parsed.valid());
    }
    auto bad=raw;memcpy(bad.data()+80,bad.data()+32,16);
    assert(parsed.decode(bad.data(),bad.size())==Result::Invalid); // Duplicate UUID.
    bad=raw;write64(bad.data()+96,0x100200);
    assert(parsed.decode(bad.data(),bad.size())==Result::Invalid); // Overlap within LUN.
    bad=raw;write32(bad.data()+68,0);
    assert(parsed.decode(bad.data(),bad.size())==Result::Invalid);
    bad=raw;write64(bad.data()+48,UINT64_MAX-511);write64(bad.data()+56,1024);
    assert(parsed.decode(bad.data(),bad.size())==Result::Invalid);
    for(uint32_t physical:{512u,4096u,16384u}) {
        Disk disk;disk.block=physical;auto blocks=std::make_unique<R::StorageBlocks<Disk>>(disk);
        auto original=disk.data;std::vector<uint8_t> source(32768,0x76),read(source.size());
        assert(blocks->write(0,512,source.data(),source.size())==Result::OK);
        assert(!memcmp(disk.data.data(),original.data(),512));
        assert(!memcmp(disk.data.data()+512,source.data(),source.size()));
        assert(!memcmp(disk.data.data()+512+source.size(),original.data()+512+source.size(),disk.data.size()-512-source.size()));
        assert(blocks->read(0,512,read.data(),read.size())==Result::OK && read==source);
        assert(blocks->read(0,65024,read.data(),512)==Result::OK);
        assert(blocks->read(0,65024,read.data(),1024)==Result::Invalid);
        const unsigned writes=disk.writes,reads=disk.reads;
        disk.writable=false;assert(blocks->write(0,512,source.data(),512)==Result::Unsupported);
        assert(disk.writes==writes && disk.reads==reads);
        assert(blocks->write(0,1,source.data(),512)==Result::Invalid);
    }
    Disk shortRead;shortRead.shortBy=512;auto blocks=std::make_unique<R::StorageBlocks<Disk>>(shortRead);
    std::vector<uint8_t> buffer(512,0xbc);auto before=buffer;
    assert(blocks->read(0,512,buffer.data(),512)==Result::IO && buffer==before); // Never expose the unfilled tail.
    auto original=shortRead.data;
    assert(blocks->write(0,512,buffer.data(),512)==Result::IO && shortRead.data==original && !shortRead.writes);
    Disk unknown;unknown.fail=1;unknown.error=Result::Quarantined;
    auto uncertain=std::make_unique<R::StorageBlocks<Disk>>(unknown);
    assert(uncertain->write(0,512,buffer.data(),512)==Result::Quarantined && !unknown.writes);
    std::puts("baseband RMTFS media identity, 512/4K/16K block adaptation and short-I/O checks passed");
}
