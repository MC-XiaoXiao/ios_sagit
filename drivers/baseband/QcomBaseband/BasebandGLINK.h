// SPDX-License-Identifier: GPL-2.0-only
// Intent-based modem transport. Linux v6.12 qcom_glink_native.c wire ABI.
#pragma once
#include <BasebandProtocol.h>

namespace Baseband { namespace GLINK {
constexpr uint32_t IntentReuse = 1, MaxDatagram = 65536, MaxIntents = 32;
enum Command : uint16_t {
    Version = 0, VersionAck = 1, Open = 2, Close = 3, OpenAck = 4,
    Intent = 5, RXDone = 6, IntentRequest = 7, IntentRequestAck = 8,
    TXData = 9, CloseAck = 11, TXDataContinue = 12, ReadNotify = 13,
    RXDoneReuse = 14, Signals = 15
};
struct Frame {
    Command command{};
    uint16_t channel = 0;
    uint32_t argument = 0, chunk = 0, remaining = 0;
    const uint8_t *payload = nullptr;
    size_t payloadBytes = 0, wireBytes = 0;
};
// A stream prefix decoder: NotReady consumes nothing and means a partial
// frame. Unknown commands are rejected because their length is unknowable.
inline Result parse(const void *buffer, size_t bytes, Frame &out) {
    out = {};
    if (!buffer) return Result::Invalid;
    if (bytes < 8) return Result::NotReady;
    const auto *p = static_cast<const uint8_t *>(buffer);
    const uint16_t cmd = read16(p), channel = read16(p + 2);
    const uint32_t arg = read32(p + 4);
    size_t header = 8, extra = 0;
    uint32_t chunk = 0, remaining = 0;
    switch (cmd) {
    case Version: case VersionAck: case ReadNotify: break;
    case Open:
        if (!channel || arg < 2 || arg > 32) return Result::Invalid;
        extra = arg; break;
    case Intent:
        if (!channel || !arg || arg > MaxIntents) return Result::Invalid;
        extra = size_t(arg) * 8; break;
    case TXData: case TXDataContinue:
        if (!channel || !arg) return Result::Invalid;
        if (bytes < 16) return Result::NotReady;
        header = 16; chunk = read32(p + 8); remaining = read32(p + 12);
        if (!chunk || chunk > MaxDatagram || remaining > MaxDatagram - chunk)
            return Result::Invalid;
        extra = chunk; break;
    case Close: case OpenAck: case RXDone: case IntentRequest: case IntentRequestAck:
    case CloseAck: case RXDoneReuse: case Signals:
        if (!channel) return Result::Invalid;
        if ((cmd == IntentRequestAck && arg > 1) ||
            (cmd == IntentRequest && (!arg || arg > MaxDatagram)))
            return Result::Invalid;
        break;
    default: return Result::Unsupported;
    }
    const size_t needed = (header + extra + 7) & ~size_t(7);
    if (needed > bytes) return Result::NotReady;
    if (cmd == Open) {
        if (p[header + extra - 1]) return Result::Invalid;
        for (size_t i = 0; i + 1 < extra; ++i)
            if (!p[header + i]) return Result::Invalid;
    }
    out = {Command(cmd), channel, arg, chunk, remaining, p + header, extra, needed};
    return Result::OK;
}
inline void header(void *buffer, Command command, uint16_t channel, uint32_t argument) {
    write16(buffer, command); write16(static_cast<uint8_t *>(buffer) + 2, channel);
    write32(static_cast<uint8_t *>(buffer) + 4, argument);
}

struct RemoteIntent { uint32_t id = 0, bytes = 0; bool busy = false; };
class RemoteIntents {
    RemoteIntent entries[MaxIntents]{};
    uint64_t epoch = 0;
    uint16_t peer = 0;
public:
    Result reset(uint64_t generation, uint16_t channel) {
        if (!generation || generation <= epoch || !channel) return Result::Stale;
        for (auto &entry : entries) entry = {};
        epoch = generation; peer = channel; return Result::OK;
    }
    Result announce(uint64_t generation, const Frame &frame) {
        if (generation != epoch || !epoch) return Result::Stale;
        if (frame.command != Intent || frame.channel != peer || !frame.payload ||
            !frame.argument || frame.argument > MaxIntents ||
            frame.payloadBytes != size_t(frame.argument) * 8) return Result::Invalid;
        unsigned free = 0;
        for (const auto &entry : entries) if (!entry.id) ++free;
        if (frame.argument > free) return Result::NoMemory;
        // Preflight the entire batch. A malicious duplicate never produces
        // a partially installed announcement.
        for (unsigned i = 0; i < frame.argument; ++i) {
            const auto *p = frame.payload + 8 * i;
            const uint32_t bytes = read32(p), id = read32(p + 4);
            if (!id || !bytes || bytes > MaxDatagram) return Result::Invalid;
            for (const auto &entry : entries) if (entry.id == id) return Result::Invalid;
            for (unsigned j = 0; j < i; ++j)
                if (read32(frame.payload + 8 * j + 4) == id) return Result::Invalid;
        }
        for (unsigned i = 0; i < frame.argument; ++i)
            for (auto &entry : entries) if (!entry.id) {
                entry = {read32(frame.payload + 8 * i + 4), read32(frame.payload + 8 * i), false};
                break;
            }
        return Result::OK;
    }
    Result reserve(uint64_t generation, size_t bytes, uint32_t &id) {
        id = 0;
        if (!epoch || generation != epoch) return Result::Stale;
        if (!bytes || bytes > MaxDatagram) return Result::Invalid;
        RemoteIntent *best = nullptr;
        for (auto &entry : entries)
            if (entry.id && !entry.busy && entry.bytes >= bytes && (!best || entry.bytes < best->bytes))
                best = &entry;
        if (!best) return Result::Busy;
        best->busy = true; id = best->id; return Result::OK;
    }
    Result complete(uint64_t generation, const Frame &frame, bool reuseNegotiated) {
        if (!epoch || generation != epoch) return Result::Stale;
        if (frame.channel != peer || (frame.command != RXDone && frame.command != RXDoneReuse) ||
            (frame.command == RXDoneReuse && !reuseNegotiated)) return Result::Invalid;
        for (auto &entry : entries) if (entry.id == frame.argument) {
            if (!entry.busy) return Result::Invalid;
            if (frame.command == RXDoneReuse) entry.busy = false;
            else entry = {};
            return Result::OK;
        }
        return Result::Invalid;
    }
    // Cancel only before any byte of the datagram was published. A partial
    // send keeps the intent busy until RX_DONE or a new remote generation.
    Result cancelUnpublished(uint64_t generation, uint32_t id) {
        if (generation != epoch || !epoch) return Result::Stale;
        for (auto &entry : entries) if (entry.id == id && entry.busy) {
            entry.busy = false; return Result::OK;
        }
        return Result::Invalid;
    }
};

// One locally advertised intent. Storage is provided by the retained kernel
// channel object, not by a stack frame or userspace pointer.
class ReceiveIntent {
    uint8_t *storage = nullptr;
    uint32_t capacity = 0, identifier = 0, used = 0, left = 0;
    uint16_t peer = 0;
    uint64_t epoch = 0;
    bool active = false, finished = false, advertised = false;
public:
    Result bind(void *buffer, uint32_t bytes, uint32_t id, uint16_t channel, uint64_t generation) {
        if (!buffer || !bytes || bytes > MaxDatagram || !id || !channel ||
            !generation || generation <= epoch) return Result::Invalid;
        storage = static_cast<uint8_t *>(buffer); capacity = bytes; identifier = id;
        peer = channel; epoch = generation; used = left = 0;
        active = finished = advertised = false; return Result::OK;
    }
    Result markAdvertised(uint64_t generation) {
        if (!epoch || generation != epoch) return Result::Stale;
        if (active || finished || advertised) return Result::Busy;
        advertised = true; return Result::OK;
    }
    Result receive(uint64_t generation, const Frame &frame) {
        if (!epoch || generation != epoch) return Result::Stale;
        if (!advertised || finished || frame.channel != peer || frame.argument != identifier ||
            (frame.command != TXData && frame.command != TXDataContinue) ||
            !frame.payload || !frame.chunk || frame.payloadBytes != frame.chunk)
            return Result::Invalid;
        if (frame.command == TXData ? active : !active) return Result::Invalid;
        if (frame.chunk > capacity - used || frame.remaining > capacity - used - frame.chunk)
            return Result::Invalid;
        if (active && (frame.chunk > left || frame.remaining != left - frame.chunk))
            return Result::Invalid;
        for (uint32_t i = 0; i < frame.chunk; ++i) storage[used + i] = frame.payload[i];
        used += frame.chunk; left = frame.remaining;
        active = true; finished = !left;
        return Result::OK;
    }
    Result datagram(uint64_t generation, const uint8_t *&buffer, size_t &bytes) const {
        buffer = nullptr; bytes = 0;
        if (!epoch || generation != epoch) return Result::Stale;
        if (!finished) return Result::NotReady;
        buffer = storage; bytes = used; return Result::OK;
    }
    // Call only after the consumer copied/accepted the datagram AND RX_DONE
    // was successfully queued. Busy TX must not release the local intent.
    Result acknowledged(uint64_t generation, bool reuse) {
        if (generation != epoch || !epoch) return Result::Stale;
        if (!finished) return Result::NotReady;
        active = finished = false; used = left = 0; advertised = reuse;
        return Result::OK;
    }
};
}} // namespace Baseband::GLINK
