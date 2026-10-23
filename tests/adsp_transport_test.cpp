// SPDX-License-Identifier: GPL-2.0-only
// Offline peer simulation; no passthrough, physical mappings, UART, or USB.
#include "ADSPGLINK.h"
#include "ADSPOutput.h"
#include "ADSPMemory.h"
#include "../drivers/baseband/QcomBaseband/BasebandFIFO.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include <array>
using namespace ADSP;
using Bytes = std::vector<uint8_t>;

static Bytes glink(uint16_t op, uint16_t channel, uint32_t arg, const Bytes &extra = {}) {
    Bytes b((8 + extra.size() + 7) & ~size_t(7));
    Baseband::GLINK::header(b.data(), Baseband::GLINK::Command(op), channel, arg);
    if (!extra.empty()) memcpy(b.data() + 8, extra.data(), extra.size());
    return b;
}
static Bytes response(uint8_t svc, uint16_t port, uint32_t token, uint32_t op,
                      const Bytes &payload, Message type = Message::Response) {
    Bytes b(APRHeader + payload.size());
    write16(b.data(), 0x50 | unsigned(type) << 8); write16(b.data() + 2, b.size());
    b[4] = b[8] = svc; b[5] = 4; b[9] = 5;
    write16(b.data() + 6, port); write16(b.data() + 10, port);
    write32(b.data() + 12, token); write32(b.data() + 16, op);
    if (!payload.empty()) memcpy(b.data() + 20, payload.data(), payload.size());
    return b;
}
static Bytes words(std::initializer_list<uint32_t> values) {
    Bytes b(values.size() * 4); size_t i = 0;
    for (auto v : values) { write32(b.data() + i, v); i += 4; }
    return b;
}
struct Wire {
    Bytes input;
    std::vector<Bytes> output;
    bool busy = false, ambiguous = false;
    unsigned consumes = 0, kicks = 0;
    Result send(const void *p, size_t n) {
        if (busy) return Result::Busy;
        auto *b = static_cast<const uint8_t *>(p);
        output.emplace_back(b, b + n);
        return ambiguous ? Result::Quarantined : Result::OK;
    }
    Result available(size_t &n) { n = input.size(); return Result::OK; }
    Result peek(void *p, size_t n) { assert(n <= input.size()); memcpy(p, input.data(), n); return Result::OK; }
    Result consume(size_t n) { assert(n <= input.size()); input.erase(input.begin(), input.begin() + n); ++consumes; return kick(); }
    Result kick() { ++kicks; return Result::OK; }
    void feed(const Bytes &b) { input.insert(input.end(), b.begin(), b.end()); }
};
struct Sink {
    std::vector<Bytes> packets;
    Result receive(const void *p, size_t n) {
        auto *b = static_cast<const uint8_t *>(p); packets.emplace_back(b, b + n); return Result::OK;
    }
};
using TestLink = Link<Wire, Sink>;
static void handshake(Wire &w, TestLink &l, bool reuse = true) {
    using namespace Baseband::GLINK;
    assert(l.begin(7) == Result::OK);
    assert(l.poll() == Result::OK);
    assert(read16(w.output.front().data()) == Version);
    w.feed(glink(Version, 1, reuse ? 1 : 0));
    w.feed(glink(VersionAck, 1, reuse ? 1 : 0));
    assert(l.poll() == Result::OK);
    // Feature negotiation can legitimately require a second exchange.
    if (!reuse) { w.feed(glink(VersionAck, 1, 0)); assert(l.poll() == Result::OK); }
    const char name[] = "apr_audio_svc";
    Bytes channel(name, name + sizeof(name));
    w.feed(glink(Open, 9, channel.size(), channel));
    w.feed(glink(OpenAck, 1, 0));
    w.feed(glink(Intent, 9, 4, words({4096, 100, 4096, 101, 4096, 102, 4096, 103})));
    assert(l.poll() == Result::OK && l.ready());
    bool credits = false, open = false;
    for (const auto &p : w.output) {
        open = open || read16(p.data()) == Open;
        if (read16(p.data()) == Intent) {
            assert(read16(p.data() + 2) == 1 && read32(p.data() + 4) == 8);
            credits = true;
        }
    }
    assert(credits && open);
}
static Bytes chunk(uint16_t command, uint32_t id, const Bytes &bytes, size_t offset, size_t n, size_t left) {
    Bytes extra(8 + n); write32(extra.data(), n); write32(extra.data() + 4, left);
    memcpy(extra.data() + 8, bytes.data() + offset, n);
    return glink(command, 9, id, extra);
}
static void linkTests() {
    using namespace Baseband::GLINK;
    Wire w; Sink s; TestLink l(w, s); handshake(w, l);
    const auto packet = response(Core, 0, 1, StateResponse, words({1}));
    w.feed(chunk(TXData, 1, packet, 0, 11, packet.size() - 11));
    assert(l.poll() == Result::OK && s.packets.empty());
    const auto final = chunk(TXDataContinue, 1, packet, 11, packet.size() - 11, 0);
    // Prefix reads never advance an incomplete frame.
    w.feed(Bytes(final.begin(), final.begin() + 12));
    auto consumed = w.consumes;
    assert(l.poll() == Result::OK && w.consumes == consumed && s.packets.empty());
    w.feed(Bytes(final.begin() + 12, final.end()));
    w.busy = true;
    assert(l.poll() == Result::OK && s.packets == std::vector<Bytes>{packet});
    for (unsigned i = 0; i < 10; ++i) assert(l.poll() == Result::OK);
    assert(s.packets.size() == 1); // queued ACK backpressure cannot redeliver data
    w.busy = false;
    assert(l.poll() == Result::OK);
    assert(read16(w.output.back().data()) == RXDoneReuse);
    w.busy = true;
    assert(l.send(packet.data(), packet.size()) == Result::Busy);
    w.busy = false;
    assert(l.send(packet.data(), packet.size()) == Result::OK);
    const auto &sent = w.output.back();
    assert(read16(sent.data()) == TXData && read16(sent.data() + 2) == 1);
    assert(read32(sent.data() + 4) == 100 && read32(sent.data() + 8) == packet.size());
    w.feed(glink(RXDoneReuse, 9, 100)); assert(l.poll() == Result::OK);
    assert(l.send(packet.data(), packet.size()) == Result::OK); // reused credit
    assert(l.close() == Result::OK && !l.ready());
    assert(l.poll() == Result::OK);
    w.feed(glink(CloseAck, 1, 0)); w.feed(glink(Baseband::GLINK::Close, 9, 0));
    assert(l.poll() == Result::OK && l.closed());
}
static void linkFailureTests() {
    using namespace Baseband::GLINK;
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        Wire w; Sink s; TestLink l(w, s); handshake(w, l, false);
        const auto p = response(Core, 0, 1, StateResponse, words({1}));
        if (scenario == 0) w.feed(glink(TXData, 9, 1, words({4097, 0})));
        if (scenario == 1) w.feed(chunk(TXDataContinue, 1, p, 0, p.size(), 0));
        if (scenario == 2) w.feed(glink(RXDoneReuse, 9, 100)); // reuse not negotiated
        if (scenario == 3) w.feed(glink(Baseband::GLINK::Close, 9, 0));
        if (scenario == 4) w.feed(glink(Intent, 9, 2, words({64, 200, 64, 200})));
        assert(l.poll() != Result::OK && l.quarantined());
        auto bytes = w.output.size();
        assert(l.send(p.data(), p.size()) == Result::Quarantined && w.output.size() == bytes);
    }
    Wire w; Sink s; TestLink l(w, s); handshake(w, l, false);
    auto p = response(Core, 0, 1, StateResponse, words({1}));
    w.feed(chunk(TXData, 1, p, 0, p.size(), 0));
    assert(l.poll() == Result::OK && s.packets.size() == 1);
    bool reannounced = false;
    for (auto &packet : w.output)
        reannounced = reannounced || (read16(packet.data()) == Intent && read32(packet.data() + 4) == 1);
    assert(reannounced); // non-reusable intents are explicitly re-advertised
    w.ambiguous = true;
    assert(l.send(p.data(), p.size()) == Result::Quarantined);
    auto bytes = w.output.size();
    assert(l.send(p.data(), p.size()) == Result::Quarantined && w.output.size() == bytes);
}
static void aprTests() {
    std::array<uint8_t, 20> cmd{};
    assert(command(cmd.data(), cmd.size(), Core, 0, 0x11223344, GetState, nullptr, 0) == 20);
    // Independent bytes from the APR/Q6 Core header definitions.
    const std::array<uint8_t, 20> golden{{0x50, 0x02, 0x14, 0x00, 0x03, 0x05, 0x00, 0x00,
        0x03, 0x04, 0x00, 0x00, 0x44, 0x33, 0x22, 0x11, 0x0c, 0x29, 0x01, 0x00}};
    assert(cmd == golden);
    auto bytes = response(ASM, 0x101, 5, BasicResponse, words({Run, 0}));
    Packet p;
    assert(parseAPR(bytes.data(), bytes.size(), p) == Result::OK && p.bytes == 8);
    for (size_t n = 0; n < APRHeader; ++n) assert(parseAPR(bytes.data(), n, p) == Result::Invalid);
    auto bad = bytes; write16(bad.data(), 0xf0); assert(parseAPR(bad.data(), bad.size(), p) == Result::Invalid);
    bad = bytes; bad[5] = 5; assert(parseAPR(bad.data(), bad.size(), p) == Result::Invalid);
    bad = bytes; write16(bad.data() + 2, bytes.size() - 1); assert(parseAPR(bad.data(), bad.size(), p) == Result::Invalid);
    Transaction t;
    assert(t.begin(7, ASM, 0x101, 5, Run, 0) == Result::OK);
    assert(t.begin(7, ASM, 0x101, 6, Run, 0) == Result::Busy);
    assert(parseAPR(bytes.data(), bytes.size(), p) == Result::OK);
    assert(t.receive(6, p) == Result::Stale && !t.done());
    p.token = 4; assert(t.receive(7, p) == Result::Stale && !t.done());
    p.token = 5; p.opcode = Accepted;
    assert(t.receive(7, p) == Result::OK && !t.done());
    p.opcode = BasicResponse;
    assert(t.receive(7, p) == Result::OK && t.done() && t.result() == Result::OK);
    assert(t.begin(7, ASM, 0, 6, Map, MapResponse) == Result::OK);
    bytes = response(ASM, 0, 6, BasicResponse, words({Map, 0}));
    assert(parseAPR(bytes.data(), bytes.size(), p) == Result::OK);
    assert(t.receive(7, p) == Result::OK && !t.done());
    t.timeout();
    bytes = response(ASM, 0, 6, MapResponse, words({99}));
    assert(parseAPR(bytes.data(), bytes.size(), p) == Result::OK);
    assert(t.receive(7, p) == Result::Stale && t.poisoned());
    assert(t.begin(8, ASM, 0, 7, Map, MapResponse) == Result::Quarantined);
}
struct MemoryPipe {
    std::array<uint8_t, 128> data{};
    uint32_t h = 0, t = 0;
    bool kickOK = true;
    unsigned headWrites = 0;
    bool head(uint32_t &n) { n = h; return true; }
    bool tail(uint32_t &n) { n = t; return true; }
    void acquire() {} void release() {}
    bool setHead(uint32_t n) { h = n; ++headWrites; return true; }
    bool setTail(uint32_t n) { t = n; return true; }
    bool kick() { return kickOK; }
    bool write(uint32_t at, const void *p, size_t n) { memcpy(data.data() + at, p, n); return true; }
    bool read(uint32_t at, void *p, size_t n) { memcpy(p, data.data() + at, n); return true; }
};
static void fifoTests() {
    MemoryPipe m; Baseband::FIFO<MemoryPipe> f(m, 128);
    auto packet = glink(Baseband::GLINK::Version, 1, 1);
    m.h = m.t = 120;
    assert(f.push(packet.data(), packet.size()) == Result::OK && m.h == 0);
    std::array<uint8_t, 8> read{};
    assert(f.peek(0, read.data(), read.size()) == Result::OK && !memcmp(packet.data(), read.data(), 8));
    assert(f.consume(8) == Result::OK && m.t == 0);
    m.h = 120; m.t = 0;
    auto writes = m.headWrites;
    assert(f.push(packet.data(), packet.size()) == Result::Busy && m.headWrites == writes);
    // The full producer still has one dedicated READ_NOTIF slot.
    m.h = 112; m.t = 0;
    auto notice = glink(Baseband::GLINK::ReadNotify, 0, 0);
    assert(f.push(notice.data(), notice.size(), true) == Result::OK && m.h == 120);
    m.h = m.t = 0; m.kickOK = false;
    assert(f.push(packet.data(), packet.size()) == Result::Quarantined && m.h == 8);
    assert(f.push(packet.data(), packet.size()) == Result::Quarantined && m.h == 8);
    MemoryPipe invalid; invalid.h = 129;
    Baseband::FIFO<MemoryPipe> g(invalid, 128);
    size_t n = 0;
    assert(g.available(n) == Result::Invalid && g.quarantined());
}
static void memoryAndRestartTests() {
    Bytes memory(32768);
    auto *toc = memory.data() + memory.size() - 4096;
    write32(toc, 0x434f5424); write32(toc + 4, 1); write32(toc + 8, 2);
    write32(toc + 32, 4096); write32(toc + 36, 16384);
    write16(toc + 44, 0); write16(toc + 46, 2); write32(toc + 48, 64);
    write32(toc + 80, 24576); write32(toc + 84, 4096);
    write16(toc + 92, 0xfffe); write16(toc + 94, 0xfffe); write32(toc + 96, 64);
    auto *global = memory.data() + 24576;
    write32(global, 0x54525024); write16(global + 4, 0xfffe); write16(global + 6, 0xfffe);
    write32(global + 8, 4096); write32(global + 16, 4096);
    auto *partition = memory.data() + 4096;
    write32(partition, 0x54525024); write16(partition + 4, 0); write16(partition + 6, 2);
    write32(partition + 8, 16384); write32(partition + 16, 16384);
    const unsigned ids[] = {478, 479, 480, 429}, sizes[] = {32, 1024, 1024, 340};
    size_t position = 32, smpOffset = 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (i == 3) { write32(partition + 12, position); position = 32; }
        unsigned stored = (sizes[i] + 7) & ~7u;
        auto *header = (i == 3 ? global : partition) + position;
        write16(header, 0xa5a5); write16(header + 2, ids[i]); write32(header + 4, stored);
        write16(header + 8, stored - sizes[i]);
        if (i == 3) smpOffset = 24576 + position + 16;
        position += 16 + stored;
    }
    write32(global + 12, position);
    auto *smp = memory.data() + smpOffset;
    write32(smp, 0x504d5324); write32(smp + 4, 0x101);
    write16(smp + 8, 2); write16(smp + 10, 0); write16(smp + 12, 16); write16(smp + 14, 1);
    memcpy(smp + 20, "slave-kernel", 12); write32(smp + 36, 2);
    MemoryLayout layout;
    assert(discoverMemory(memory.data(), memory.size(), layout) == Result::OK);
    assert(layout.descriptor.bytes == 32 && layout.smp.bytes == 340 && layout.smp.offset == smpOffset);
    assert(layout.smpHost == QcomSMEMProtocol::GlobalHost);
    // SMP2P may live in the host partition. Prefer it over the global item,
    // while GLINK must always have a real AP/ADSP private partition.
    const auto uncached = read32(partition + 12);
    memcpy(partition + uncached, global + 32, 360);
    write32(partition + 12, uncached + 360);
    write32(global + 12, 32);
    assert(discoverMemory(memory.data(), memory.size(), layout) == Result::OK);
    assert(layout.smpHost == 2 && layout.smp.offset == 4096 + uncached + 16);
    write32(partition + 12, uncached); write32(global + 12, position);
    auto globalOnly = memory;
    auto *wrongTOC = globalOnly.data() + globalOnly.size() - 4096;
    write16(wrongTOC + 44, 0xfffe); write16(wrongTOC + 46, 0xfffe);
    write16(globalOnly.data() + 4100, 0xfffe); write16(globalOnly.data() + 4102, 0xfffe);
    write16(wrongTOC + 92, 0); write16(wrongTOC + 94, 1);
    write16(globalOnly.data() + 24580, 0); write16(globalOnly.data() + 24582, 1);
    assert(discoverMemory(globalOnly.data(), globalOnly.size(), layout) == Result::Unsupported);
    RemoteState state;
    assert(state.bind(smp, 340) == Result::OK && state.sample(smp, 340) == Result::OK);
    write32(smp + 16, 1); assert(state.sample(smp, 340) == Result::Stale);
    write32(smp + 16, 0); write32(smp + 36, 0);
    assert(state.sample(smp, 340) == Result::NotReady);
    write32(smp + 36, 3); assert(state.sample(smp, 340) == Result::NotReady);
    write32(smp + 36, 2); write16(smp + 14, 17);
    assert(state.sample(smp, 340) == Result::Invalid);
    write16(smp + 14, 2); memcpy(smp + 40, smp + 20, 20);
    assert(state.sample(smp, 340) == Result::Invalid);
    write16(smp + 14, 1); write16(smp + 8, 1);
    assert(state.sample(smp, 340) == Result::Invalid); // modem PID cannot stand in for ADSP
    write16(partition + 34, 999); // descriptor allocation is missing
    assert(discoverMemory(memory.data(), memory.size(), layout) == Result::NotReady);
    write16(partition + 34, 478); write32(partition + 36, UINT32_MAX);
    assert(discoverMemory(memory.data(), memory.size(), layout) == Result::Invalid);
    write32(toc, 0);
    assert(discoverMemory(memory.data(), memory.size(), layout) == Result::Unsupported);
}
static void sharedMemoryPolicyTests() {
    QcomSMEMProtocol::Request r;
    r.operation = QcomSMEMProtocol::Snapshot; r.bytes = QcomSMEMProtocol::Bytes; r.buffer = &r;
    assert(QcomSMEMProtocol::permitted(r, false) && !QcomSMEMProtocol::permitted(r, true));
    for (auto host : {2u, QcomSMEMProtocol::GlobalHost}) {
        r = {}; r.host = host; r.item = 429;
        assert(QcomSMEMProtocol::permitted(r, false) && !QcomSMEMProtocol::permitted(r, true));
        r.operation = QcomSMEMProtocol::Allocate; r.bytes = 340;
        assert(!QcomSMEMProtocol::permitted(r, false)); // ADSP never allocates or resets metadata.
    }
    r = {}; r.host = 2; r.item = 478;
    assert(QcomSMEMProtocol::permitted(r, false) && !QcomSMEMProtocol::permitted(r, true));
    r.host = 1; assert(!QcomSMEMProtocol::permitted(r, false));
    r.host = QcomSMEMProtocol::GlobalHost; assert(!QcomSMEMProtocol::permitted(r, false));
    alignas(8) uint8_t mapping[32]{};
    r = {}; r.mapping = mapping; r.offset = 4144; r.length = 32;
    const Baseband::SMEMItem item{4144, 32, false};
    assert(matchesMapping(r, item));
    r.flags = QcomSMEMProtocol::Cached; assert(!matchesMapping(r, item));
    r.flags = QcomSMEMProtocol::Created; assert(!matchesMapping(r, item));
    r.flags = 0; r.offset += 8; assert(!matchesMapping(r, item));
    r.offset = 4144; r.length = 16; assert(!matchesMapping(r, item));
    r.length = 32; r.mapping = mapping + 1; assert(!matchesMapping(r, item));
    r.mapping = nullptr; assert(!matchesMapping(r, item));
}
static Handoff outputHandoff() {
    return {HandoffMagic, 1, sizeof(Handoff), OutputFlags, 0, 3, 0x8b200000, 0x1000000,
            0x90000000, 0x10000000, 16384, 1, 2, 0x1006, 512};
}
struct DSP {
    struct Sent { uint32_t token; uint64_t address; uint32_t handle, bytes; };
    std::vector<uint32_t> requests;
    std::vector<Sent> sends;
    uint32_t serial = 100, failOpcode = 0;
    bool poison = false, timeout = false, busy = false;
    uint64_t sessionUs = 0, clock = 5000;
    unsigned copies = 0;
    bool uncertain() const { return poison; }
    uint64_t now() { return ++clock; }
    Result token(uint32_t &t) { t = ++serial; return Result::OK; }
    Result request(uint8_t svc, uint16_t port, uint32_t op, uint32_t expected,
                   const void *p, size_t bytes, uint8_t *out, size_t &capacity) {
        requests.push_back(op);
        if (op == failOpcode) { poison = timeout; return timeout ? Result::Timeout : Result::IO; }
        capacity = 0;
        if (op == Map) {
            assert(svc == ASM && !port && bytes == 20 && expected == MapResponse);
            assert(read16(p) == 3 && read16(static_cast<const uint8_t *>(p) + 2) == 1);
            assert(read32(static_cast<const uint8_t *>(p) + 8) == 0x10000000);
            write32(out, 7); capacity = 4;
        } else if (op == OpenWrite) {
            assert(svc == ASM && port == 0x101 && bytes == 16);
            auto *b = static_cast<const uint8_t *>(p);
            assert(read16(b + 6) == 16 && read32(b + 12) == PCMFormat);
        } else if (op == FormatUpdate) {
            auto *b = static_cast<const uint8_t *>(p);
            assert(bytes == 24 && read32(b) == 20 && read32(b + 8) == 48000);
        } else if (op == MatrixMap) {
            auto *b = static_cast<const uint8_t *>(p);
            assert(svc == ADM && !port && bytes == 16 && read16(b + 12) == 2);
        } else if (op == SessionTime) {
            write32(out, 0); write64(out + 4, sessionUs); write64(out + 12, 900000);
            capacity = 20; // hardware rendering time supplied independently of writes/timers
        } else if (op == Unmap) assert(bytes == 4 && read32(p) == 7);
        return Result::OK;
    }
    Result copyPCM(uint64_t from, uint64_t to, size_t n) {
        assert(from < 16384 && to < 16384 && n == 2048 && n <= 16384 - from && n <= 16384 - to);
        ++copies; return Result::OK;
    }
    Result send(uint8_t svc, uint16_t port, uint32_t t, uint32_t op, const void *data, size_t n) {
        assert(svc == ASM && port == 0x101 && op == Write && n == 32);
        if (busy) return Result::Busy;
        auto *p = static_cast<const uint8_t *>(data);
        sends.push_back({t, uint64_t(read32(p)) | uint64_t(read32(p + 4)) << 32, read32(p + 8), read32(p + 12)});
        return Result::OK;
    }
    Packet done(unsigned index, Bytes &storage) {
        const auto &s = sends[index];
        storage = response(ASM, 0x101, s.token, WriteDone,
                           words({uint32_t(s.address), uint32_t(s.address >> 32), s.handle, 0}), Message::Event);
        Packet p; assert(parseAPR(storage.data(), storage.size(), p) == Result::OK); return p;
    }
};
static void outputTests() {
    const auto h = outputHandoff(); assert(validHandoff(h));
    for (unsigned scenario = 0; scenario < 7; ++scenario) {
        auto bad = h;
        if (scenario == 0) bad.flags &= ~DSPMappingRetained;
        if (scenario == 1) bad.pcmPA = h.firmwarePA;
        if (scenario == 2) bad.pcmDSP = UINT64_MAX - 4095;
        if (scenario == 3) bad.pcmBytes = 4096;
        if (scenario == 4) bad.session = 0;
        if (scenario == 5) bad.afePort = 0x1007;
        if (scenario == 6) bad.reserved = 1;
        assert(!validHandoff(bad));
    }
    DSP d; Output o;
    assert(o.prepare(d, h) == Result::OK && o.retained());
    assert(o.start(d, 10) == Result::OK && d.sends.size() == 4);
    AudioOutput::Position pos{};
    // WRITE_DONE allows reuse but does not advance the published sample clock.
    Bytes storage;
    assert(o.completion(d.done(1, storage)) == Result::OK);
    assert(o.position(d, pos) == Result::OK && pos.frames == 0 && d.sends.size() == 4);
    assert(o.completion(d.done(0, storage)) == Result::OK);
    d.sessionUs = 21334;
    assert(o.position(d, pos) == Result::OK && pos.frames == 1024 && pos.generation == 10);
    assert(d.sends.size() == 6);
    auto late = d.done(0, storage);
    assert(o.completion(late) == Result::Stale);
    assert(o.stop(d) == Result::OK && !o.active());
    assert(o.start(d, 10) == Result::Invalid); // output epoch cannot repeat
    assert(o.start(d, 11) == Result::OK);
    assert(o.completion(d.done(0, storage)) == Result::Stale);
    assert(o.release(d) == Result::OK && !o.retained());
    assert(d.requests.back() == Unmap);
    // A rejected OPEN still leaves the successful MAP to be explicitly retired.
    DSP rejected; Output partial; rejected.failOpcode = OpenWrite;
    assert(partial.prepare(rejected, h) == Result::IO && partial.retained());
    assert(partial.release(rejected) == Result::OK && !partial.retained());
    // Lost MAP/UNMAP completion keeps the window and command epoch quarantined.
    DSP uncertain; Output held; uncertain.failOpcode = Map; uncertain.timeout = true;
    assert(held.prepare(uncertain, h) == Result::Timeout && held.retained());
    assert(held.release(uncertain) == Result::Quarantined && held.retained());
    DSP unmap; Output pinned; assert(pinned.prepare(unmap, h) == Result::OK);
    unmap.failOpcode = Unmap; unmap.timeout = true;
    assert(pinned.release(unmap) == Result::Timeout && pinned.retained());
}
static void clockAndDrainTests() {
    auto h = outputHandoff();
    DSP d; Output o;
    assert(o.prepare(d, h) == Result::OK && o.start(d, 1) == Result::OK);
    d.sessionUs = 5000; AudioOutput::Position p{};
    assert(o.position(d, p) == Result::OK && p.frames == 240);
    d.sessionUs = 4000; assert(o.position(d, p) == Result::Invalid);
    d.failOpcode = Flush;
    assert(o.stop(d) == Result::IO && o.active() && o.retained());
    d.failOpcode = 0; assert(o.stop(d) == Result::OK && !o.active());
    assert(o.release(d) == Result::OK);
    DSP forged; Output q; assert(q.prepare(forged, h) == Result::OK && q.start(forged, 1) == Result::OK);
    Bytes b; auto event = forged.done(0, b);
    write32(b.data() + 20 + 8, 99); // wrong memory map handle
    assert(q.completion(event) == Result::Invalid && q.faulted());
    assert(q.stop(forged) == Result::OK && q.release(forged) == Result::OK);
    DSP rejectedWrite; Output rw;
    assert(rw.prepare(rejectedWrite, h) == Result::OK && rw.start(rejectedWrite, 1) == Result::OK);
    auto failure = response(ASM, 0x101, rejectedWrite.sends[0].token, BasicResponse, words({Write, 1}));
    Packet rejected; assert(parseAPR(failure.data(), failure.size(), rejected) == Result::OK);
    assert(rw.completion(rejected) == Result::IO && rw.faulted());
    assert(rw.release(rejectedWrite) == Result::OK);
    DSP backlog; Output r; assert(r.prepare(backlog, h) == Result::OK);
    backlog.busy = true;
    assert(r.start(backlog, 1) == Result::OK && backlog.sends.empty());
    backlog.busy = false;
    assert(r.fill(backlog) == Result::OK && backlog.sends.size() == 4);
    backlog.sessionUs = 1000000; // software wall clock cannot authorize unsubmitted audio
    assert(r.position(backlog, p) == Result::Invalid);
    assert(r.release(backlog) == Result::OK);
}
int main() {
    linkTests(); linkFailureTests(); aprTests(); fifoTests(); memoryAndRestartTests(); sharedMemoryPolicyTests(); outputTests(); clockAndDrainTests();
    puts("ADSP: GLINK/APR peer, fragmentation, credit backpressure, FIFO wrap, ASM lifecycle and DSP rendering clock passed");
}
