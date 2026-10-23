// SPDX-License-Identifier: GPL-2.0-only
// APR v2 wire protocol; see docs/audio/reference/sources.json.
#pragma once
#include <BasebandProtocol.h>

namespace ADSP {
using Baseband::Result;
using Baseband::read16; using Baseband::read32;
using Baseband::write16; using Baseband::write32; using Baseband::write64;
constexpr unsigned MaxPacket = 4096, APRHeader = 20;
constexpr uint8_t Core = 3, AFE = 4, ASM = 7, ADM = 8;
constexpr uint32_t BasicResponse = 0x110e8, Accepted = 0x100be;
constexpr uint32_t GetState = 0x1290c, StateResponse = 0x1290d;
constexpr uint32_t Map = 0x10d92, MapResponse = 0x10d93, Unmap = 0x10d94;
constexpr uint32_t OpenWrite = 0x10db3, PCMFormat = 0x10da5, FormatUpdate = 0x10d98;
constexpr uint32_t Run = 0x10daa, Pause = 0x10bd3, Flush = 0x10bce, Close = 0x10bcd;
constexpr uint32_t Write = 0x10dab, WriteDone = 0x10d99, MatrixMap = 0x10325;
constexpr uint32_t SessionTime = 0x10d9d, SessionTimeResponse = 0x10d9e;
enum class Message : uint8_t { Event, Response, Command, NonSequential };
struct Packet {
    Message type{};
    uint8_t service = 0;
    uint16_t sourcePort = 0, destinationPort = 0;
    uint32_t token = 0, opcode = 0;
    const uint8_t *payload = nullptr;
    size_t bytes = 0;
};
inline bool service(uint8_t s) { return s == Core || s == AFE || s == ASM || s == ADM; }
inline Result parseAPR(const void *buffer, size_t bytes, Packet &out) {
    out = {};
    if (!buffer || bytes < APRHeader || bytes > MaxPacket) return Result::Invalid;
    const auto *p = static_cast<const uint8_t *>(buffer);
    uint16_t h = read16(p);
    size_t headerBytes = ((h >> 4) & 15) * 4;
    if ((h & 15) > 1 || (h & 0xfc00) || headerBytes < APRHeader || headerBytes > bytes ||
        read16(p + 2) != bytes || p[5] != 4 || p[9] != 5 ||
        !service(p[4]) || p[8] != p[4]) return Result::Invalid;
    out = {Message((h >> 8) & 3), p[4], read16(p + 6), read16(p + 10),
           read32(p + 12), read32(p + 16), p + headerBytes, bytes - headerBytes};
    return Result::OK;
}
inline size_t command(void *buffer, size_t capacity, uint8_t svc, uint16_t port,
                      uint32_t token, uint32_t opcode, const void *payload, size_t bytes) {
    if (!buffer || !service(svc) || !token || !opcode || (bytes && !payload) ||
        bytes > MaxPacket - APRHeader || capacity < APRHeader + bytes) return 0;
    auto *p = static_cast<uint8_t *>(buffer);
    write16(p, 0x250); write16(p + 2, APRHeader + bytes);
    p[4] = svc; p[5] = 5; write16(p + 6, port);
    p[8] = svc; p[9] = 4; write16(p + 10, port);
    write32(p + 12, token); write32(p + 16, opcode);
    const auto *q = static_cast<const uint8_t *>(payload);
    for (size_t i = 0; i < bytes; ++i) p[APRHeader + i] = q[i];
    return APRHeader + bytes;
}

// Exactly one synchronous command; data completions are routed separately.
// A timed-out published command poisons the epoch: even a late successful
// reply cannot authorize freeing memory or reissuing the command.
class Transaction {
    uint64_t epoch_ = 0;
    uint32_t token_ = 0, command_ = 0, response_ = 0;
    uint16_t port_ = 0;
    uint8_t service_ = 0;
    bool pending_ = false, poisoned_ = false, done_ = false;
    Result result_ = Result::NotReady;
    uint8_t data_[64]{};
    size_t bytes_ = 0;
public:
    bool pending() const { return pending_; }
    bool done() const { return done_; }
    bool poisoned() const { return poisoned_; }
    Result result() const { return result_; }
    const uint8_t *data() const { return data_; }
    size_t bytes() const { return bytes_; }
    Result begin(uint64_t epoch, uint8_t svc, uint16_t port, uint32_t token,
                 uint32_t opcode, uint32_t response) {
        if (poisoned_) return Result::Quarantined;
        if (pending_) return Result::Busy;
        if (!epoch || !service(svc) || !token || !opcode) return Result::Invalid;
        epoch_ = epoch; service_ = svc; port_ = port; token_ = token;
        command_ = opcode; response_ = response; pending_ = true;
        done_ = false; result_ = Result::NotReady; bytes_ = 0;
        return Result::OK;
    }
    void unpublished() { pending_ = false; } // only if no GLINK head was published
    void fault() { pending_ = false; poisoned_ = done_ = true; result_ = Result::Quarantined; }
    void timeout() { fault(); result_ = Result::Timeout; }
    Result receive(uint64_t epoch, const Packet &p) {
        if (!pending_ || epoch != epoch_ || p.token != token_ || p.service != service_ ||
            p.destinationPort != port_ || p.sourcePort != port_) return Result::Stale;
        if (p.type != Message::Response) return Result::Stale;
        if (p.opcode == Accepted) return Result::OK; // acceptance is not completion
        if (p.opcode == BasicResponse) {
            if (p.bytes != 8 || read32(p.payload) != command_) return Result::Invalid;
            const auto status = read32(p.payload + 4);
            if (!status && response_) return Result::OK; // wait for typed response
            result_ = status ? Result::IO : Result::OK;
        } else {
            if (!response_ || p.opcode != response_) return Result::Stale;
            if (!p.bytes || p.bytes > sizeof(data_)) return Result::Invalid;
            bytes_ = p.bytes;
            for (size_t i = 0; i < p.bytes; ++i) data_[i] = p.payload[i];
            result_ = Result::OK;
        }
        pending_ = false; done_ = true;
        return Result::OK;
    }
};

constexpr uint64_t SMEMBase = 0x86000000, SMEMBytes = 0x200000;
constexpr uint64_t MailboxBase = 0x17911000, MailboxBytes = 0x1000;
constexpr uint64_t MutexBase = 0x01f40000, MutexBytes = 0x20000;
constexpr uint32_t HandoffMagic = 0x50534441; // 'ADSP', little-endian
enum HandoffFlags : uint32_t {
    FirmwareRunning = 1, ExclusiveFreshEndpoint = 2, SMEMUncached = 4, PowerRetained = 8,
    PCMReserved = 16, DSPMappingRetained = 32, CalibratedRouteRetained = 64
};
constexpr uint32_t TransportFlags = 15, OutputFlags = 127;
struct Handoff {
    uint32_t magic;
    uint16_t version, bytes;
    uint32_t flags, reserved;
    uint64_t generation, firmwarePA, firmwareBytes, pcmPA, pcmDSP;
    uint32_t pcmBytes;
    uint16_t session, copp;
    uint32_t afePort, latencyFrames;
};
static_assert(sizeof(Handoff) == 72, "ADSP retained handoff ABI");
inline bool validHandoff(const Handoff &h) {
    if (h.magic != HandoffMagic || h.version != 1 || h.bytes != sizeof(h) || h.reserved ||
        !h.generation || (h.flags != TransportFlags && h.flags != OutputFlags) ||
        h.firmwarePA < 0x80000000 || (h.firmwarePA & 4095) || !h.firmwareBytes || (h.firmwareBytes & 4095) ||
        h.firmwareBytes > 64 * 1024 * 1024 ||
        !Baseband::range(h.firmwarePA, h.firmwareBytes, uint64_t(1) << 32) ||
        Baseband::overlaps(h.firmwarePA, h.firmwareBytes, SMEMBase, SMEMBytes)) return false;
    if (h.flags == TransportFlags)
        return !h.pcmPA && !h.pcmDSP && !h.pcmBytes && !h.session && !h.copp && !h.afePort && !h.latencyFrames;
    return h.pcmPA >= 0x80000000 && !(h.pcmPA & 4095) && h.pcmDSP && !(h.pcmDSP & 4095) &&
        h.pcmBytes == 16384 && Baseband::range(h.pcmPA, h.pcmBytes, uint64_t(1) << 32) &&
        Baseband::range(h.pcmDSP, h.pcmBytes, uint64_t(1) << 32) &&
        !Baseband::overlaps(h.pcmPA, h.pcmBytes, h.firmwarePA, h.firmwareBytes) &&
        !Baseband::overlaps(h.pcmPA, h.pcmBytes, SMEMBase, SMEMBytes) &&
        h.session >= 1 && h.session <= 8 && h.copp < 99 && h.afePort == 0x1006 &&
        h.latencyFrames <= 48000;
}
}
