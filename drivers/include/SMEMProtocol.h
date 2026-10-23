// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <BasebandProtocol.h>

namespace QcomSMEMProtocol {
constexpr const char *Function = "QcomSMEMRequest";
constexpr uint32_t GlobalHost = 0xfffe;
constexpr uint64_t Base = 0x86000000, Bytes = 0x200000;
enum Operation : uint32_t { Lookup, Allocate, Snapshot };
// Kernel-only synchronous protocol. Callers retain/open QcomSMEM while using
// a mapping, and serialize requests/close with their own runtime work loop.
// No scalar or structure user-client method exposes these addresses.
struct Request {
    uint32_t version = 1, size = sizeof(Request);
    Operation operation = Lookup;
    uint32_t host = GlobalHost, item = 0, bytes = 0, reserved = 0, flags = 0;
    uint64_t offset = 0, length = 0;
    void *buffer = nullptr, *mapping = nullptr;
};
constexpr uint32_t Cached = 1, Created = 2;
inline bool valid(const Request &r) {
    if (r.version != 1 || r.size != sizeof(Request) || r.operation > Snapshot || r.item >= 512 ||
        r.reserved || r.flags || r.offset || r.length || r.mapping ||
        (r.host >= 20 && r.host != GlobalHost) || !r.host) return false;
    if (r.operation == Snapshot) return r.host == GlobalHost && !r.item && r.bytes == Bytes && r.buffer;
    return !r.buffer && (r.operation == Lookup ? !r.bytes : r.item >= 8 && r.bytes && r.bytes <= 65536);
}
inline bool permitted(const Request &r, bool modem) {
    if (!valid(r)) return false;
    if (!modem) {
        return r.operation == Snapshot || (r.operation == Lookup &&
            ((r.host == 2 && r.item >= 478 && r.item <= 480) ||
             ((r.host == 2 || r.host == GlobalHost) && r.item == 429)));
    }
    if (r.operation == Snapshot) return false;
    if (r.operation == Allocate)
        return (r.host == GlobalHost && r.item == 435 && r.bytes == 340) ||
            (r.host == 1 && ((r.item == 478 && r.bytes == 32) || (r.item == 479 && r.bytes == 16384)));
    return (r.host == 1 && r.item >= 478 && r.item <= 480) ||
        (r.host == GlobalHost && (r.item == 428 || r.item == 435));
}
} // namespace QcomSMEMProtocol
