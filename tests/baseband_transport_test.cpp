// SPDX-License-Identifier: GPL-2.0-only
#include "BasebandSMEM.h"
#include "BasebandFIFO.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace Baseband;
struct Ring {
    std::array<uint8_t, 64> data{};
    uint32_t h = 0, t = 0;
    unsigned kicks = 0, releases = 0, acquires = 0;
    bool failKick = false;
    bool head(uint32_t &v) { v = h; return true; }
    bool tail(uint32_t &v) { v = t; return true; }
    bool setHead(uint32_t v) { assert(releases); h = v; return true; }
    bool setTail(uint32_t v) { assert(releases); t = v; return true; }
    void acquire() { ++acquires; }
    void release() { ++releases; }
    bool read(uint32_t off, void *p, size_t n) {
        assert(range(off, n, data.size())); memcpy(p, data.data() + off, n); return true;
    }
    bool write(uint32_t off, const void *p, size_t n) {
        assert(range(off, n, data.size())); memcpy(data.data() + off, p, n); return true;
    }
    bool kick() { ++kicks; return !failKick; }
};
int main() {
    Ring backend; FIFO<Ring> fifo(backend, 64);
    uint8_t packet[48]{}, output[48]{};
    for (unsigned i = 0; i < sizeof(packet); ++i) packet[i] = i;
    assert(fifo.push(packet, 48) == Result::OK && backend.h == 48);
    assert(fifo.push(packet, 8) == Result::Busy);
    uint8_t notif[8]{}; write16(notif, 13);
    assert(fifo.push(notif, 8, true) == Result::OK);
    assert(fifo.consume(48) == Result::OK);
    assert(fifo.consume(8) == Result::OK);
    assert(fifo.push(packet, 17) == Result::OK && backend.h == 16);
    assert(fifo.peek(0, output, 24) == Result::OK);
    assert(!memcmp(packet, output, 17));
    for (unsigned i = 17; i < 24; ++i) assert(output[i] == 0);
    assert(fifo.peek(SIZE_MAX, output, 8) == Result::NotReady);
    assert(fifo.consume(25) == Result::Invalid);
    assert(fifo.consume(24) == Result::OK);
    size_t count = 999;
    assert(fifo.available(count) == Result::OK && count == 0);
    backend.failKick = true;
    assert(fifo.push(packet, 8) == Result::Quarantined);
    const auto head = backend.h;
    assert(fifo.push(packet, 8) == Result::Quarantined && backend.h == head);
    Ring invalid; invalid.h = 64; FIFO<Ring> broken(invalid, 64);
    assert(broken.available(count) == Result::Invalid && count == 0);
    assert(broken.available(count) == Result::Quarantined);
    Ring invalidAlignment; invalidAlignment.t = 3;
    FIFO<Ring> alignment(invalidAlignment, 64);
    assert(alignment.available(count) == Result::Invalid);

    std::vector<uint8_t> smem(16384);
    auto *toc = smem.data() + 12288, *part = smem.data() + 4096;
    write32(toc, 0x434f5424); write32(toc + 4, 1); write32(toc + 8, 1);
    write32(toc + 32, 4096); write32(toc + 36, 4096);
    write16(toc + 44, 0); write16(toc + 46, 1); write32(toc + 48, 64);
    write32(part, 0x54525024); write16(part + 4, 0); write16(part + 6, 1);
    write32(part + 8, 4096); write32(part + 12, 80); write32(part + 16, 4096);
    write16(part + 32, 0xa5a5); write16(part + 34, 478); write32(part + 36, 32);
    SMEMSnapshot view; SMEMItem item;
    assert(view.bind(smem.data(), smem.size()) == Result::OK);
    assert(view.find(0, 1, 478, item) == Result::OK && item.offset == 4144 && item.bytes == 32 && !item.cached);
    assert(view.find(1, 0, 479, item) == Result::NotReady);
    write16(part + 40, 33);
    assert(view.find(0, 1, 478, item) == Result::Invalid);
    write16(part + 40, 0); write32(part + 36, UINT32_MAX);
    assert(view.find(0, 1, 478, item) == Result::Invalid);
    write32(part + 36, 32);
    // One cached allocation: 64-byte padded header and 64-byte padded data.
    write32(part + 16, 3968);
    write16(part + 4032, 0xa5a5); write16(part + 4034, 479);
    write32(part + 4036, 64); write16(part + 4040, 32); write16(part + 4042, 48);
    assert(view.find(0, 1, 479, item) == Result::OK && item.cached && item.bytes == 32 && item.offset == 8064);
    write16(part + 4034, 478);
    assert(view.find(0, 1, 478, item) == Result::Invalid); // Ambiguous duplicate.
    write32(toc + 36, UINT32_MAX);
    assert(view.bind(smem.data(), smem.size()) == Result::Invalid);
    assert(view.find(0, 1, 478, item) == Result::NotReady);

    std::vector<uint8_t> legacy(16384);
    write32(legacy.data() + 92, 11u << 16); write32(legacy.data() + 192, 1);
    write32(legacy.data() + 196, 9000); write32(legacy.data() + 200, 16384 - 9000);
    auto *oldItem = legacy.data() + 208 + 16 * 435;
    write32(oldItem, 1); write32(oldItem + 4, 8400); write32(oldItem + 8, 344);
    assert(view.bind(legacy.data(), legacy.size()) == Result::OK);
    assert(view.find(0, 1, 435, item) == Result::OK && item.offset == 8400 && item.bytes == 344);
    assert(view.find(0, 1, 428, item) == Result::NotReady);
    write32(oldItem + 12, 0x90000000); // Auxiliary physical memory is not an offset into this mapping.
    assert(view.bind(legacy.data(), legacy.size()) == Result::Invalid);
    write32(oldItem + 12, 0); write32(oldItem + 8, UINT32_MAX);
    assert(view.bind(legacy.data(), legacy.size()) == Result::Invalid);

    uint8_t shared[SMP2P::Bytes]{};
    assert(SMP2P::initialize(shared, sizeof(shared)) == Result::OK);
    write16(shared + 8, 1); write16(shared + 10, 0);
    memset(shared + 20, 0, 16); memcpy(shared + 20, "slave-kernel", 12);
    SMP2P::Monitor monitor; SMP2P::Observation observation;
    write32(shared + 36, SMP2P::Ready);
    assert(monitor.sample(shared, sizeof(shared), observation) == Result::OK);
    assert(observation.rising == SMP2P::Ready && !observation.restart);
    uint8_t alignedShared[344]{}; memcpy(alignedShared, shared, sizeof(shared));
    assert(monitor.sample(alignedShared, sizeof(alignedShared), observation) == Result::OK);
    assert(monitor.sample(alignedShared, 339, observation) == Result::Invalid);
    assert(monitor.sample(shared, sizeof(shared), observation) == Result::OK && !observation.rising);
    write32(shared + 16, 1); write32(shared + 36, SMP2P::Fatal);
    assert(monitor.sample(shared, sizeof(shared), observation) == Result::OK);
    assert(observation.restart && observation.outboundFlags == 2 && observation.rising == SMP2P::Fatal);
    write16(shared + 14, 17);
    assert(monitor.sample(shared, sizeof(shared), observation) == Result::Invalid);
    write16(shared + 14, 2); memcpy(shared + 40, shared + 20, 20);
    assert(monitor.sample(shared, sizeof(shared), observation) == Result::Invalid);
    std::puts("baseband SMEM/SMP2P/FIFO checks passed");
}
