// SPDX-License-Identifier: GPL-2.0-only
#include "BasebandSMEMPool.h"
#include <SMEMProtocol.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
using namespace Baseband;
struct IO {
    std::vector<uint8_t> memory = std::vector<uint8_t>(131072);
    bool held = false, failUnlock = false, failPublish = false, lockBusy = false;
    unsigned writes = 0, publications = 0, releases = 0;
    Result lock() { assert(!held); if (lockBusy) return Result::Busy; held = true; return Result::OK; }
    bool unlock() { assert(held); held = false; return !failUnlock; }
    void acquire() { assert(held); }
    void release() { assert(held); ++releases; }
    bool read(uint32_t at, void *p, size_t n) {
        assert(held && range(at, n, memory.size())); memcpy(p, memory.data() + at, n); return true;
    }
    bool write(uint32_t at, const void *p, size_t n) {
        assert(held && range(at, n, memory.size())); ++writes; memcpy(memory.data() + at, p, n); return true;
    }
    bool store32(uint32_t at, uint32_t value) {
        assert(held && releases && !(at & 3)); ++publications;
        write32(memory.data() + at, value); return !failPublish; // Failure may already have published.
    }
    void partitions() {
        auto *toc = memory.data() + memory.size() - 4096;
        write32(toc, 0x434f5424); write32(toc + 4, 1); write32(toc + 8, 2);
        const uint32_t offsets[] = {4096, 73728}, sizes[] = {65536, 32768};
        for (unsigned i = 0; i < 2; ++i) {
            auto *p = toc + 32 + i * 48;
            write32(p, offsets[i]); write32(p + 4, sizes[i]);
            write16(p + 12, i ? 0xfffe : 0); write16(p + 14, i ? 0xfffe : 1); write32(p + 16, 64);
            p = memory.data() + offsets[i]; write32(p, 0x54525024);
            write16(p + 4, i ? 0xfffe : 0); write16(p + 6, i ? 0xfffe : 1);
            write32(p + 8, sizes[i]); write32(p + 12, 32); write32(p + 16, sizes[i]);
        }
    }
    void legacy() {
        write32(memory.data() + 92, 11u << 16); write32(memory.data() + 192, 1);
        write32(memory.data() + 196, 8400); write32(memory.data() + 200, uint32_t(memory.size() - 8400));
        // Bootloader's fixed allocation-table item aliases the fixed header.
        auto *e = memory.data() + 208 + 3 * 16;
        write32(e, 1); write32(e + 4, 0); write32(e + 8, 8400);
    }
};
int main() {
    QcomSMEMProtocol::Request policy;
    policy.operation = QcomSMEMProtocol::Allocate; policy.host = 1; policy.item = 479; policy.bytes = 16384;
    assert(QcomSMEMProtocol::permitted(policy, true) && !QcomSMEMProtocol::permitted(policy, false));
    policy.item = 480; assert(!QcomSMEMProtocol::permitted(policy, true)); // RX belongs to the modem.
    policy.host = QcomSMEMProtocol::GlobalHost; policy.item = 435; policy.bytes = 340;
    assert(QcomSMEMProtocol::permitted(policy, true));
    policy.item = 428; assert(!QcomSMEMProtocol::permitted(policy, true)); // SMP2P inbound belongs to the modem.
    policy = {}; policy.operation = QcomSMEMProtocol::Snapshot;
    policy.bytes = QcomSMEMProtocol::Bytes; policy.buffer = &policy;
    assert(QcomSMEMProtocol::permitted(policy, false) && !QcomSMEMProtocol::permitted(policy, true));
    policy.reserved = 1; assert(!QcomSMEMProtocol::valid(policy));
    IO io; io.partitions();
    auto pool = std::make_unique<SMEMPool<IO>>(io, uint32_t(io.memory.size()));
    SMEMItem item; bool created;
    assert(pool->item(1, 478, 0, item, created) == Result::NotReady && !created && !io.writes);
    assert(pool->item(1, 478, 32, item, created) == Result::OK && created && item.bytes == 32 && item.offset == 4144);
    assert(read32(io.memory.data() + 4108) == 80 && io.publications == 1);
    io.memory[item.offset + 4] = 0x42;
    const auto writes = io.writes;
    assert(pool->item(1, 478, 32, item, created) == Result::OK && !created && writes == io.writes);
    assert(io.memory[item.offset + 4] == 0x42); // Existing descriptor cursors must be preserved.
    assert(pool->item(1, 478, 64, item, created) == Result::Invalid && io.writes == writes);
    assert(pool->item(1, 479, 16384, item, created) == Result::OK && created);
    assert(pool->item(1, 480, 16384, item, created) == Result::OK && created);
    assert(pool->item(0xfffe, 435, 340, item, created) == Result::OK && created && item.bytes == 340 && item.offset == 73776);
    for (size_t i = 0; i < 344; ++i) assert(io.memory[item.offset + i] == 0);
    assert(pool->item(2, 435, 0, item, created) == Result::OK && item.offset == 73776); // No private host2 -> global fallback.
    const auto before = io.memory;
    assert(pool->item(0xfffe, 436, 65536, item, created) == Result::NoMemory && io.memory == before);
    assert(pool->item(1, 0, 16, item, created) == Result::Invalid);
    assert(pool->item(20, 479, 0, item, created) == Result::Invalid);
    assert(pool->item(1, 512, 0, item, created) == Result::Invalid);
    io.lockBusy = true;
    assert(pool->item(1, 478, 0, item, created) == Result::Busy && !io.held);
    io.lockBusy = false; io.failPublish = true;
    assert(pool->item(1, 481, 16, item, created) == Result::Quarantined && !created && !item.bytes);
    const auto count = io.publications;
    assert(pool->item(1, 481, 0, item, created) == Result::Quarantined && io.publications == count);
    IO old; old.legacy();
    auto legacy = std::make_unique<SMEMPool<IO>>(old, uint32_t(old.memory.size()));
    assert(legacy->item(1, 435, 340, item, created) == Result::OK && created && item.offset == 8400 && item.bytes == 344);
    assert(read32(old.memory.data() + 196) == 8744 && old.publications == 3);
    assert(legacy->item(1, 435, 340, item, created) == Result::OK && !created);
    write32(old.memory.data() + 208 + 435 * 16 + 12, 0x86000001);
    assert(legacy->item(1, 435, 0, item, created) == Result::OK); // Explicit main-region auxiliary base.
    write32(old.memory.data() + 208 + 435 * 16 + 12, 0x90000000);
    assert(legacy->item(1, 435, 0, item, created) == Result::Invalid);
    IO malformed; malformed.partitions();
    write32(malformed.memory.data() + 4108, UINT32_MAX);
    auto broken = std::make_unique<SMEMPool<IO>>(malformed, uint32_t(malformed.memory.size()));
    assert(broken->item(1, 478, 32, item, created) == Result::Invalid && !malformed.writes);
    IO unlock; unlock.partitions(); unlock.failUnlock = true;
    auto lost = std::make_unique<SMEMPool<IO>>(unlock, uint32_t(unlock.memory.size()));
    assert(lost->item(1, 478, 0, item, created) == Result::Quarantined && lost->quarantined());
    std::puts("baseband live SMEM allocation/global fallback/publication checks passed");
}
