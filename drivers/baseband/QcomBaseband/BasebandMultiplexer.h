// SPDX-License-Identifier: GPL-2.0-only
// One GLINK negotiation and independent named channels; Linux v6.12 wire ABI.
#pragma once
#include "BasebandChannel.h"

namespace Baseband { namespace GLINK {
constexpr unsigned MaxChannelNames = 8, MaxChannelInstances = 16;
// Instances remain until both directions complete CLOSE/CLOSE_ACK. Local
// identifiers are never recycled in a connection; retired ACKs cannot bind a
// newly opened endpoint. A remote may reuse its own ID after our CLOSE_ACK,
// as ordered by the shared FIFO, while the old local ACK is still pending.
template<class TX> class Multiplexer {
    TX &tx;
    struct Registration {
        char name[32]{};
        size_t bytes = 0;
        unsigned current = MaxChannelInstances;
        uint16_t pendingRemote = 0;
        bool proactive = false, started = false, requested = false, closed = false;
    } registrations[MaxChannelNames]{};
    struct Instance { Channel<TX> *channel = nullptr; unsigned registration = 0; } instances[MaxChannelInstances]{};
    unsigned names = 0, cursor = 0;
    uint32_t nextLocal = 0, features = IntentReuse;
    uint64_t epoch = 0, lastTime = 0, deadline = 0;
    bool active = false, failed = false, versionPending = false, ackPending = false;
    bool versionSent = false, negotiated = false, locked = false;
    Result failure = Result::NotReady;
    Result fail(Result r) { failed = true; failure = r; return r; }
    Result clock(uint64_t now) {
        if (failed) return failure;
        if (!active) return Result::NotReady;
        if (now < lastTime) return fail(Result::Invalid);
        lastTime = now;
        return !negotiated && now >= deadline ? fail(Result::Timeout) : Result::OK;
    }
    int find(const char *name) const {
        if (!name) return -1;
        for (unsigned i = 0; i < names; ++i) {
            bool equal = true;
            for (size_t j = 0; j < registrations[i].bytes; ++j)
                if (name[j] != registrations[i].name[j]) { equal = false; break; }
            if (equal) return int(i);
        }
        return -1;
    }
    int find(const Frame &frame) const {
        if (frame.command != Open || !frame.payload) return -1;
        for (unsigned i = 0; i < names; ++i) {
            const auto &r = registrations[i]; if (frame.payloadBytes != r.bytes) continue;
            bool equal = true;
            for (size_t j = 0; j < r.bytes; ++j) equal &= uint8_t(r.name[j]) == frame.payload[j];
            if (equal) return int(i);
        }
        return -1;
    }
    Channel<TX> *current(unsigned index) const {
        return registrations[index].current < MaxChannelInstances ? instances[registrations[index].current].channel : nullptr;
    }
    bool remoteReserved(uint16_t id) const {
        for (unsigned i = 0; i < names; ++i) if (registrations[i].pendingRemote == id) return true;
        for (const auto &i : instances) if (i.channel && i.channel->peerReserved() && i.channel->remoteChannel() == id) return true;
        return false;
    }
    Result create(unsigned index, uint64_t now) {
        auto &r = registrations[index];
        auto *old = current(index);
        if (old && (old->state() != ChannelState::Closed || old->peerReserved())) return Result::Busy;
        if (nextLocal > UINT16_MAX) return Result::NoMemory;
        for (unsigned i = 0; i < MaxChannelInstances; ++i) if (!instances[i].channel) {
            auto *channel = new Channel<TX>(tx, r.name); if (!channel) return Result::NoMemory;
            auto result = channel->startNegotiated(epoch, uint16_t(nextLocal), now, features);
            if (result == Result::OK && r.pendingRemote) {
                const Frame open{Open, r.pendingRemote, uint32_t(r.bytes), 0, 0,
                    reinterpret_cast<const uint8_t *>(r.name), r.bytes, (8 + r.bytes + 7) & ~size_t(7)};
                result = channel->feed(epoch, open, now);
            }
            if (result != Result::OK) { delete channel; return result; }
            ++nextLocal; locked = true;
            instances[i] = {channel, index}; r.current = i;
            r.pendingRemote = 0; r.started = true; r.requested = r.closed = false;
            return Result::OK;
        }
        return Result::NoMemory;
    }
    void reap() {
        for (unsigned i = 0; i < MaxChannelInstances; ++i) if (instances[i].channel && instances[i].channel->retired()) {
            auto &r = registrations[instances[i].registration];
            if (r.current == i) { r.current = MaxChannelInstances; r.closed = true; }
            delete instances[i].channel; instances[i] = {};
        }
    }
public:
    explicit Multiplexer(TX &backend) : tx(backend) { add("IPCRTR", true); }
    Multiplexer(const Multiplexer &) = delete;
    Multiplexer &operator=(const Multiplexer &) = delete;
    ~Multiplexer() { for (auto &i : instances) delete i.channel; }
    Result add(const char *name, bool proactive = false) {
        if (active || epoch || !name || find(name) >= 0 || names == MaxChannelNames) return Result::Invalid;
        size_t bytes = 0;
        while (bytes < 32 && name[bytes]) ++bytes;
        if (!bytes || bytes == 32) return Result::Invalid;
        auto &r = registrations[names++]; r.bytes = bytes + 1; r.proactive = proactive;
        for (size_t i = 0; i <= bytes; ++i) r.name[i] = name[i];
        return Result::OK;
    }
    Result start(uint64_t generation, uint16_t firstLocal, uint64_t now, uint64_t timeout = 5000000) {
        if (epoch || !generation || !firstLocal || !timeout || timeout > UINT64_MAX - now) return Result::Invalid;
        epoch = generation; nextLocal = firstLocal; lastTime = now; deadline = now + timeout;
        features = IntentReuse; active = versionPending = true;
        return Result::OK;
    }
    bool quarantined() const { return failed; }
    bool connected(const char *name = "IPCRTR") const {
        const auto i = find(name); const auto *c = i < 0 ? nullptr : current(unsigned(i));
        return !failed && c && c->state() == ChannelState::Ready;
    }
    uint16_t identity(const char *name = "IPCRTR") const {
        const auto i = find(name); const auto *c = i < 0 ? nullptr : current(unsigned(i));
        return !failed && c && c->state() == ChannelState::Ready ? c->localChannel() : 0;
    }
    bool closed(const char *name) const {
        const auto i = find(name); if (i < 0) return false;
        const auto *c = current(unsigned(i));
        return registrations[i].closed || (c && c->state() == ChannelState::Closed);
    }
    bool transmitIdle(const char *name = "IPCRTR") const {
        const auto i = find(name); const auto *c = i < 0 ? nullptr : current(unsigned(i));
        return !c || c->transmitIdle();
    }
    unsigned instanceCount() const {
        unsigned count = 0; for (const auto &i : instances) count += i.channel != nullptr; return count;
    }
    Result feed(uint64_t generation, const Frame &frame, uint64_t now) {
        if (!epoch || generation != epoch) return Result::Stale;
        auto result = clock(now); if (result != Result::OK) return result;
        if (frame.command == Version || frame.command == VersionAck) {
            if (locked) {
                // Crossed or repeated version traffic with unchanged
                // parameters does not reset channels or any live intents.
                // A changed feature set requires a new connection owner.
                const bool same = frame.command == Version ? (features & frame.argument) == features : frame.argument == features;
                if (frame.channel != 1 || !same)
                    return fail(Result::Stale);
                if (frame.command == Version) ackPending = true;
                return Result::OK;
            }
            if (!frame.channel) return fail(Result::Unsupported);
            if (frame.command == Version) {
                if (frame.channel == 1 && (features & frame.argument) != features) {
                    features &= frame.argument; negotiated = false; versionPending = true;
                }
                ackPending = true; return Result::OK;
            }
            if (!versionSent) return fail(Result::Invalid);
            if (frame.channel == 1 && frame.argument == features) negotiated = true;
            else {
                if (frame.channel == 1) features &= frame.argument;
                negotiated = false; versionPending = true;
            }
            return Result::OK;
        }
        if (frame.command == ReadNotify) return Result::OK;
        if (frame.command == Open) {
            const auto index = find(frame); if (index < 0) return Result::Unsupported;
            if (remoteReserved(frame.channel)) return fail(Result::Invalid);
            auto &r = registrations[index]; auto *c = current(unsigned(index));
            if (r.pendingRemote) return fail(Result::Invalid);
            if (c && c->state() != ChannelState::Closed) {
                if (c->remoteChannel()) return fail(Result::Invalid);
                result = c->feed(epoch, frame, now);
                return result == Result::OK ? result : fail(result);
            }
            if (c && c->peerReserved()) return fail(Result::Invalid);
            // Copy the identity before consuming the frame. OPEN may arrive
            // before the peer's VERSION_ACK; no FIFO head is held for it.
            r.pendingRemote = frame.channel; r.requested = true;
            return Result::OK;
        }
        const bool localID = frame.command == OpenAck || frame.command == CloseAck;
        for (auto &i : instances) if (i.channel && (localID ? i.channel->localChannel() == frame.channel :
            i.channel->peerReserved() && i.channel->remoteChannel() == frame.channel)) {
            result = i.channel->feed(epoch, frame, now);
            if (result == Result::OK || result == Result::Busy || result == Result::NotReady || result == Result::Unsupported) return result;
            return fail(result);
        }
        return Result::Unsupported;
    }
    Result progress(uint64_t now) {
        auto result = clock(now); if (result != Result::OK) return result;
        if (ackPending || versionPending) {
            uint8_t wire[8]; const bool ack = ackPending;
            header(wire, ack ? VersionAck : Version, 1, features);
            result = tx.send(wire, sizeof(wire), false);
            if (result == Result::OK) {
                if (ack) ackPending = false;
                else { versionPending = false; versionSent = true; }
            } else if (result != Result::Busy) return fail(result);
            return result;
        }
        if (!negotiated) return Result::NotReady;
        reap();
        for (unsigned i = 0; i < names; ++i) if (registrations[i].requested ||
            (registrations[i].proactive && !registrations[i].started)) {
            result = create(i, now);
            if (result != Result::OK && result != Result::Busy) return fail(result);
        }
        bool busy = false;
        for (unsigned scan = 0; scan < MaxChannelInstances; ++scan) {
            const unsigned i = (cursor + scan) % MaxChannelInstances;
            if (!instances[i].channel) continue;
            result = instances[i].channel->progress(now);
            if (result == Result::OK) { cursor = (i + 1) % MaxChannelInstances; return result; }
            if (result == Result::Busy) busy = true;
            else if (result != Result::NotReady) return fail(result);
        }
        return busy ? Result::Busy : Result::NotReady;
    }
    Result open(uint64_t generation, const char *name, uint64_t now) {
        if (!epoch || generation != epoch) return Result::Stale;
        auto result = clock(now); if (result != Result::OK) return result;
        const auto index = find(name); if (index < 0) return Result::Unsupported;
        auto *c = current(unsigned(index));
        if (c && (c->state() != ChannelState::Closed || c->peerReserved())) return Result::Busy;
        registrations[index].requested = true; return Result::OK;
    }
    Result close(uint64_t generation, const char *name, uint64_t now) {
        if (!epoch || generation != epoch) return Result::Stale;
        auto result = clock(now); if (result != Result::OK) return result;
        const auto index = find(name); auto *c = index < 0 ? nullptr : current(unsigned(index));
        return c ? c->close(generation, now) : Result::NotReady;
    }
    Result queue(uint64_t generation, const char *name, const void *data, size_t bytes, uint64_t now) {
        if (!epoch || generation != epoch) return Result::Stale;
        auto result = clock(now); if (result != Result::OK) return result;
        const auto index = find(name); auto *c = index < 0 ? nullptr : current(unsigned(index));
        return c ? c->queue(generation, data, bytes, now) : Result::NotReady;
    }
    Result datagram(uint64_t generation, const char *name, const uint8_t *&data, size_t &bytes) const {
        data = nullptr; bytes = 0;
        if (!epoch || generation != epoch) return Result::Stale;
        if (failed) return failure;
        const auto index = find(name); const auto *c = index < 0 ? nullptr : current(unsigned(index));
        return c ? c->datagram(generation, data, bytes) : Result::NotReady;
    }
    Result accept(uint64_t generation, const char *name) {
        if (!epoch || generation != epoch) return Result::Stale;
        if (failed) return failure;
        const auto index = find(name); auto *c = index < 0 ? nullptr : current(unsigned(index));
        return c ? c->accept(generation) : Result::NotReady;
    }
};
}} // namespace Baseband::GLINK
