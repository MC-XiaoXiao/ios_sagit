// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <WLANProtocol.h>

namespace WLAN {
namespace QMI {
constexpr uint32_t Service = 0x45, Version = 1;
constexpr uint16_t IndicationRegister = 0x20, FirmwareReady = 0x21, WLANMode = 0x22,
    WLANConfiguration = 0x23, Capabilities = 0x24, BoardDownload = 0x25,
    MSAReadyIndication = 0x2b, MSAInfo = 0x2d, MSAReady = 0x2e, HostCapabilities = 0x34;
constexpr size_t HeaderBytes = 7, MaximumPayload = 8192;
enum Type : uint8_t { Request = 0, Response = 2, Indication = 4 };
struct Message { Type type; uint16_t transaction, id; const uint8_t *payload; size_t length; };
inline Result parse(const void *buffer, size_t length, Message &out) {
    if (!buffer || length < HeaderBytes) return Result::Invalid;
    const auto *p = static_cast<const uint8_t *>(buffer);
    const size_t bytes = read16(p + 5);
    if ((p[0] != Request && p[0] != Response && p[0] != Indication) || bytes > MaximumPayload ||
        bytes != length - HeaderBytes || (p[0] != Indication && !read16(p + 1))) return Result::Invalid;
    out = {Type(p[0]),read16(p + 1),read16(p + 3),p + HeaderBytes,bytes}; return Result::Success;
}
inline Result encodeRequest(void *buffer, size_t capacity, uint16_t transaction, uint16_t id,
    const void *payload, size_t length, size_t &written) {
    written = 0;
    if (!buffer || !transaction || length > MaximumPayload || (length && !payload) ||
        capacity < HeaderBytes || length > capacity - HeaderBytes) return Result::Invalid;
    auto *p = static_cast<uint8_t *>(buffer);
    const auto *source = static_cast<const uint8_t *>(payload);
    // Permit building the header in front of an overlapping payload. Copy
    // first, in memmove order, so the header never destroys source bytes.
    if (reinterpret_cast<uintptr_t>(p + HeaderBytes) > reinterpret_cast<uintptr_t>(source)) {
        for (size_t i = length; i; --i) p[HeaderBytes + i - 1] = source[i - 1];
    } else for (size_t i = 0; i < length; ++i) p[HeaderBytes + i] = source[i];
    p[0] = Request; write16(p + 1, transaction); write16(p + 3, id); write16(p + 5, uint16_t(length));
    written = HeaderBytes + length; return Result::Success;
}
class TLVs {
    const uint8_t *bytes;
    size_t remaining;
    bool malformed = false;
public:
    explicit TLVs(const Message &message) : bytes(message.payload), remaining(message.length),
        malformed((message.length && !message.payload) || message.length > MaximumPayload) {}
    bool next(uint8_t &type, const uint8_t *&value, uint16_t &length) {
        if (!remaining || malformed) return false;
        if (remaining < 3) { malformed = true; return false; }
        length = read16(bytes + 1);
        if (length > remaining - 3) { malformed = true; return false; }
        type = bytes[0]; value = bytes + 3; bytes += 3 + length; remaining -= 3 + length;
        return true;
    }
    bool valid() const { return !malformed && !remaining; }
};
inline Result responseStatus(const Message &message, uint16_t &error) {
    if (message.type != Response) return Result::Invalid;
    TLVs tlvs(message); uint8_t type; const uint8_t *value; uint16_t length;
    bool found = false; uint16_t status = 0, code = 0;
    while (tlvs.next(type,value,length)) if (type == 2) {
        if (found || length != 4) return Result::Invalid;
        found = true; status = read16(value); code = read16(value + 2);
    }
    if (!tlvs.valid() || !found || status > 1 || (!status && code)) return Result::Invalid;
    error = code; return status ? Result::Fault : Result::Success;
}
// One in-flight firmware transaction. Exhaustion requires a fresh QRTR
// service connection, so a delayed response can never match a wrapped ID.
class Transaction {
    uint64_t generation = 0, deadline = 0;
    uint16_t sequence = 0, pending = 0, messageID = 0;
public:
    bool busy() const { return pending != 0; }
    Result begin(uint64_t epoch, uint16_t id, uint64_t now, uint64_t timeout, uint16_t &transaction) {
        if (pending) return Result::Busy;
        if (!epoch || !timeout || timeout > UINT64_MAX - now) return Result::Invalid;
        if (epoch < generation) return Result::NotReady;
        if (epoch != generation) { generation = epoch; sequence = 0; }
        if (sequence == UINT16_MAX) return Result::NotReady;
        pending = ++sequence; messageID = id; deadline = now + timeout;
        transaction = pending; return Result::Success;
    }
    Result receive(uint64_t epoch, uint64_t now, const Message &message, uint16_t &error) {
        if (!pending || epoch != generation || message.type != Response ||
            message.transaction != pending || message.id != messageID) return Result::NotReady;
        if (now >= deadline) { pending = 0; return Result::Timeout; }
        const Result result = responseStatus(message,error);
        if (result != Result::Invalid) pending = 0;
        return result;
    }
    Result expire(uint64_t epoch, uint64_t now) {
        if (!pending || epoch != generation) return Result::NotReady;
        if (now < deadline) return Result::Busy;
        pending = 0; return Result::Timeout;
    }
    void cancel() { pending = 0; }
};
}

namespace HTC {
constexpr size_t HeaderBytes = 8;
constexpr uint8_t MaximumEndpoints = 9;
struct Packet { uint8_t endpoint; const uint8_t *payload; size_t length; };
inline Result parse(const void *buffer, size_t length, Packet &out) {
    if (!buffer || length < HeaderBytes) return Result::Invalid;
    const auto *p = static_cast<const uint8_t *>(buffer);
    const size_t payload = read16(p + 2);
    if (p[0] >= MaximumEndpoints || payload != length - HeaderBytes) return Result::Invalid;
    // Bundles and the "more block" flag need a separate buffer ownership
    // implementation. Do not silently interpret them as one packet.
    if (p[1] & ~uint8_t(2)) return Result::Unsupported;
    size_t trailer = p[1] & 2 ? p[4] : 0;
    if (trailer > payload) return Result::Invalid;
    const auto *record = p + HeaderBytes + payload - trailer;
    size_t remaining = trailer;
    while (remaining) {
        if (remaining < 2 || record[1] > remaining - 2) return Result::Invalid;
        const size_t bytes = size_t(record[1]) + 2; record += bytes; remaining -= bytes;
    }
    out = {p[0],p + HeaderBytes,payload - trailer}; return Result::Success;
}
}
}
