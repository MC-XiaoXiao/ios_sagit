// SPDX-License-Identifier: GPL-2.0-only
// Qualcomm service QMI over QRTR, with bounded TLV and response correlation.
#pragma once
#include <BasebandProtocol.h>

namespace Baseband { namespace QMI {
constexpr uint8_t Request = 0, Response = 2, Indication = 4;
constexpr size_t HeaderBytes = 7, MaxPayload = 65536 - 32 - HeaderBytes;
constexpr uint32_t WDS = 1, DMS = 2, NAS = 3, UIM = 11;
struct Message {
    uint8_t type = 0;
    uint16_t transaction = 0, id = 0, bytes = 0;
    const uint8_t *payload = nullptr;
};
class TLVs {
    const uint8_t *data;
    size_t bytes, at = 0;
    bool failed = false;
public:
    TLVs(const void *buffer, size_t length) : data(static_cast<const uint8_t *>(buffer)), bytes(length) {
        failed = length && !buffer;
    }
    bool next(uint8_t &tag, const uint8_t *&value, uint16_t &length) {
        if (failed || at == bytes) return false;
        if (!range(at, 3, bytes)) { failed = true; return false; }
        tag = data[at]; length = read16(data + at + 1); at += 3;
        if (!range(at, length, bytes)) { failed = true; return false; }
        value = data + at; at += length; return true;
    }
    bool valid() const { return !failed && at == bytes; }
};
inline Result parse(const void *buffer, size_t length, Message &out, bool allowZeroTransaction = false) {
    out = {};
    if (!buffer || length < HeaderBytes || length - HeaderBytes > MaxPayload) return Result::Invalid;
    const auto *p = static_cast<const uint8_t *>(buffer);
    if (p[0] != Request && p[0] != Response && p[0] != Indication) return Result::Invalid;
    const auto transaction = read16(p + 1), bytes = read16(p + 5);
    if (bytes != length - HeaderBytes || (p[0] == Indication ? transaction != 0 : !transaction && !allowZeroTransaction))
        return Result::Invalid;
    TLVs fields(p + HeaderBytes, bytes); uint8_t tag; const uint8_t *value; uint16_t size;
    while (fields.next(tag, value, size)) {}
    if (!fields.valid()) return Result::Invalid;
    out = {p[0], transaction, read16(p + 3), bytes, p + HeaderBytes}; return Result::OK;
}
inline Result request(void *buffer, size_t capacity, uint16_t transaction, uint16_t id,
                      const void *payload, size_t bytes, size_t &written) {
    written = 0;
    if (!buffer || !transaction || bytes > MaxPayload || capacity < HeaderBytes ||
        bytes > capacity - HeaderBytes || (bytes && !payload)) return Result::Invalid;
    auto *p = static_cast<uint8_t *>(buffer);
    p[0] = Request; write16(p + 1, transaction); write16(p + 3, id); write16(p + 5, uint16_t(bytes));
    const auto *source = static_cast<const uint8_t *>(payload);
    for (size_t i = 0; i < bytes; ++i) p[HeaderBytes + i] = source[i];
    Message validated;
    const auto r = parse(buffer, HeaderBytes + bytes, validated);
    if (r == Result::OK) written = HeaderBytes + bytes;
    return r;
}
inline Result reply(void *buffer, size_t capacity, uint16_t transaction, uint16_t id,
                    const void *payload, size_t bytes, size_t &written) {
    written = 0;
    if (!buffer || bytes > MaxPayload || capacity < HeaderBytes || bytes > capacity - HeaderBytes ||
        (bytes && !payload)) return Result::Invalid;
    auto *p = static_cast<uint8_t *>(buffer);
    p[0] = Response; write16(p + 1, transaction); write16(p + 3, id); write16(p + 5, uint16_t(bytes));
    for (size_t i = 0; i < bytes; ++i) p[HeaderBytes + i] = static_cast<const uint8_t *>(payload)[i];
    Message decoded;
    const auto r = parse(p, HeaderBytes + bytes, decoded, true);
    if (r == Result::OK) written = HeaderBytes + bytes;
    return r;
}
inline Result response(const Message &message, uint16_t &error) {
    error = 0;
    if (message.type != Response) return Result::Invalid;
    TLVs fields(message.payload, message.bytes); uint8_t tag; const uint8_t *value; uint16_t size;
    bool found = false; uint16_t status = 0;
    while (fields.next(tag, value, size)) if (tag == 2) {
        if (found || size != 4) return Result::Invalid;
        found = true; status = read16(value); error = read16(value + 2);
    }
    if (!fields.valid() || !found || status > 1 || (status == 0 && error) || (status == 1 && !error))
        return Result::Invalid;
    return status ? Result::IO : Result::OK; // Preserve the actual QMI error separately.
}
}} // namespace Baseband::QMI
