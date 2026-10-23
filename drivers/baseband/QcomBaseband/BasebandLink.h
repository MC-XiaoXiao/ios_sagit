// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "BasebandFIFO.h"
#include "BasebandMultiplexer.h"

namespace Baseband { namespace GLINK {
constexpr uint32_t ModemFIFOBytes = 16384;
// AP reset/resource admission retires the old producer, but cannot reset its
// cursors. Require an observed empty peer descriptor before accepting queued
// traffic from this boot. Once observed, a newly queued VERSION may precede
// READY. A missed reset window fails closed instead of consuming old frames.
class ColdPeerGate {
    bool fresh = false;
public:
    Result observe(uint32_t txTail, uint32_t rxHead) {
        if (txTail >= ModemFIFOBytes || rxHead >= ModemFIFOBytes || (txTail & 7) || (rxHead & 7)) return Result::Invalid;
        if (!txTail && !rxHead) fresh = true;
        return fresh ? Result::OK : Result::NotReady;
    }
    bool ready() const { return fresh; }
};
// Concrete uncached-SMEM ring backend. Mapping lifetime and generation belong
// to its owning IOKit object; no pointer from a scalar user client reaches here.
// The mailbox action is a retained, kernel-only owner, not an unvalidated MMIO
// address. Binding never resets peer cursors or claims inherited FIFO contents.
class MemoryRing {
    volatile uint32_t *producer = nullptr, *consumer = nullptr;
    volatile uint8_t *data = nullptr;
    void *context = nullptr;
    bool (*notify)(void *) = nullptr;
    uint32_t bytes = 0;
public:
    bool bind(volatile void *storage, uint32_t size, volatile uint32_t *head,
              volatile uint32_t *tail, void *owner, bool (*kick)(void *)) {
        if (data || !storage || !head || !tail || !owner || !kick ||
            size != ModemFIFOBytes || (reinterpret_cast<uintptr_t>(head) & 3) ||
            (reinterpret_cast<uintptr_t>(tail) & 3) || head == tail) return false;
        data = static_cast<volatile uint8_t *>(storage); bytes = size;
        producer = head; consumer = tail; context = owner; notify = kick; return true;
    }
    bool head(uint32_t &v) { if (!producer) return false; v = *producer; return true; }
    bool tail(uint32_t &v) { if (!consumer) return false; v = *consumer; return true; }
    void acquire() {
#if defined(__aarch64__)
        __asm__ volatile("dmb oshld" ::: "memory");
#else
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
#endif
    }
    void release() {
#if defined(__aarch64__)
        __asm__ volatile("dmb oshst" ::: "memory");
#else
        __atomic_thread_fence(__ATOMIC_RELEASE);
#endif
    }
    bool read(uint32_t offset, void *buffer, size_t length) {
        if (!data || !range(offset, length, bytes) || (length && !buffer)) return false;
        auto *p = static_cast<uint8_t *>(buffer);
        for (size_t i = 0; i < length; ++i) p[i] = data[offset + i];
        return true;
    }
    bool write(uint32_t offset, const void *buffer, size_t length) {
        if (!data || !range(offset, length, bytes) || (length && !buffer)) return false;
        const auto *p = static_cast<const uint8_t *>(buffer);
        for (size_t i = 0; i < length; ++i) data[offset + i] = p[i];
        return true;
    }
    bool setHead(uint32_t v) { if (!producer) return false; *producer = v; return true; }
    bool setTail(uint32_t v) { if (!consumer) return false; *consumer = v; return true; }
    bool kick() {
        release();
#if defined(__aarch64__)
        __asm__ volatile("dsb oshst" ::: "memory");
#endif
        return notify && notify(context);
    }
};

// End-to-end stream adapter. It holds an incoming frame until the channel
// accepts it, preserves partially published TX datagrams, and services blocked
// FIFO READ_NOTIF using the reserved control slot. All storage is retained.
template<class Ring> class Link {
    Ring &txMemory, &rxMemory;
    FIFO<Ring> transmit, receive;
    struct Sender {
        FIFO<Ring> &fifo;
        Result send(const void *p, size_t n, bool notification) { return fifo.push(p, n, notification); }
    } sender;
    Multiplexer<Sender> channels;
    uint8_t frameBuffer[ModemFIFOBytes]{};
    uint64_t epoch = 0;
    bool notificationSent = false, pinned = false;
    Result fail(Result r) { pinned = true; return r; }
public:
    Link(Ring &tx, Ring &rx) : txMemory(tx), rxMemory(rx), transmit(tx, ModemFIFOBytes),
        receive(rx, ModemFIFOBytes), sender{transmit}, channels(sender) {}
    bool quarantined() const { return pinned || channels.quarantined() || transmit.quarantined() || receive.quarantined(); }
    bool connected(const char *name = "IPCRTR") const { return !quarantined() && channels.connected(name); }
    bool transmitIdle() const { return channels.transmitIdle(); }
    Result addChannel(const char *name, bool proactive = false) { return channels.add(name, proactive); }
    uint16_t channelIdentity(const char *name = "IPCRTR") const { return channels.identity(name); }
    bool channelClosed(const char *name) const { return channels.closed(name); }
    Result start(uint64_t generation, uint16_t localChannel, uint64_t now, bool freshRemote = false) {
        if (pinned || epoch) return Result::Busy; // A retired link gets a fresh object/mapping.
        if (!generation) return Result::Invalid;
        size_t txBytes = 0, rxBytes = 0;
        auto r = transmit.available(txBytes);
        if (r == Result::OK) r = receive.available(rxBytes);
        if (r != Result::OK) return fail(r);
        // Cold admission must have retired inherited producer/consumer state.
        // This adapter cannot distinguish old remote traffic from a new boot.
        // A freshly authenticated, reset-proven peer may already have queued
        // VERSION/OPEN before asserting READY. Its new traffic must be read,
        // not discarded or mistaken for an inherited-running endpoint.
        if (txBytes || (rxBytes && !freshRemote)) return Result::NotReady;
        r = channels.start(generation, localChannel, now);
        if (r == Result::OK) epoch = generation;
        return r;
    }
    Result pump(uint64_t generation, uint64_t now) {
        if (!epoch || generation != epoch) return Result::Stale;
        if (quarantined()) return Result::Quarantined;
        size_t available = 0;
        auto r = receive.available(available);
        if (r != Result::OK) return fail(r);
        if (available) {
            r = receive.peek(0, frameBuffer, available);
            if (r != Result::OK) return fail(r);
            Frame frame{}; r = parse(frameBuffer, available, frame);
            // The wire header reveals its bounded total length. A producer
            // may publish a prefix; it cannot ever fit a frame >= FIFO size.
            if (r == Result::NotReady && available >= 8) {
                size_t expected = 8;
                const auto cmd = read16(frameBuffer);
                if (cmd == TXData || cmd == TXDataContinue) {
                    if (available >= 16) expected = 16 + read32(frameBuffer + 8);
                } else if (cmd == Open) expected += read32(frameBuffer + 4);
                else if (cmd == Intent) expected += uint64_t(read32(frameBuffer + 4)) * 8;
                if (expected > ModemFIFOBytes - 16) return fail(Result::Invalid);
            } else if (r != Result::OK) return fail(r);
            if (r == Result::OK) {
                r = channels.feed(epoch, frame, now);
                if (r != Result::OK && r != Result::Unsupported && r != Result::Busy && r != Result::NotReady)
                    return fail(r);
                if (r != Result::Busy && r != Result::NotReady) {
                    r = receive.consume(frame.wireBytes);
                    if (r != Result::OK) return fail(r);
                    if (frame.command == ReadNotify && !rxMemory.kick()) return fail(Result::Quarantined);
                }
            }
        }
        r = channels.progress(now);
        if (r == Result::Busy && !notificationSent) {
            uint8_t notify[8]{}; header(notify, ReadNotify, 0, 0);
            const auto sent = transmit.push(notify, sizeof(notify), true);
            if (sent == Result::OK) notificationSent = true;
            else if (sent != Result::Busy) return fail(sent);
        } else if (r == Result::OK) notificationSent = false;
        if (r != Result::OK && r != Result::Busy && r != Result::NotReady) return fail(r);
        return r;
    }
    Result send(uint64_t generation, const void *data, size_t bytes, uint64_t now) {
        if (quarantined()) return Result::Quarantined;
        return channels.queue(generation, "IPCRTR", data, bytes, now);
    }
    Result datagram(uint64_t generation, const uint8_t *&data, size_t &bytes) const {
        if (quarantined()) { data = nullptr; bytes = 0; return Result::Quarantined; }
        return channels.datagram(generation, "IPCRTR", data, bytes);
    }
    Result accept(uint64_t generation) {
        return quarantined() ? Result::Quarantined : channels.accept(generation, "IPCRTR");
    }
    Result sendOn(uint64_t generation, const char *name, const void *data, size_t bytes, uint64_t now) {
        return quarantined() ? Result::Quarantined : channels.queue(generation, name, data, bytes, now);
    }
    Result datagramOn(uint64_t generation, const char *name, const uint8_t *&data, size_t &bytes) const {
        if (quarantined()) { data = nullptr; bytes = 0; return Result::Quarantined; }
        return channels.datagram(generation, name, data, bytes);
    }
    Result acceptOn(uint64_t generation, const char *name) {
        return quarantined() ? Result::Quarantined : channels.accept(generation, name);
    }
    Result openChannel(uint64_t generation, const char *name, uint64_t now) {
        return quarantined() ? Result::Quarantined : channels.open(generation, name, now);
    }
    Result closeChannel(uint64_t generation, const char *name, uint64_t now) {
        return quarantined() ? Result::Quarantined : channels.close(generation, name, now);
    }
};
}} // namespace Baseband::GLINK
