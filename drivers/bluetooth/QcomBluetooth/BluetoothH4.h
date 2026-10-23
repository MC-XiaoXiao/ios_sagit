// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <BluetoothProtocol.h>

namespace Bluetooth {
enum PacketType : uint8_t { Command = 1, ACL = 2, SCO = 3, Event = 4 };
constexpr uint8_t SleepIndication = 0xfe, WakeIndication = 0xfd, WakeAcknowledgement = 0xfc;
constexpr uint16_t ResetOpcode = 0x0c03;
constexpr uint8_t ResetCommand[4] = {Command, 0x03, 0x0c, 0};

inline size_t headerBytes(uint8_t type) {
    switch (type) {
    case Command: return 4;
    case ACL: return 5;
    case SCO: return 4;
    case Event: return 3;
    default: return 0; // WCN3990 does not implement LE ISO transport
    }
}
inline size_t payloadBytes(const uint8_t *packet) {
    return packet[0] == ACL ? read16(packet + 3) : packet[headerBytes(packet[0]) - 1];
}
inline bool validPacket(const uint8_t *packet, size_t bytes, bool hostToController) {
    if (!packet || !bytes || bytes > MaxH4Frame) return false;
    const size_t header = headerBytes(packet[0]);
    if (!header || bytes < header || (hostToController ? packet[0] == Event : packet[0] == Command)) return false;
    const size_t payload = payloadBytes(packet);
    return (packet[0] != ACL || payload <= MaxACLPayload) && bytes - header == payload;
}

// One instance per controller; serialized by its IOCommandGate. Callback views
// last only for the callback. A framing error poisons the stream until the UART
// has been stopped/flushed: scanning payload for an H4 type is not resync.
class H4Decoder {
    uint8_t frame[MaxH4Frame] = {};
    size_t used = 0, expected = 0;
    bool failed = false, ibs = false;
public:
    void reset(bool enableIBS = false) { used = expected = 0; failed = false; ibs = enableIBS; }
    bool atBoundary() const { return !used; }
    bool poisoned() const { return failed; }
    void enableIBS() { ibs = true; }
    template<class Sink> Result feed(const uint8_t *data, size_t bytes, Sink &sink) {
        if (failed) return Result::Fault;
        if (!data && bytes) return Result::Invalid;
        for (size_t i = 0; i < bytes; ++i) {
            const uint8_t value = data[i];
            if (!used) {
                if (ibs && value >= WakeAcknowledgement && value <= SleepIndication) {
                    const Result result = sink.signal(value);
                    if (result != Result::Success) { failed = true; return result; }
                    continue;
                }
                if (!headerBytes(value) || value == Command) { failed = true; return Result::Invalid; }
                expected = headerBytes(value);
            }
            frame[used++] = value;
            if (used == headerBytes(frame[0])) {
                const size_t payload = payloadBytes(frame);
                if (payload > MaxH4Frame - used || (frame[0] == ACL && payload > MaxACLPayload)) {
                    failed = true; return Result::Invalid;
                }
                expected = used + payload;
            }
            if (used == expected) {
                const Result result = sink.packet(frame,used);
                used = expected = 0;
                if (result != Result::Success) { failed = true; return result; }
            }
        }
        return Result::Success;
    }
};

// The bootstrap HCI Reset requires a real Command Complete, not merely a
// successful Command Status. A timed-out command cannot be reused in the same
// controller generation because HCI events contain no host transaction token.
class ResetTransaction {
    uint64_t generation = 0, deadline = 0;
    bool waiting = false, poisoned = false, used = false;
public:
    void reset(uint64_t epoch) { generation = epoch; waiting = poisoned = used = false; deadline = 0; }
    bool pending() const { return waiting; }
    Result begin(uint64_t epoch, uint64_t now, uint64_t timeout) {
        if (!epoch || epoch != generation) return Result::NotReady;
        if (waiting) return Result::Busy;
        if (poisoned || used) return Result::Fault;
        if (!deadlineAfter(now,timeout,deadline)) return Result::Invalid;
        waiting = used = true; return Result::Success;
    }
    Result poll(uint64_t epoch, uint64_t now) {
        if (!epoch || epoch != generation) return Result::Ignored;
        if (poisoned) return Result::Fault;
        if (waiting && now >= deadline) { waiting = false; poisoned = true; return Result::Timeout; }
        return waiting ? Result::Pending : Result::Ignored;
    }
    Result receive(uint64_t epoch, uint64_t now, const uint8_t *p, size_t bytes) {
        if (!epoch || epoch != generation || !waiting) return Result::Ignored;
        if (poll(epoch,now) == Result::Timeout) return Result::Timeout;
        if (!validPacket(p,bytes,false) || p[0] != Event) return Result::Invalid;
        if (p[1] == 0x0f) {
            if (p[2] != 4) return Result::Invalid;
            if (read16(p + 5) != ResetOpcode) return Result::Ignored;
            if (p[3]) { waiting = false; poisoned = true; return Result::Fault; }
            return Result::Pending;
        }
        if (p[1] != 0x0e) return Result::Ignored;
        if (p[2] < 3) return Result::Invalid;
        if (read16(p + 4) != ResetOpcode) return Result::Ignored;
        if (p[2] != 4) return Result::Invalid;
        waiting = false;
        if (p[6]) { poisoned = true; return Result::Fault; }
        return Result::Success;
    }
};
}
