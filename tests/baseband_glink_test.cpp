// SPDX-License-Identifier: GPL-2.0-only
#include "BasebandLink.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
using namespace Baseband;
namespace G = Baseband::GLINK;
struct Transport {
    Result next = Result::OK;
    std::vector<std::vector<uint8_t>> sent;
    Result send(const void *p, size_t n, bool) {
        if (next != Result::OK) return next;
        const auto *b = static_cast<const uint8_t *>(p);
        sent.emplace_back(b, b + n);
        sent.back().resize((n + 7) & ~size_t(7), 0);
        return Result::OK;
    }
    G::Frame last() {
        G::Frame f{};
        assert(G::parse(sent.back().data(), sent.back().size(), f) == Result::OK);
        return f;
    }
};
static void channelChecks() {
    G::ColdPeerGate cold;
    assert(cold.observe(8,16)==Result::NotReady && !cold.ready());
    assert(cold.observe(0,16)==Result::NotReady && !cold.ready());
    assert(cold.observe(0,0)==Result::OK && cold.ready());
    assert(cold.observe(0,8)==Result::OK); // Fresh VERSION already queued before READY.
    assert(cold.observe(1,8)==Result::Invalid);
    Transport tx;
    auto channel = std::make_unique<G::Channel<Transport>>(tx);
    uint8_t wire[64]{};
    auto feed = [&](G::Command command, uint16_t cid, uint32_t arg, size_t bytes = 8,
                    uint64_t generation = 1) {
        G::header(wire, command, cid, arg);
        G::Frame f{};
        assert(G::parse(wire, bytes, f) == Result::OK);
        return channel->feed(generation, f, 1);
    };
    assert(channel->start(1, 3, 0) == Result::OK);
    assert(channel->progress(0) == Result::OK && tx.last().command == G::Version);
    assert(feed(G::Version, 1, 1) == Result::OK);
    assert(channel->progress(1) == Result::OK && tx.last().command == G::VersionAck);
    assert(feed(G::VersionAck, 1, 1) == Result::OK);
    tx.next = Result::Busy;
    assert(channel->progress(1) == Result::Busy);
    assert(channel->state() == G::ChannelState::Opening);
    tx.next = Result::OK;
    assert(channel->progress(1) == Result::OK && tx.last().command == G::Open);
    assert(tx.last().channel == 3);
    assert(feed(G::OpenAck, 3, 0) == Result::OK);
    memcpy(wire + 8, "IPCRTR", 7);
    assert(feed(G::Open, 17, 7, 16) == Result::OK);
    assert(channel->progress(1) == Result::OK && tx.last().command == G::OpenAck);
    assert(tx.last().channel == 17); // OPEN_ACK names the remote channel.
    assert(channel->progress(1) == Result::OK && tx.last().command == G::Intent);
    assert(tx.last().channel == 3); // INTENT names our local channel.
    assert(channel->state() == G::ChannelState::Ready);
    std::vector<uint8_t> payload(9000, 0x65);
    assert(channel->queue(1, payload.data(), payload.size(), 1) == Result::Busy);
    assert(channel->progress(1) == Result::OK && tx.last().command == G::IntentRequest);
    assert(feed(G::IntentRequestAck, 17, 1) == Result::OK);
    write32(wire + 8, 16384); write32(wire + 12, 99);
    assert(feed(G::Intent, 17, 1, 16) == Result::OK);
    assert(channel->queue(1, payload.data(), payload.size(), 1) == Result::OK);
    tx.next = Result::Busy;
    assert(channel->progress(1) == Result::Busy && !channel->transmitIdle());
    tx.next = Result::OK;
    assert(channel->progress(1) == Result::OK);
    assert(tx.last().command == G::TXData && tx.last().channel == 3 && tx.last().argument == 99);
    assert(tx.last().chunk == 8192 && tx.last().remaining == 808);
    assert(channel->progress(1) == Result::OK);
    assert(tx.last().command == G::TXDataContinue && tx.last().chunk == 808 && !tx.last().remaining);
    assert(channel->transmitIdle());
    assert(feed(G::RXDoneReuse, 17, 99) == Result::OK);

    write32(wire + 8, 8); write32(wire + 12, 0); memcpy(wire + 16, "response", 8);
    assert(feed(G::TXData, 17, 1, 24) == Result::OK);
    const uint8_t *p; size_t n;
    assert(channel->datagram(1, p, n) == Result::OK && n == 8 && !memcmp(p, "response", 8));
    assert(channel->accept(1) == Result::OK);
    assert(channel->accept(1) == Result::NotReady);
    tx.next = Result::Busy;
    assert(channel->progress(1) == Result::Busy);
    tx.next = Result::OK;
    assert(channel->progress(1) == Result::OK && tx.last().command == G::RXDoneReuse);
    assert(tx.last().channel == 3);
    assert(channel->queue(1, payload.data(), payload.size(), 1) == Result::OK);
    assert(channel->progress(1) == Result::OK); // First chunk reached the peer.
    tx.next = Result::Quarantined;
    assert(channel->progress(1) == Result::Quarantined);
    const auto sends = tx.sent.size();
    tx.next = Result::OK;
    assert(channel->progress(1) == Result::Quarantined && tx.sent.size() == sends);
    assert(channel->start(2, 4, 0) == Result::OK);
    assert(feed(G::RXDoneReuse, 17, 99) == Result::Stale);
    assert(channel->progress(5000000) == Result::Timeout);
    assert(channel->start(3, 4, 0) == Result::OK);
    assert(channel->progress(2) == Result::OK);
    assert(channel->progress(1) == Result::Invalid); // Monotonic clock contract.
}
static void liveRingAdapterChecks() {
    struct Shared {
        alignas(8) uint8_t apToModem[16384]{}, modemToAP[16384]{};
        alignas(8) uint32_t descriptors[8]{};
        unsigned kicks = 0;
        static bool kick(void *owner) { ++static_cast<Shared *>(owner)->kicks; return true; }
    } memory;
    G::MemoryRing atx, arx, btx, brx;
    assert(atx.bind(memory.apToModem, 16384, memory.descriptors + 1, memory.descriptors, &memory, Shared::kick));
    assert(brx.bind(memory.apToModem, 16384, memory.descriptors + 1, memory.descriptors, &memory, Shared::kick));
    assert(btx.bind(memory.modemToAP, 16384, memory.descriptors + 3, memory.descriptors + 2, &memory, Shared::kick));
    assert(arx.bind(memory.modemToAP, 16384, memory.descriptors + 3, memory.descriptors + 2, &memory, Shared::kick));
    auto ap = std::make_unique<G::Link<G::MemoryRing>>(atx, arx);
    auto modem = std::make_unique<G::Link<G::MemoryRing>>(btx, brx);
    assert(modem->start(1, 17, 0) == Result::OK);
    modem->pump(1,0); // A fresh remote may send VERSION before the AP reaches READY.
    assert(ap->start(1,3,0)==Result::NotReady);
    assert(ap->start(1,3,0,true)==Result::OK);
    for (unsigned i = 0; i < 32; ++i) {
        ap->pump(1, i); modem->pump(1, i);
        assert(!ap->quarantined() && !modem->quarantined());
    }
    assert(ap->connected() && modem->connected());
    std::vector<uint8_t> payload(65536);
    for (unsigned i = 0; i < payload.size(); ++i) payload[i] = uint8_t(i * 13);
    assert(ap->send(1, payload.data(), payload.size(), 32) == Result::OK);
    bool blocked = false;
    for (unsigned i = 32; i < 40; ++i) blocked |= ap->pump(1, i) == Result::Busy;
    assert(blocked); // TX fills before the remote consumes any fragment.
    assert(!ap->transmitIdle());
    for (unsigned i = 40; i < 100; ++i) {
        modem->pump(1, i); ap->pump(1, i);
        assert(!ap->quarantined() && !modem->quarantined());
    }
    const uint8_t *p = nullptr; size_t n = 0;
    assert(ap->transmitIdle() && modem->datagram(1, p, n) == Result::OK);
    assert(n == payload.size() && !memcmp(p, payload.data(), n));
    assert(modem->accept(1) == Result::OK);
    for (unsigned i = 100; i < 110; ++i) { modem->pump(1, i); ap->pump(1, i); }
    assert(ap->send(1, payload.data(), 8, 110) == Result::OK); // Reused remote intent.
    assert(ap->pump(0, 110) == Result::Stale && memory.kicks);
}
int main() {
    channelChecks();
    liveRingAdapterChecks();
    uint8_t wire[128]{};
    G::Frame frame{};
    G::header(wire, G::Open, 17, 7); memcpy(wire + 8, "IPCRTR", 7);
    assert(G::parse(wire, 15, frame) == Result::NotReady);
    assert(G::parse(wire, 16, frame) == Result::OK && frame.wireBytes == 16);
    wire[14] = 'x';
    assert(G::parse(wire, 16, frame) == Result::Invalid);
    wire[14] = 0; wire[10] = 0;
    assert(G::parse(wire, 16, frame) == Result::Invalid);
    G::header(wire, G::Intent, 17, UINT32_MAX);
    assert(G::parse(wire, sizeof(wire), frame) == Result::Invalid);
    G::header(wire, G::Intent, 17, 2);
    write32(wire + 8, 64); write32(wire + 12, 10);
    write32(wire + 16, 16); write32(wire + 20, 11);
    assert(G::parse(wire, 24, frame) == Result::OK);
    G::RemoteIntents pool;
    assert(pool.reset(1, 17) == Result::OK);
    assert(pool.announce(0, frame) == Result::Stale);
    write32(wire + 20, 10); // Duplicates fail before either entry is added.
    assert(pool.announce(1, frame) == Result::Invalid);
    uint32_t id;
    assert(pool.reserve(1, 8, id) == Result::Busy);
    write32(wire + 20, 11);
    assert(pool.announce(1, frame) == Result::OK);
    assert(pool.reserve(1, 8, id) == Result::OK && id == 11);
    assert(pool.reserve(1, 8, id) == Result::OK && id == 10);
    assert(pool.reserve(1, 8, id) == Result::Busy);
    assert(pool.cancelUnpublished(1, 10) == Result::OK);
    G::header(wire, G::RXDoneReuse, 17, 11);
    assert(G::parse(wire, 8, frame) == Result::OK);
    assert(pool.complete(1, frame, false) == Result::Invalid);
    assert(pool.complete(1, frame, true) == Result::OK);
    assert(pool.complete(1, frame, true) == Result::Invalid);
    assert(pool.reset(2, 17) == Result::OK);
    assert(pool.complete(1, frame, true) == Result::Stale);
    assert(pool.reserve(2, 1, id) == Result::Busy);

    uint8_t data[64]{};
    G::ReceiveIntent receive;
    assert(receive.bind(data, sizeof(data), 42, 17, 1) == Result::OK);
    assert(receive.markAdvertised(1) == Result::OK);
    auto fragment = [&](G::Command command, uint32_t chunk, uint32_t left) {
        G::header(wire, command, 17, 42);
        write32(wire + 8, chunk); write32(wire + 12, left);
        memcpy(wire + 16, "abcdefgh", 8);
        return G::parse(wire, 24, frame);
    };
    assert(fragment(G::TXDataContinue, 8, 8) == Result::OK);
    assert(receive.receive(1, frame) == Result::Invalid);
    assert(fragment(G::TXData, 8, 8) == Result::OK);
    assert(receive.receive(1, frame) == Result::OK);
    assert(receive.receive(1, frame) == Result::Invalid); // Duplicate first fragment.
    assert(fragment(G::TXDataContinue, 8, 1) == Result::OK);
    assert(receive.receive(1, frame) == Result::Invalid); // Inconsistent total.
    assert(fragment(G::TXDataContinue, 8, 0) == Result::OK);
    assert(receive.receive(1, frame) == Result::OK);
    const uint8_t *out; size_t size;
    assert(receive.datagram(1, out, size) == Result::OK && size == 16);
    assert(!memcmp(out, "abcdefghabcdefgh", 16));
    assert(receive.receive(1, frame) == Result::Invalid); // RX_DONE not yet published.
    assert(receive.acknowledged(1, true) == Result::OK);
    assert(fragment(G::TXData, 8, 0) == Result::OK);
    assert(receive.receive(1, frame) == Result::OK);
    assert(receive.acknowledged(1, false) == Result::OK);
    assert(receive.receive(1, frame) == Result::Invalid); // No reuse without advertisement.
    assert(receive.bind(data, sizeof(data), 42, 17, 2) == Result::OK);
    assert(receive.receive(1, frame) == Result::Stale);
    assert(receive.datagram(2, out, size) == Result::NotReady);
    G::header(wire, G::TXData, 17, 42); write32(wire + 8, UINT32_MAX);
    assert(G::parse(wire, 24, frame) == Result::Invalid);
    for (unsigned i = 0; i < 8; ++i)
        assert(G::parse(wire, i, frame) == Result::NotReady);
    G::header(wire, G::Command(10), 17, 0);
    assert(G::parse(wire, 24, frame) == Result::Unsupported);
    std::puts("baseband GLINK intent/fragment/generation checks passed");
}
