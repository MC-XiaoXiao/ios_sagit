/* SPDX-License-Identifier: BSD-2-Clause */
#include "UFSProtocol.h"
#include "QcomUFSPhy.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <utility>
#include <array>
#include <random>
#include <map>

struct FakeMMIO {
    std::array<uint32_t, 0x1000 / 4> regs{};
    std::vector<std::pair<uint32_t,uint32_t>> writes;
    unsigned elapsed = 0, disableAfter = 0, commandAfter = 0;
    bool disableStuck = false, uicStuck = false;
    uint32_t uicError = 0, powerStatus = 1;
    std::map<uint32_t, uint32_t> attributes;
    uint32_t read(uint32_t offset) { assert(offset < 0x1000); return regs[offset / 4]; }
    void write(uint32_t offset, uint32_t value) {
        assert(offset < 0x1000); writes.emplace_back(offset, value);
        if (offset == UFS::IS) regs[offset / 4] &= ~value;
        else if (offset == UFS::UTRLDBR) regs[offset / 4] |= value;
        else if (offset == UFS::HCE && !value) {
            if (regs[offset / 4]) disableAfter = elapsed + 50;
        } else regs[offset / 4] = value;
        if (offset == UFS::UICCMD) commandAfter = elapsed + 20;
    }
    void delay(unsigned usec) {
        elapsed += usec;
        if (disableAfter && elapsed >= disableAfter && !disableStuck) {
            regs[UFS::HCE / 4] = 0; regs[UFS::UTRLDBR / 4] = 0; disableAfter = 0;
        }
        if (commandAfter && elapsed >= commandAfter && !uicStuck) {
            regs[UFS::IS / 4] |= UFS::UICDone;
            regs[UFS::UICARG2 / 4] = uicError;
            uint32_t opcode = regs[UFS::UICCMD / 4];
            uint32_t mib = regs[UFS::UICARG1 / 4] >> 16;
            uint32_t key = mib | ((opcode == 3 || opcode == 4) ? 0x10000 : 0);
            if (!uicError && (opcode == 1 || opcode == 3) && attributes.count(key))
                regs[UFS::UICARG3 / 4] = attributes[key];
            if (!uicError && (opcode == 2 || opcode == 4)) attributes[key] = regs[UFS::UICARG3 / 4];
            if (opcode == 2 && regs[UFS::UICARG1 / 4] == uint32_t(UFS::PowerMode) << 16)
                regs[UFS::IS / 4] |= UFS::PowerDone;
            if (opcode == 0x17) regs[UFS::IS / 4] |= UFS::HibernateEnter;
            if (opcode == 0x18) regs[UFS::IS / 4] |= UFS::HibernateExit;
            regs[UFS::HCS / 4] = (regs[UFS::HCS / 4] & ~0x700u) | powerStatus << 8;
            commandAfter = 0;
        }
    }
};

static void protocolTests() {
    UFS::PlatformContract contract{1, 15, 200000000, 150000000, 40, 1};
    assert(contract.valid());
    for (unsigned missing = 0; missing < 4; ++missing) {
        auto bad = contract; bad.flags &= ~(1u << missing); assert(!bad.valid());
    }
    auto bad = contract; bad.flags |= 32; assert(!bad.valid());
    bad = contract; bad.coreHz = 50000000; assert(!bad.valid());
    bad = contract; bad.lanes = 3; assert(!bad.valid());
    bad = contract; bad.addressBits = 64; assert(!bad.valid());
    uint8_t cdb[16], request[512]; uint32_t bytes;
    assert(UFS::readWriteCDB(cdb, 0x123456789ULL, 128, 4096, 0x200000000ULL, true, true, bytes));
    assert(bytes == 524288 && cdb[0] == 0x8a && cdb[1] == 8);
    assert(UFS::be64(cdb + 2) == 0x123456789ULL && UFS::be32(cdb + 10) == 128);
    UFS::scsi(request, 7, 31, cdb, bytes, true, true);
    assert(request[0] == 1 && request[1] == 0x21 && request[2] == 7 && request[3] == 31);
    assert(request[5] == 0 && UFS::be32(request + 12) == bytes && !memcmp(request + 16, cdb, 16));
    assert(!UFS::readWriteCDB(cdb, UINT64_MAX, 1, 4096, UINT64_MAX, false, false, bytes));
    assert(!UFS::readWriteCDB(cdb, 100, 1, 4096, 100, false, false, bytes));
    assert(!UFS::readWriteCDB(cdb, 99, 2, 4096, 100, false, false, bytes));
    assert(!UFS::readWriteCDB(cdb, 0, 257, 4096, 1000, false, false, bytes));
    assert(!UFS::readWriteCDB(cdb, 0, 1, 1000, 1000, false, false, bytes));
    assert(!UFS::readWriteCDB(cdb, 0, 0, 4096, 1000, false, false, bytes));
    assert(UFS::readWriteCDB(cdb, 99, 1, 512, 100, false, false, bytes));
    assert(cdb[0] == 0x28 && bytes == 512 && UFS::be32(cdb+2)==99 && UFS::be16(cdb+7)==1);
    assert(UFS::readWriteCDB(cdb, UINT32_MAX, 1, 4096, uint64_t(UINT32_MAX)+2, true, true, bytes));
    assert(cdb[0]==0x2a && cdb[1]==8 && UFS::be32(cdb+2)==UINT32_MAX);
    assert(UFS::readWriteCDB(cdb, UINT32_MAX, 2, 4096, uint64_t(UINT32_MAX)+2, false, false, bytes));
    assert(cdb[0]==0x88 && UFS::be64(cdb+2)==UINT32_MAX && UFS::be32(cdb+10)==2);
    UFS::query(request, 31, 1, 2, 7, 255);
    assert(request[0] == 0x16 && request[3] == 31 && request[5] == 1);
    assert(request[12] == 1 && request[13] == 2 && request[14] == 7 && UFS::be16(request + 18) == 255);
    UFS::query(request, 0, 6, 1, 0, 0); assert(request[5] == 0x81);
    UFS::query(request, 0, 3, 0x0e, 0, 0); assert(request[5] == 1);

    UFS::Transfer transfer;
    UFS::initTransfer(transfer, 0x1234567800, 256, 2);
    assert(transfer.header == 0x15000000 && transfer.ocs == 15 && transfer.commandAddress == 0x1234567800);
    assert(transfer.responseLength == 128 && transfer.responseOffset == 128 && transfer.prdtLength == 256 && transfer.prdtOffset == 256);
    uint8_t *raw = reinterpret_cast<uint8_t *>(&transfer);
    assert(raw[3] == 0x15 && raw[16] == 0 && raw[17] == 0x78 && raw[24] == 128 && raw[29] == 1);
}

static void dmaTests() {
    UFS::PRD table[UFS::MaxPRDs]; unsigned count = 0;
    assert(UFS::appendPRD(table, count, 0x120000000ULL, UFS::MaxTransfer, 40));
    assert(count == 4);
    for (unsigned i = 0; i < 4; ++i) {
        assert(table[i].address == 0x120000000ULL + i * UFS::MaxPRDBytes);
        assert(!table[i].reserved && table[i].byteCount == 0x3ffff);
    }
    count = 0; assert(!UFS::appendPRD(table, count, 0x100000000ULL, 4096, 32));
    assert(!UFS::appendPRD(table, count, UINT64_MAX - 3, 8, 64));
    assert(!UFS::appendPRD(table, count, 0x1001, 4096, 40));
    assert(!UFS::appendPRD(table, count, 0x1000, 4095, 40));
    assert(!UFS::appendPRD(table, count, 0, 0, 40));
    assert(UFS::appendPRD(table, count, 0xfffff000, 4096, 32));
    count = UFS::MaxPRDs; assert(!UFS::appendPRD(table, count, 0, 4, 40));
    std::mt19937 rng(835);
    // Random discontiguous and maximum-length SG spans, including 4G crossings.
    for (unsigned test = 0; test < 10000; ++test) {
        uint64_t start = (uint64_t(rng()) << 4) & ~3ULL;
        uint64_t size = (rng() % (UFS::MaxTransfer / 4) + 1) * 4;
        count = 0; assert(UFS::appendPRD(table, count, start, size, 40));
        uint64_t sum = 0;
        for (unsigned i = 0; i < count; ++i) {
            assert(table[i].address == start + sum && table[i].byteCount < UFS::MaxPRDBytes);
            assert((table[i].byteCount & 3) == 3 && !table[i].reserved);
            sum += table[i].byteCount + 1;
        }
        assert(sum == size && count <= 5);
    }
}

static void responseTests() {
    assert(!UFS::uicRequiresRecovery(0x80000010,0x80000001,0,0,0));
    assert(UFS::uicRequiresRecovery(0,0x80000002,0,0,0));
    assert(UFS::uicRequiresRecovery(0,0x80002000,0,0,0));
    assert(UFS::uicRequiresRecovery(0x80000001,0,0,0,0));
    assert(UFS::uicRequiresRecovery(0,0,0x80000001,0,0));
    assert(UFS::uicRequiresRecovery(0,0,0,0x80000001,0));
    assert(UFS::uicRequiresRecovery(0,0,0,0,0x80000001));
    UFS::Transfer transfer{}; uint8_t p[512] = {};
    p[0] = 0x21; p[2] = 7; p[3] = 31;
    auto parse = [&]() { return UFS::response(transfer, p, UFS::Response, 31, 7, 4096); };
    auto result = parse(); assert(result.result == UFS::Result::Good && result.bytes == 4096);
    p[9] = 1; assert(parse().result == UFS::Result::Good); // Valid device exception notification.
    p[9] = 2; assert(parse().result == UFS::Result::Protocol); p[9] = 0;
    p[3] = 30; assert(parse().result == UFS::Result::Protocol); p[3] = 31;
    p[2] = 0; assert(parse().result == UFS::Result::Protocol); p[2] = 7;
    transfer.ocs = 1; assert(parse().result == UFS::Result::Controller); transfer.ocs = 0;
    p[0] = 0x22; assert(parse().result == UFS::Result::Protocol); p[0] = 0x21;
    p[1] = 0x40; assert(parse().result == UFS::Result::Protocol);
    p[1] = 0x20; UFS::put32(p + 12, 512);
    result = parse(); assert(result.result == UFS::Result::ShortTransfer && result.bytes == 3584);
    UFS::put32(p + 12, 4097); assert(parse().result == UFS::Result::Protocol);
    p[1] = 0; UFS::put32(p + 12, 0); p[7] = 8; assert(parse().result == UFS::Result::Busy);
    p[7] = 2; UFS::put16(p + 10, 20); UFS::put16(p + 32, 18);
    p[34] = 0x70; p[36] = 7; p[46] = 0x27;
    result = parse(); assert(result.result == UFS::Result::CheckCondition && result.senseKey == 7 && result.asc == 0x27);
    p[6] = 1;
    result = parse(); assert(result.result == UFS::Result::CheckCondition && result.senseKey == 7 && result.asc == 0x27);
    p[6] = 2;
    assert(parse().result == UFS::Result::Protocol);
    p[6] = 0;
    UFS::put16(p + 32, 511); assert(parse().result == UFS::Result::Protocol);
    UFS::zero(p, sizeof(p)); p[0] = UFS::QueryResponse; p[3] = 31;
    assert(UFS::response(transfer, p, UFS::QueryResponse, 31, 0, 0).result == UFS::Result::Good);
    p[6] = 0xfc;
    assert(UFS::response(transfer, p, UFS::QueryResponse, 31, 0, 0).result == UFS::Result::QueryRejected);
    p[6] = 0; UFS::put16(p + 10, 256);
    assert(UFS::response(transfer, p, UFS::QueryResponse, 31, 0, 0).result == UFS::Result::Protocol);
}

struct Request {
    Request *next = nullptr;
    bool ordered = false;
    uint8_t responseType = UFS::Response, lun = 0;
};
struct LUN { bool enabled = true; unsigned depth = 32; };
static void queueTests() {
    Request requests[100], *active[32] = {}, *head = &requests[0], *tail = &requests[99], *request;
    LUN luns[8]; uint32_t mask = 0; unsigned count = 100, tag;
    for (unsigned i = 0; i < 99; ++i) requests[i].next = &requests[i + 1];
    auto select = [&]() { return UFS::selectPending(head, tail, count, active, 32, luns, mask, tag, request); };
    // All 32 hardware slots are used, including bit 31. Complete in reverse
    // order and reuse tags without reissuing a completed doorbell bit.
    for (unsigned i = 0; i < 32; ++i) {
        assert(select() && tag == i); active[tag] = request; mask |= 1u << tag;
    }
    assert(mask == UINT32_MAX && count == 68 && !select());
    for (int i = 31; i >= 0; --i) {
        uint32_t doorbell = mask & ~(1u << i);
        assert(UFS::completedSlots(mask, doorbell) == (1u << i));
        active[i] = nullptr; mask = doorbell;
    }
    assert(!mask);
    head->ordered = true;
    assert(select() && !mask); active[tag] = request; mask |= 1u << tag;
    assert(!select()); // Younger commands wait for the ordered flush.
    active[tag] = nullptr; mask = 0;
    assert(select()); active[tag] = request; mask |= 1u << tag;
    head->ordered = true; assert(!select()); // Flush waits for older data.
    active[tag] = nullptr; mask = 0;
    assert(select()); active[tag] = request; mask |= 1u << tag;
    active[tag] = nullptr; mask = 0;
    luns[0].depth = 1;
    assert(select()); active[tag] = request; mask |= 1u << tag; assert(!select());
    active[tag] = nullptr; mask = 0; luns[0].depth = 32;
    while (select()) { assert(count < 100); }
    assert(!head && !tail && count == 0);
    assert(UFS::slotMask(32) == UINT32_MAX && UFS::slotMask(31) == 0x7fffffff);
}

static void registerTests() {
    FakeMMIO io; io.regs[UFS::HCE / 4] = 1; io.regs[UFS::HCS / 4] = 0xf;
    io.regs[UFS::UTRLDBR / 4] = UINT32_MAX;
    UFS::Host<FakeMMIO> host(io);
    assert(host.stop() && io.elapsed >= 50 && !io.read(UFS::UTRLDBR));
    assert(io.writes[0].first == UFS::IE && !io.writes[0].second);
    assert(io.writes[1].first == UFS::UTRLRSR && io.writes[2].first == UFS::UTMRLRSR);
    assert(io.writes[3].first == UFS::HCE);
    assert(host.enable());
    uint32_t value;
    assert(host.get(UFS::MaxRXHS, value));
    assert(io.read(UFS::UICARG1) == uint32_t(UFS::MaxRXHS) << 16);
    assert(host.configureTimeouts());
    assert(io.read(UFS::UICARG1) == 0xd0430000 && io.read(UFS::UICARG3) == 32767);
    assert(io.attributes[0x15b0] == 8191 && io.attributes[0x15b4] == 65535);
    io.attributes[0x15aa] = io.attributes[0x115aa] = 1;
    io.attributes[0x15a8] = 3; io.attributes[0x115a8] = 4;
    assert(host.tuneActivation() && io.attributes[0x115a8] == 4);
    io.attributes[0x115a8] = 2;
    assert(host.tuneActivation() && io.attributes[0x115a8] == 4);
    io.attributes[0x15aa] = 6; io.attributes[0x15a8] = 255;
    assert(!host.tuneActivation());
    io.attributes[0x15aa] = 0; assert(!host.tuneActivation());
    assert(host.set(UFS::PowerMode, 0x44, UFS::PowerDone));
    assert(!(io.read(UFS::IS) & (UFS::PowerDone | UFS::UICDone)));
    assert(host.uic(0x17, 0, 0, nullptr, UFS::HibernateEnter));
    assert(host.uic(0x18, 0, 0, nullptr, UFS::HibernateExit));
    io.powerStatus = 5; assert(!host.set(UFS::PowerMode, 0x44, UFS::PowerDone));
    io.uicError = 1; assert(!host.get(UFS::RXGear, value));
    io.uicError = 0; io.uicStuck = true;
    unsigned before = io.elapsed; assert(!host.get(UFS::RXGear, value));
    assert(io.elapsed - before <= 502000);
    io.uicStuck = false; io.regs[UFS::HCE / 4] = 1; io.disableStuck = true;
    before = io.elapsed; assert(!host.stop()); assert(io.read(UFS::HCE) == 1);
    assert(io.elapsed - before <= 100020);
    // PHY register programming is the actual calibrated production table.
    FakeMMIO phy; assert(QcomUFSPhy::start(phy));
    assert(phy.read(QcomUFSPhy::Power) == 1 && phy.read(0xf0) == 0x44);
    assert(phy.read(0x48c) == 6 && phy.read(0x88c) == 6);
    assert(phy.read(0x708) == 0x24 && phy.read(0xb08) == 0x24);
    assert(!phy.read(QcomUFSPhy::Start)); // Host releases PHY reset before start.
}

int main() {
    protocolTests(); dmaTests(); responseTests(); queueTests(); registerTests();
    std::puts("UFS: UPIU/SCSI, 10000 SG cases, 32-slot queue/barriers, errors, PHY, UIC and DMA stop passed");
}
