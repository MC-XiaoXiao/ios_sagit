// SPDX-License-Identifier: GPL-2.0-only
// Protocol reference: linux-msm/rmtfs (BSD-3-Clause); see NOTICE.RMTFS.
#pragma once
#include "BasebandQRTRService.h"

namespace Baseband { namespace RMTFS {
constexpr uint32_t Service = 14;
constexpr uint64_t SharedBase = 0x88f00000, SharedBytes = 0x200000;
constexpr size_t SectorBytes = 512, MaxHandles = 10, TransferBytes = 16384;
enum Command : uint16_t { Open = 1, Close, ReadWrite, Allocate, DeviceError, ForceSync };
constexpr uint16_t Internal = 1, Malformed = 2;
struct Partition { const char *path, *label; };
constexpr Partition Partitions[] = {
    {"/boot/modem_fs1", "modemst1"}, {"/boot/modem_fs2", "modemst2"},
    {"/boot/modem_fsc", "fsc"}, {"/boot/modem_fsg", "fsg"}
};
constexpr unsigned PartitionCount = sizeof(Partitions) / sizeof(Partitions[0]);
inline int partition(const uint8_t *path, size_t bytes) {
    if (!path || !bytes || bytes > 256) return -1;
    if (!path[bytes - 1]) --bytes; // Both length-delimited and trailing-NUL strings exist.
    for (unsigned i = 0; i < PartitionCount; ++i) {
        size_t n = 0; while (Partitions[i].path[n]) ++n;
        if (n != bytes) continue;
        bool equal = true;
        for (size_t j = 0; j < n; ++j) equal &= path[j] == uint8_t(Partitions[i].path[j]);
        if (equal) return int(i);
    }
    return -1;
}
struct Fields {
    const uint8_t *value[5]{};
    uint16_t bytes[5]{};
    uint32_t present = 0;
    bool parse(const QMI::Message &message, uint32_t required) {
        QMI::TLVs fields(message.payload, message.bytes);
        uint8_t tag; const uint8_t *p; uint16_t n;
        while (fields.next(tag, p, n)) {
            if (tag >= 1 && tag <= 4) {
                if (present & (1u << tag)) return false;
                present |= 1u << tag; value[tag] = p; bytes[tag] = n;
            } else if (tag < 0x10) return false;
        }
        return fields.valid() && present == required;
    }
};
struct Handle {
    uint32_t id = 0, node = 0;
    unsigned part = 0;
    bool used = false;
    uint8_t error = 0;
};

// Backend methods are real storage/shared-memory operations. The engine never
// invents an empty NV image, ACKs discarded writes, or treats a short I/O as a
// full transfer. Objects live on the heap and are serialized by their owner.
template<class IO> class Engine {
    IO &io;
    uint64_t epoch = 0;
    uint32_t nextID = 0, node = 0, port = 0;
    bool active = false, pending = false, pinned = false;
    Handle handles[MaxHandles]{};
    uint8_t requestBytes[QRTR::ServiceRequestBytes]{}, responseBytes[64]{}, scratch[TransferBytes]{};
    size_t requestSize = 0, responseSize = 0;
    Handle *find(uint32_t peer, uint32_t id) {
        for (auto &h : handles) if (h.used && h.node == peer && h.id == id) return &h;
        return nullptr;
    }
    bool storageResult(Result r, Handle *h) {
        if (r == Result::OK) return true;
        if (h) h->error = 1;
        if (r == Result::Quarantined) pinned = true;
        return false;
    }
    uint16_t execute(uint32_t peer, const QMI::Message &m, uint8_t &extraTag, uint64_t &extra, unsigned &extraBytes) {
        Fields f; extraTag = 0; extra = 0; extraBytes = 0;
        const uint32_t required = m.id == Open || m.id == Close || m.id == DeviceError ? 2u :
            m.id == Allocate ? 6u : m.id == ReadWrite ? 30u : 0u;
        if (!required || !f.parse(m, required)) return Malformed;
        if (m.id == Open) {
            const auto part = partition(f.value[1], f.bytes[1]);
            if (part < 0) return Internal;
            for (const auto &h : handles) if (h.used && h.node == peer && h.part == unsigned(part)) {
                extraTag = 0x10; extra = h.id; extraBytes = 4; return 0;
            }
            if (nextID == UINT32_MAX) return Internal;
            for (auto &h : handles) if (!h.used) {
                const auto r = io.open(unsigned(part));
                if (!storageResult(r, nullptr)) return Internal;
                h = {++nextID, peer, unsigned(part), true, 0};
                extraTag = 0x10; extra = h.id; extraBytes = 4; return 0;
            }
            return Internal;
        }
        if (f.bytes[1] != 4) return Malformed;
        auto *h = find(peer, read32(f.value[1])); if (!h) return Internal;
        if (m.id == Close) {
            const auto r = io.close(h->part);
            if (!storageResult(r, h)) return Internal;
            h->used = false; return 0;
        }
        if (m.id == DeviceError) {
            extraTag = 0x10; extra = h->error; extraBytes = 1; return 0;
        }
        if (m.id == Allocate) {
            if (f.bytes[2] != 4 || !read32(f.value[2])) return Malformed;
            if (read32(f.value[2]) > SharedBytes) return Internal;
            extraTag = 0x10; extra = SharedBase; extraBytes = 8; return 0;
        }
        if (f.bytes[2] != 1 || f.bytes[4] != 1 ||
            !f.bytes[3] || f.bytes[3] != 1 + uint32_t(f.value[3][0]) * 12) return Malformed;
        const bool write = f.value[2][0] != 0;
        const unsigned count = f.value[3][0];
        uint64_t capacity = 0;
        if (!storageResult(io.size(h->part, capacity), h)) return Internal;
        uint64_t total = 0;
        // Validate the entire batch before touching either storage or shared
        // memory. Address and sector multiplication use 64 bits throughout.
        for (unsigned i = 0; i < count; ++i) {
            const auto *entry = f.value[3] + 1 + 12 * i;
            const uint64_t sector = uint64_t(read32(entry)) * SectorBytes;
            const uint64_t physical = read32(entry + 4), bytes = uint64_t(read32(entry + 8)) * SectorBytes;
            if (!bytes || physical < SharedBase || !range(physical - SharedBase, bytes, SharedBytes) ||
                !range(sector, bytes, capacity) || bytes > SharedBytes - total) return Malformed;
            total += bytes;
        }
        bool failed = false;
        for (unsigned i = 0; i < count && !failed; ++i) {
            const auto *entry = f.value[3] + 1 + 12 * i;
            uint64_t sector = uint64_t(read32(entry)) * SectorBytes, physical = read32(entry + 4);
            uint64_t bytes = uint64_t(read32(entry + 8)) * SectorBytes;
            while (bytes) {
                const auto n = size_t(bytes > sizeof(scratch) ? sizeof(scratch) : bytes);
                Result r;
                if (write) {
                    r = io.readShared(physical, scratch, n);
                    if (r == Result::OK) r = io.write(h->part, sector, scratch, n);
                } else {
                    r = io.read(h->part, sector, scratch, n);
                    if (r == Result::OK) r = io.writeShared(physical, scratch, n);
                }
                if (!storageResult(r, h)) { failed = true; break; }
                sector += n; physical += n; bytes -= n;
            }
        }
        // NV write success means persistence. Also flush completed prefixes
        // after a later known I/O failure, unless completion is uncertain.
        // An empty write is also a flush request. A previous known sync
        // failure may have left dirty data even without new vectors here.
        if (write && !pinned)
            if (!storageResult(io.sync(h->part), h)) failed = true;
        return failed || pinned ? Internal : 0;
    }
public:
    explicit Engine(IO &backend) : io(backend) {}
    Result start(uint64_t generation) {
        if (active || pinned || !generation || generation <= epoch) return Result::Invalid;
        if (!io.ready()) return Result::NotReady;
        epoch = generation; active = true; pending = false;
        for (auto &h : handles) h = {};
        return Result::OK;
    }
    bool quarantined() const { return pinned; }
    bool hasReply() const { return pending; }
    Result prepare(uint64_t generation, uint32_t peer, uint32_t endpoint, const QMI::Message &m) {
        if (pinned) return Result::Quarantined;
        if (!active || generation != epoch) return Result::Stale;
        if (peer == QRTR::Broadcast || !endpoint || endpoint >= QRTR::Control || m.type != QMI::Request ||
            m.bytes > sizeof(requestBytes) - QMI::HeaderBytes || (m.bytes && !m.payload)) return Result::Invalid;
        // The receive buffer remains retained while a response is pending.
        // Re-entering due to TX backpressure must not perform I/O again.
        if (pending) {
            bool same = peer == node && endpoint == port && requestSize == QMI::HeaderBytes + m.bytes &&
                read16(requestBytes + 1) == m.transaction && read16(requestBytes + 3) == m.id;
            for (unsigned i = 0; same && i < m.bytes; ++i) same &= requestBytes[7 + i] == m.payload[i];
            return same ? Result::OK : Result::Busy;
        }
        requestBytes[0] = QMI::Request; write16(requestBytes + 1, m.transaction); write16(requestBytes + 3, m.id);
        write16(requestBytes + 5, m.bytes);
        for (unsigned i = 0; i < m.bytes; ++i) requestBytes[7 + i] = m.payload[i];
        requestSize = 7 + m.bytes; QMI::Message input;
        if (QMI::parse(requestBytes, requestSize, input, true) != Result::OK) return Result::Invalid;
        uint8_t tag = 0; uint64_t value = 0; unsigned valueBytes = 0;
        const auto error = execute(peer, input, tag, value, valueBytes);
        if (pinned) return Result::Quarantined;
        uint8_t tlvs[24]{}; tlvs[0] = 2; write16(tlvs + 1, 4);
        write16(tlvs + 3, error ? 1 : 0); write16(tlvs + 5, error);
        size_t payloadBytes = 7;
        if (!error && tag) {
            tlvs[7] = tag; write16(tlvs + 8, uint16_t(valueBytes));
            for (unsigned i = 0; i < valueBytes; ++i) tlvs[10 + i] = uint8_t(value >> (8 * i));
            payloadBytes += 3 + valueBytes;
        }
        auto r = QMI::reply(responseBytes, sizeof(responseBytes), m.transaction, m.id, tlvs, payloadBytes, responseSize);
        if (r == Result::OK) { node = peer; port = endpoint; pending = true; }
        return r;
    }
    Result reply(const uint8_t *&data, size_t &bytes) const {
        data = nullptr; bytes = 0;
        if (pinned) return Result::Quarantined;
        if (!pending) return Result::NotReady;
        data = responseBytes; bytes = responseSize; return Result::OK;
    }
    Result committed(uint64_t generation) {
        if (!active || generation != epoch || pinned) return Result::Stale;
        if (!pending) return Result::NotReady;
        // A later completed transaction may legitimately reuse an ID, even
        // with identical IOVECs and newly written MSS buffer contents.
        pending = false; return Result::OK;
    }
    Result removed(uint64_t generation, const QRTR::PeerRemoved &peer) {
        if (pinned) return Result::Quarantined;
        if (!active || generation != epoch) return Result::Stale;
        if (!peer.valid) return Result::Invalid;
        if (pending && node == peer.node && (peer.wholeNode || port == peer.port)) pending = false;
        // Handles are node-scoped. A single client socket may close while
        // other sockets on the same node still use them. BYE retires the node.
        if (peer.wholeNode) for (auto &h : handles) if (h.used && h.node == peer.node) {
            const auto r = io.close(h.part);
            if (!storageResult(r, &h)) return pinned ? Result::Quarantined : r;
            h.used = false;
        }
        return Result::OK;
    }
    Result resetAfterRemoteStopped() {
        if (pinned) return Result::Quarantined;
        for (auto &h : handles) if (h.used) {
            if (!storageResult(io.close(h.part), &h)) return Result::IO;
            h.used = false;
        }
        pending = active = false; return Result::OK;
    }
};

// Couples server flow control to storage execution. Incoming requests are
// copied by ServiceHost; a full TX FIFO keeps the same prepared response.
template<class Endpoint, class IO> class ServiceEndpoint {
    QRTR::ServiceHost<Endpoint> host;
    Engine<IO> engine;
    uint64_t epoch = 0;
    bool active = false;
public:
    ServiceEndpoint(Endpoint &link, IO &backend) : host(link), engine(backend) {}
    Result start(uint64_t generation, uint32_t node, uint32_t port, uint64_t now) {
        if (active) return Result::Busy;
        if (!generation || node == QRTR::Broadcast || !port || port >= QRTR::Control) return Result::Invalid;
        auto r = engine.start(generation);
        if (r == Result::OK) r = host.start(generation, node, port, Service, 1, 0, now);
        if (r == Result::OK) { epoch = generation; active = true; }
        else if (!engine.quarantined()) engine.resetAfterRemoteStopped(); // No request was admitted.
        return r;
    }
    bool owns(const QRTR::Packet &packet) const { return host.owns(packet); }
    bool published() const { return host.published(); }
    bool quarantined() const { return engine.quarantined(); }
    Result feed(uint64_t generation, const void *data, size_t bytes, uint64_t now) {
        QRTR::PeerRemoved removed;
        auto r = host.feed(generation, data, bytes, now, &removed);
        if (r == Result::OK && removed.valid) r = engine.removed(generation, removed);
        return r;
    }
    Result progress(uint64_t generation, uint64_t now) {
        if (generation != epoch) return Result::Stale;
        auto r = host.progress(generation, now);
        if (r != Result::OK && r != Result::Busy && r != Result::NotReady) return r;
        QRTR::ServiceRequest request;
        r = host.request(generation, request);
        if (r != Result::OK) return r;
        r = engine.prepare(generation, request.node, request.port, request.message);
        if (r != Result::OK) return r;
        const uint8_t *data = nullptr; size_t bytes = 0;
        r = engine.reply(data, bytes);
        if (r == Result::OK) r = host.respond(generation, data, bytes, now);
        if (r == Result::OK) r = engine.committed(generation);
        return r;
    }
    Result stopped() {
        host.disconnect(); const auto r = engine.resetAfterRemoteStopped();
        if (r == Result::OK) active = false;
        return r;
    }
};
}} // namespace Baseband::RMTFS
