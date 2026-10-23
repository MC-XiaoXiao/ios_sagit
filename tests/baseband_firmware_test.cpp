// SPDX-License-Identifier: GPL-2.0-only
#include "BasebandFirmware.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
using namespace Baseband;

static std::vector<uint8_t> fixture() {
    std::vector<uint8_t> b(512);
    write32(b.data(), 0x464c457f); b[4] = b[5] = b[6] = 1;
    write16(b.data() + 16, 2); write16(b.data() + 18, 164);
    write32(b.data() + 20, 1); write32(b.data() + 28, 52);
    write16(b.data() + 40, 52); write16(b.data() + 42, 32);
    write16(b.data() + 44, 4);
    auto header = [&](unsigned index, uint32_t type, uint32_t offset,
                      uint32_t address, uint32_t file, uint32_t memory, uint32_t flags) {
        auto *p = b.data() + 52 + 32 * index;
        write32(p, type); write32(p + 4, offset); write32(p + 12, address);
        write32(p + 16, file); write32(p + 20, memory); write32(p + 24, flags);
    };
    header(0, 0, 0, 0, 180, 0, 0x7000000);
    header(1, 0, 192, 0, 32, 32, MDTHash);
    header(2, 1, 256, 0x80000000, 32, 4096, MDTRelocatable | 5);
    header(3, 1, 288, 0x80001000, 16, 4096, MDTRelocatable | 6);
    for (unsigned i = 192; i < b.size(); ++i) b[i] = uint8_t(i);
    return b;
}
struct Sink {
    std::vector<uint8_t> memory = std::vector<uint8_t>(8192, 0xcc);
    unsigned copies = 0, publications = 0;
    bool fail = false;
    bool copy(uint64_t address, const uint8_t *p, size_t n) {
        ++copies;
        if (fail) return false;
        assert(range(address - 0x8d000000, n, memory.size()));
        memcpy(memory.data() + address - 0x8d000000, p, n); return true;
    }
    bool zero(uint64_t address, size_t n) {
        assert(range(address - 0x8d000000, n, memory.size()));
        memset(memory.data() + address - 0x8d000000, 0, n); return true;
    }
    bool publish(uint64_t, size_t) { ++publications; return true; }
};
int main() {
    auto data = fixture();
    FirmwareImage image;
    assert(image.parse(data.data(), data.size()) == Result::OK);
    assert(image.segmentCount() == 2 && image.codeLength() == 8192);
    assert(image.memorySpan() == 8192 && image.relocatable());
    std::vector<uint8_t> metadata(image.metadataBytes());
    assert(image.metadata(metadata.data(), metadata.size()) == Result::OK);
    assert(metadata.size() == 212 && !memcmp(metadata.data(), data.data(), 180));
    assert(!memcmp(metadata.data() + 180, data.data() + 192, 32));
    assert(image.metadata(data.data(), data.size()) == Result::Invalid);
    Sink sink;
    assert(image.load({0x8d000000, 4096}, sink) == Result::Invalid);
    assert(sink.copies == 0); // Full preflight before touching destination.
    assert(image.load({0x8d000000, 8192}, sink) == Result::OK);
    assert(sink.copies == 2 && sink.publications == 2);
    for (size_t i = 32; i < 4096; ++i) assert(sink.memory[i] == 0);
    for (size_t i = 4096 + 16; i < 8192; ++i) assert(sink.memory[i] == 0);
    sink.fail = true;
    assert(image.load({0x8d000000, 8192}, sink) == Result::IO);
    auto reject = [&](size_t offset, uint32_t value) {
        auto bad = data; write32(bad.data() + offset, value);
        assert(image.parse(bad.data(), bad.size()) != Result::OK);
        assert(!image.valid() && !image.segment(0));
    };
    reject(28, UINT32_MAX);
    reject(52 + 16, 100); // Header omits program table.
    reject(52 + 64 + 16, 8192); // filesz exceeds memsz.
    reject(52 + 64 + 4, 500); // Truncated segment.
    reject(52 + 96 + 12, 0x80000080); // Physical overlap.
    reject(52 + 64 + 12, 0xfffff800); // Physical end exceeds the 32-bit aperture.
    reject(52 + 64 + 20, 8192); // Overlap with next segment.
    reject(52 + 32 + 24, 0); // Missing signed hash metadata.
    reject(52 + 64 + 4, 192); // Data aliases authentication hashes.
    for (size_t n = 0; n < 304; ++n)
        assert(image.parse(data.data(), n) != Result::OK);
    assert(image.parse(nullptr, data.size()) == Result::Invalid);
    auto fixed = data;
    write32(fixed.data() + 52 + 64 + 24, 5);
    write32(fixed.data() + 52 + 96 + 24, 6);
    assert(image.parse(fixed.data(), fixed.size()) == Result::OK && !image.relocatable());
    assert(image.validateRegion({0x8d000000, 8192}) == Result::Invalid);
    assert(image.validateRegion({0x80000000, 8192}) == Result::OK);
    // The same production parser consumes the pinned monolithic modem image.
    std::ifstream input("firmware/sagit-baseband/modem.mbn", std::ios::binary);
    if (input) {
        std::vector<uint8_t> real((std::istreambuf_iterator<char>(input)), {});
        assert(image.parse(real.data(), real.size()) == Result::OK);
        assert(image.segmentCount() == 28 && image.lowestAddress() == 0x8d000000);
        assert(image.memorySpan() == 0x7000000);
        assert(image.validateRegion({0x8d000000, 0x7000000}) == Result::OK);
        assert(image.validateRegion({0x8d000000, 0x6fff000}) == Result::Invalid);
        std::printf("Real sagit modem: %u segments, span %#x, metadata %zu bytes\n",
            image.segmentCount(), image.memorySpan(), image.metadataBytes());
    } else std::puts("Real firmware fixture unavailable; synthetic parser checks only");
    std::puts("baseband firmware checks passed");
}
