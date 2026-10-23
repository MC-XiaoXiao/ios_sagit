// SPDX-License-Identifier: GPL-2.0-only
#include "BasebandMemory.h"
#include "BasebandMemoryAccess.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
using namespace Baseband;
struct CPU {
    unsigned copies = 0, zeros = 0, publications = 0;
    bool copy(unsigned i, uint64_t offset, const uint8_t *p, size_t n) {
        assert(i < 3 && p && n && offset < 0x7000000); ++copies; return true;
    }
    bool zero(unsigned i, uint64_t offset, size_t n) {
        assert(i < 3 && n && offset < 0x7000000); ++zeros; return true;
    }
    bool publish(unsigned i, uint64_t offset, size_t n) {
        assert(i < 3 && n && offset < 0x7000000); ++publications; return true;
    }
};
static std::vector<uint8_t> file(const char *path) {
    std::ifstream input(path, std::ios::binary); assert(input);
    return {(std::istreambuf_iterator<char>(input)), {}};
}
int main() {
    const auto b = file("firmware/sagit-baseband/mba.mbn"), m = file("firmware/sagit-baseband/modem.mbn");
    MemoryHandoff manifest;
    manifest.mbaBytes=b.size();manifest.modemBytes=m.size();manifest.modemOffset=(b.size()+15)&~15ULL;
    manifest.mbaHash[0]=1;manifest.modemHash[0]=2;
    uint8_t data[MemoryHandoff::Bytes+1]{};
    assert(manifest.valid(false) && !manifest.valid());
    assert(manifest.encode(data+1,MemoryHandoff::Bytes,false)==Result::OK);
    MemoryHandoff parsed;
    assert(parsed.decode(data+1,MemoryHandoff::Bytes)==Result::Invalid);
    assert(parsed.decode(data+1,MemoryHandoff::Bytes,false)==Result::OK);
    manifest.flags=MemoryHandoff::Verified;manifest.layout.metadata={0xb0000000,0x4000};
    manifest.image={0xb1000000,manifest.imageBytes()};manifest.arena={0xa0000000,0x10000000};
    assert(manifest.valid() && manifest.encode(data+1,MemoryHandoff::Bytes)==Result::OK);
    assert(parsed.decode(data+1,MemoryHandoff::Bytes)==Result::OK && parsed.image.address==0xb1000000);
    for (auto at:{0u,4u,6u,8u,12u,148u,216u}) {
        data[1+at]^=0x80;assert(parsed.decode(data+1,MemoryHandoff::Bytes)==Result::Invalid);data[1+at]^=0x80;
    }
    for (const auto region:std::vector<Region>{{0x94400000,0x4000},{0xb0000000,0x1000},{UINT64_MAX-0x3fff,0x4000}}) {
        auto bad=manifest;bad.layout.metadata=region;assert(!bad.valid());
    }
    auto overlap=manifest;overlap.image.address=overlap.layout.modem.address;assert(!overlap.valid());
    overlap=manifest;overlap.image.address=overlap.arena.address;assert(!overlap.valid());
    overlap=manifest;overlap.modemOffset=UINT64_MAX;assert(!overlap.valid());
    overlap=manifest;overlap.policy.cx=256;assert(!overlap.valid());
    for(size_t bytes=0;bytes<MemoryHandoff::Bytes;++bytes)assert(parsed.decode(data+1,bytes)==Result::Invalid);
    auto extended=manifest;extended.version=MemoryHandoff::CatalogVersion;
    extended.filesOffset=extended.bootImageBytes();extended.filesBytes=4709032;extended.filesHash[0]=3;
    extended.image.bytes=extended.imageBytes();
    uint8_t larger[MemoryHandoff::MaxBytes+1]{};
    assert(extended.wireBytes()==272 && extended.valid());
    assert(extended.encode(larger+1,272)==Result::OK && parsed.decode(larger+1,272)==Result::OK);
    assert(parsed.filesBytes==4709032 && parsed.filesHash[0]==3 && parsed.image.bytes>manifest.image.bytes);
    assert(parsed.decode(larger+1,224)==Result::Invalid && extended.encode(larger+1,224)==Result::Invalid);
    auto invalid=extended;invalid.filesOffset-=16;assert(!invalid.valid());
    invalid=extended;invalid.filesBytes=8*1024*1024+1;assert(!invalid.valid());
    invalid=extended;memset(invalid.filesHash,0,32);assert(!invalid.valid());
    invalid=extended;invalid.image.bytes=manifest.image.bytes;assert(!invalid.valid());
    invalid=extended;invalid.version=1;assert(!invalid.valid());
    CPU cpu;MemoryAccess<CPU> memory(cpu);const auto &layout=manifest.layout;
    assert(!memory.copy(layout.mba.address,b.data(),b.size()) && !cpu.copies);
    assert(memory.confirm(layout)==Result::OK && memory.confirm(layout)==Result::Invalid);
    assert(memory.copy(layout.mba.address,b.data(),b.size()) && cpu.copies==1);
    assert(!memory.copy(layout.mba.address+layout.mba.bytes-8,b.data(),16));
    assert(!memory.copy(0x94400000,b.data(),16));
    assert(memory.assigned(layout.modem,Ownership::Host,Ownership::Modem)==Result::OK);
    assert(!memory.copy(layout.modem.address,m.data(),16) && !memory.zero(layout.modem.address,4096));
    assert(!memory.publish(layout.modem.address,4096) && !memory.canClose());
    assert(memory.assigned(layout.modem,Ownership::Host,Ownership::Shared)==Result::Invalid);
    assert(memory.assigned(layout.modem,Ownership::Modem,Ownership::Shared)==Result::OK);
    assert(memory.copy(layout.modem.address,m.data(),16) && memory.zero(layout.modem.address+16,4096));
    assert(memory.publish(layout.modem.address,4112));
    assert(memory.assigned(layout.modem,Ownership::Shared,Ownership::Host)==Result::OK && memory.canClose());
    memory.quarantine();
    const auto copies=cpu.copies;
    assert(!memory.copy(layout.mba.address,b.data(),16) && !memory.canClose() && cpu.copies==copies);
    std::puts("baseband EFI handoff/bounds/overlap/CPU ownership ledger checks passed");
}
