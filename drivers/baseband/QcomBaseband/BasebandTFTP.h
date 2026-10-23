// SPDX-License-Identifier: GPL-2.0-only
// TFTP over QRTR, service 4096 v1/0. See NOTICE.TFTP and pinned tqftpserv.
#pragma once
#include "BasebandQRTR.h"
#include "BasebandTFTPFiles.h"

namespace Baseband { namespace TFTP {
constexpr uint32_t Service = 4096;
constexpr size_t MaxBlock = 8192, MaxWindow = 16, MaxSessions = 8, RequestBytes = 1024;
constexpr unsigned RetryLimit = 5;
enum Opcode : uint16_t { RRQ = 1, WRQ, DATA, ACK, ERROR, OACK };
enum Error : uint16_t { Undefined = 0, NotFound, Access, NoSpace, BadOperation, BadTransfer,
                        Exists, NoUser, BadOptions, EndOfTransfer };
inline uint16_t be16(const uint8_t *p) { return uint16_t(uint16_t(p[0]) << 8 | p[1]); }
inline void putBE16(uint8_t *p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
enum Option : uint32_t { Block = 1, Timeout = 2, TransferSize = 4, Window = 8, ReadSize = 16, Seek = 32 };
struct Options {
    uint32_t mask = 0, block = 512, timeoutMS = 1000, window = 1;
    uint64_t transfer = 0, read = 0, seek = 0;
};
struct Request { char path[PathBytes]{}; bool write = false; Options options; };
inline bool equalFold(const uint8_t *p, size_t n, const char *text) {
    size_t i = 0;
    for (; i < n && text[i]; ++i) {
        uint8_t c = p[i]; if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != uint8_t(text[i])) return false;
    }
    return i == n && !text[i];
}
inline bool number(const uint8_t *p, size_t n, uint64_t &out) {
    out = 0;
    if (!n || n > 20) return false;
    for (size_t i = 0; i < n; ++i) {
        if (p[i] < '0' || p[i] > '9' || out > (UINT64_MAX - (p[i] - '0')) / 10) return false;
        out = out * 10 + p[i] - '0';
    }
    return true;
}
inline uint16_t parseRequest(const void *data, size_t bytes, Request &out) {
    out = {};
    if (!data || bytes < 4 || bytes > RequestBytes) return BadOperation;
    const auto *p = static_cast<const uint8_t *>(data);
    const auto op = be16(p); if (op != RRQ && op != WRQ) return BadOperation;
    out.write = op == WRQ;
    size_t cursor = 2, n = 0;
    while (cursor + n < bytes && p[cursor + n]) ++n;
    if (!n || n >= PathBytes || cursor + n == bytes) return BadOperation;
    for (size_t i = 0; i < n; ++i) out.path[i] = char(p[cursor + i]);
    bool readOnly = false;
    if (!pathValid(out.path, readOnly) || (out.write && readOnly)) return Access;
    cursor += n + 1; n = 0;
    while (cursor + n < bytes && p[cursor + n]) ++n;
    if (cursor + n == bytes || !equalFold(p + cursor, n, "octet")) return BadOperation;
    cursor += n + 1;
    while (cursor < bytes) {
        const size_t key = cursor;
        while (cursor < bytes && p[cursor]) ++cursor;
        const size_t length = cursor - key;
        if (!length || cursor == bytes) return BadOptions;
        const size_t value = ++cursor;
        while (cursor < bytes && p[cursor]) ++cursor;
        if (cursor == bytes || cursor == value) return BadOptions;
        uint32_t bit = 0;
        if (equalFold(p + key, length, "blksize")) bit = Block;
        else if (equalFold(p + key, length, "timeoutms")) bit = Timeout;
        else if (equalFold(p + key, length, "tsize")) bit = TransferSize;
        else if (equalFold(p + key, length, "wsize")) bit = Window;
        else if (equalFold(p + key, length, "rsize")) bit = ReadSize;
        else if (equalFold(p + key, length, "seek")) bit = Seek;
        if (bit) {
            uint64_t v = 0;
            if ((out.options.mask & bit) || !number(p + value, cursor - value, v)) return BadOptions;
            out.options.mask |= bit;
            switch (bit) {
            case Block:
                if (v < 8 || v > 65464) return BadOptions;
                out.options.block = uint32_t(v > MaxBlock ? MaxBlock : v); break;
            case Timeout:
                if (!v || v > 255000) return BadOptions;
                out.options.timeoutMS = uint32_t(v); break;
            case TransferSize:
                if ((!out.write && v) || (out.write && v > TemporaryBytes)) return BadOptions;
                out.options.transfer = v; break;
            case Window:
                if (!v || v > 65535) return BadOptions;
                out.options.window = uint32_t(v > MaxWindow ? MaxWindow : v); break;
            case ReadSize:
                if (out.write || !v || v > 100 * 1024 * 1024) return BadOptions;
                out.options.read = v; break;
            case Seek: out.options.seek = v; break;
            }
        }
        ++cursor; // Unknown options are omitted from OACK, per RFC 2347.
    }
    return 0;
}
inline size_t appendOption(uint8_t *data, size_t offset, const char *name, uint64_t value) {
    while (*name) data[offset++] = uint8_t(*name++);
    data[offset++] = 0;
    char digits[20]; unsigned n = 0;
    do { digits[n++] = char('0' + value % 10); value /= 10; } while (value);
    while (n) data[offset++] = uint8_t(digits[--n]);
    data[offset++] = 0; return offset;
}
inline const char *errorText(uint16_t code) {
    switch (code) {
    case NotFound: return "File not found";
    case Access: return "Access violation";
    case NoSpace: return "Allocation exceeded";
    case BadOperation: return "Illegal TFTP operation";
    case BadTransfer: return "Unknown transfer ID";
    case BadOptions: return "Option negotiation failed";
    default: return "Transfer failed";
    }
}
inline size_t makeError(uint8_t *out, uint16_t code) {
    putBE16(out, ERROR); putBE16(out + 2, code);
    const char *text = errorText(code); size_t n = 4;
    do { out[n++] = uint8_t(*text); } while (*text++);
    return n;
}
enum class Phase { Reading, Writing, Dally, Failing, Retiring };

// Link::send copies a whole QRTR datagram. Busy never commits a publication;
// Quarantined forbids retries. Actual file writes happen once, before ACK.
template<class Endpoint> class Server {
    Endpoint &link;
    Files &files;
    QRTR::PortAllocator ownedPorts;
    QRTR::PortAllocator &ports;
    uint64_t epoch = 0, lastTime = 0;
    uint32_t local = 1, listener = 0;
    bool active = false, announce = false, announced = false;
    unsigned cursor = 0;
    struct Session {
        uint32_t node = 0, peer = 0, port = 0, handle = 0;
        Phase phase = Phase::Reading;
        Options options;
        uint64_t total = 0, timeout = 1000000, deadline = 0, expires = 0;
        uint32_t lastAck = 0, lastSent = 0, nextRead = 1, windowEnd = 1, finalBlock = 1;
        uint32_t expected = 1, lastWritten = 0, inWindow = 0;
        size_t requestSize = 0, controlSize = 0, priorSize = 0;
        unsigned retries = 0;
        bool writer = false, awaitZero = false, initialPublished = false;
        bool controlPending = false, finalWrite = false, initialOACK = false;
        uint8_t request[RequestBytes]{}, control[256]{}, priorData[MaxBlock]{};
    };
    Session *sessions[MaxSessions]{};
    struct Resume { uint32_t node = 0, peer = 0, port = 0; bool used = false; } resumes[32];
    QRTR::Flow flows[32]{};
    uint8_t wire[MaxBlock + 36]{}, payload[MaxBlock + 4]{};
    struct ErrorReply {
        uint32_t node = 0, peer = 0, port = 0;
        size_t bytes = 0;
        uint8_t payload[128]{};
        bool pending = false;
    } errorReply;
    uint32_t completedCount = 0, timeoutCount = 0, rejectedCount = 0;
    Result clock(uint64_t now) {
        if (now < lastTime) { active = false; return Result::Stale; }
        lastTime = now; return Result::OK;
    }
    Result closeFile(Session &s) {
        if (!s.handle) return Result::OK;
        const auto r = files.close(s.handle); s.handle = 0;
        if (r != Result::OK) { active = false; return Result::Quarantined; }
        return Result::OK;
    }
    Result retire(Session &s) {
        const auto r = closeFile(s); s.phase = Phase::Retiring; s.controlPending = false; return r;
    }
    QRTR::Flow *flow(uint32_t node, uint32_t port, bool create) {
        QRTR::Flow *empty = nullptr;
        for (auto &f : flows) {
            if (f.used && f.node == node && f.port == port) return &f;
            if (!f.used && !empty) empty = &f;
        }
        if (!create || !empty) return nullptr;
        *empty = {node, port, 0, true}; return empty;
    }
    bool resumeAvailable(const QRTR::Packet &p) const {
        for (const auto &r : resumes) if (!r.used ||
            (r.node == p.sourceNode && r.peer == p.sourcePort && r.port == p.destinationPort)) return true;
        return false;
    }
    void addResume(const QRTR::Packet &p) {
        for (const auto &r : resumes) if (r.used && r.node == p.sourceNode && r.peer == p.sourcePort && r.port == p.destinationPort) return;
        for (auto &r : resumes) if (!r.used) { r = {p.sourceNode, p.sourcePort, p.destinationPort, true}; return; }
    }
    Result send(uint32_t type, uint32_t port, uint32_t node, uint32_t peer,
                const void *data, size_t bytes, uint64_t now) {
        QRTR::Flow *f = nullptr;
        if (type == QRTR::Data) {
            f = flow(node, peer, true); if (!f) return Result::Busy;
            if (f->pending >= 10) return Result::Busy;
        }
        size_t n = 0;
        auto r = QRTR::encode(wire, sizeof(wire), {type, local, port, node, peer,
            f && f->pending + 1 == 5, static_cast<const uint8_t *>(data), uint32_t(bytes)}, n);
        if (r == Result::OK) r = link.send(epoch, wire, n, now);
        if (r == Result::OK && f) ++f->pending;
        if (r == Result::Quarantined) active = false;
        return r;
    }
    Result reject(uint32_t node, uint32_t peer, uint32_t port, uint16_t code) {
        // ERROR packets are best effort. Retaining another invalid request
        // here could hide the very QRTR RESUME needed by the queued ERROR.
        if (errorReply.pending) { ++rejectedCount; return Result::OK; }
        errorReply.node = node; errorReply.peer = peer; errorReply.port = port;
        errorReply.bytes = makeError(errorReply.payload, code); errorReply.pending = true;
        ++rejectedCount; return Result::OK;
    }
    Result fail(Session &s, uint16_t code) {
        const auto r = closeFile(s); if (r != Result::OK) return r;
        s.controlSize = makeError(s.control, code); s.controlPending = true;
        s.phase = Phase::Failing; ++rejectedCount; return Result::OK;
    }
    void ack(Session &s) {
        if (s.controlPending) return;
        if (!(s.initialOACK && !s.lastWritten)) {
            putBE16(s.control, ACK); putBE16(s.control + 2, uint16_t(s.lastWritten)); s.controlSize = 4;
        }
        s.controlPending = true;
    }
    Result begin(const QRTR::Packet &p, uint64_t now) {
        if (p.bytes >= 2 && be16(p.payload) == ERROR) return Result::OK;
        if (p.bytes > RequestBytes) return reject(p.sourceNode, p.sourcePort, listener, BadOperation);
        // A retransmitted request must retain the same transfer port and must
        // not truncate/write/open its file for a second time.
        for (auto *s : sessions) if (s && s->node == p.sourceNode && s->peer == p.sourcePort &&
            s->phase != Phase::Dally && s->phase != Phase::Retiring) {
            bool identical = s->requestSize == p.bytes;
            for (size_t i = 0; identical && i < p.bytes; ++i) identical &= s->request[i] == p.payload[i];
            if (!identical) return reject(p.sourceNode, p.sourcePort, listener, Undefined);
            if (!s->lastAck && !s->lastWritten) {
                if (s->controlSize) s->controlPending = true;
                else { s->nextRead = 1; s->lastSent = 0; }
            }
            return Result::OK;
        }
        Request request;
        const auto error = parseRequest(p.payload, p.bytes, request);
        if (error) return reject(p.sourceNode, p.sourcePort, listener, error);
        if (request.write && (request.options.seek > TemporaryBytes ||
            request.options.transfer > TemporaryBytes - request.options.seek))
            return reject(p.sourceNode, p.sourcePort, listener, NoSpace);
        Session **slot = nullptr;
        for (auto &s : sessions) if (!s) { slot = &s; break; }
        if (!slot) return reject(p.sourceNode, p.sourcePort, listener, NoSpace);
        uint32_t port = 0;
        if (ports.allocate(port) != Result::OK) return reject(p.sourceNode, p.sourcePort, listener, NoSpace);
        auto *s = new Session; if (!s) return reject(p.sourceNode, p.sourcePort, listener, NoSpace);
        *slot = s; s->node = p.sourceNode; s->peer = p.sourcePort; s->port = port;
        s->requestSize = p.bytes; for (size_t i = 0; i < p.bytes; ++i) s->request[i] = p.payload[i];
        s->writer = request.write; s->options = request.options;
        s->phase = s->writer ? Phase::Writing : Phase::Reading;
        s->timeout = uint64_t(s->options.timeoutMS) * 1000;
        s->expires = now + 300000000 + 6 * s->timeout; s->deadline = now + s->timeout;
        auto r = files.open(p.sourceNode, request.path, s->writer, !s->options.seek, s->handle);
        if (r != Result::OK) return fail(*s, r == Result::NotReady ? NotFound : r == Result::Unsupported ? Access : NoSpace);
        uint64_t size = 0; r = files.size(s->handle, size);
        if (r != Result::OK) return fail(*s, Undefined);
        if (!s->writer) {
            if (s->options.seek > size || ((s->options.mask & ReadSize) && s->options.read > size - s->options.seek))
                return fail(*s, BadOptions);
            s->total = s->options.mask & ReadSize ? s->options.read : size - s->options.seek;
            s->options.transfer = size;
            s->finalBlock = uint32_t(s->total / s->options.block + 1);
            s->windowEnd = s->options.window < s->finalBlock ? s->options.window : s->finalBlock;
        }
        if (s->options.mask) {
            putBE16(s->control, OACK); size_t n = 2;
            if (s->options.mask & Block) n = appendOption(s->control, n, "blksize", s->options.block);
            if (s->options.mask & Timeout) n = appendOption(s->control, n, "timeoutms", s->options.timeoutMS);
            if (s->options.mask & TransferSize) n = appendOption(s->control, n, "tsize", s->options.transfer);
            if (s->options.mask & Window) n = appendOption(s->control, n, "wsize", s->options.window);
            if (s->options.mask & ReadSize) n = appendOption(s->control, n, "rsize", s->options.read);
            if (s->options.mask & Seek) n = appendOption(s->control, n, "seek", s->options.seek);
            s->controlSize = n; s->controlPending = s->initialOACK = true; s->awaitZero = !s->writer;
        } else if (s->writer) ack(*s);
        return Result::OK;
    }
    Result receive(Session &s, const QRTR::Packet &p, uint64_t now) {
        if (p.sourceNode != s.node || p.sourcePort != s.peer)
            return reject(p.sourceNode, p.sourcePort, s.port, BadTransfer);
        if (s.phase == Phase::Retiring) return Result::OK;
        if (p.bytes < 4) return fail(s, BadOperation);
        const auto op = be16(p.payload), block = be16(p.payload + 2);
        if (op == ERROR) return retire(s); // Includes Qualcomm's stat-like code 9.
        if (s.phase == Phase::Failing) return Result::OK;
        if (!s.writer) {
            if (op != ACK || p.bytes != 4) return fail(s, BadOperation);
            if (!s.initialPublished) return Result::OK;
            if (s.awaitZero) {
                if (block) return Result::OK;
                s.awaitZero = false; s.controlPending = false; s.deadline = now + s.timeout; s.retries = 0;
                return Result::OK;
            }
            // Only ACKs for data actually published in this window count.
            // Unsigned distance also handles the 16-bit block wrap at 65536.
            const uint32_t delta = uint16_t(block - uint16_t(s.lastAck));
            if (!delta || delta > s.lastSent - s.lastAck) return Result::OK;
            s.lastAck += delta; s.retries = 0; s.deadline = now + s.timeout;
            if (s.lastAck == s.finalBlock) { ++completedCount; return retire(s); }
            s.nextRead = s.lastAck + 1; s.lastSent = s.lastAck;
            s.windowEnd = s.lastAck + s.options.window;
            if (s.windowEnd > s.finalBlock) s.windowEnd = s.finalBlock;
            return Result::OK;
        }
        if (op != DATA || p.bytes > 4 + s.options.block) return fail(s, BadOperation);
        if (!s.initialPublished) return Result::OK;
        const size_t n = p.bytes - 4;
        if (block == uint16_t(s.lastWritten) && s.lastWritten) {
            bool identical = n == s.priorSize;
            for (size_t i = 0; identical && i < n; ++i) identical &= p.payload[4 + i] == s.priorData[i];
            if (!identical) return fail(s, BadOperation);
            ack(s); return Result::OK;
        }
        if (s.phase == Phase::Dally) return Result::OK;
        if (block != uint16_t(s.expected)) { ack(s); return Result::OK; }
        // The previous window has not been acknowledged. Consume premature
        // next-window DATA without a write/ACK, allowing RESUME/control behind
        // it to drain; TFTP retransmission will supply this block again.
        if (s.controlPending) return Result::OK;
        const uint64_t offset = uint64_t(s.expected - 1) * s.options.block;
        const bool final = n < s.options.block;
        if ((s.options.mask & TransferSize) && (offset + n > s.options.transfer ||
            (final && offset + n != s.options.transfer))) return fail(s, BadOptions);
        const auto r = files.write(s.handle, s.options.seek + offset, p.payload + 4, n);
        if (r != Result::OK) return fail(s, r == Result::NoMemory ? NoSpace : Undefined);
        for (size_t i = 0; i < n; ++i) s.priorData[i] = p.payload[4 + i];
        s.priorSize = n; s.lastWritten = s.expected++; ++s.inWindow;
        s.deadline = now + s.timeout; s.retries = 0; s.finalWrite = final;
        if (final) { const auto closed = closeFile(s); if (closed != Result::OK) return closed; }
        if (final || s.inWindow == s.options.window) ack(s);
        return Result::OK;
    }
    Result tick(Session &s, uint64_t now) {
        if (s.phase == Phase::Retiring) return Result::OK;
        if (now >= s.expires) { ++timeoutCount; return retire(s); }
        if (s.phase == Phase::Failing) return Result::OK;
        if (s.phase == Phase::Dally) return now >= s.deadline ? retire(s) : Result::OK;
        if (s.controlPending || !s.initialPublished || now < s.deadline) return Result::OK;
        if (++s.retries > RetryLimit) { ++timeoutCount; return retire(s); }
        if (s.writer) ack(s);
        else if (s.awaitZero) s.controlPending = true;
        else { s.nextRead = s.lastAck + 1; s.lastSent = s.lastAck; }
        // Busy publications retain this retry; the absolute lifetime still bounds it.
        s.deadline = now + s.timeout; return Result::OK;
    }
    Result progress(Session &s, uint64_t now, bool &removed) {
        removed = false;
        auto r = tick(s, now); if (r != Result::OK) return r;
        if (s.phase == Phase::Retiring) {
            uint8_t data[20]{}; write32(data, QRTR::DeleteClient); write32(data + 4, local); write32(data + 8, s.port);
            const auto sent = send(QRTR::DeleteClient, QRTR::Control, QRTR::Broadcast, QRTR::Control, data, sizeof(data), now);
            removed = sent == Result::OK; return sent;
        }
        if (s.controlPending) {
            r = send(QRTR::Data, s.port, s.node, s.peer, s.control, s.controlSize, now);
            if (r == Result::OK) {
                s.controlPending = false; s.initialPublished = true;
                if (s.phase == Phase::Failing) s.phase = Phase::Retiring;
                else if (s.writer) {
                    s.inWindow = 0;
                    if (s.finalWrite && s.phase != Phase::Dally) { s.phase = Phase::Dally; ++completedCount; }
                    s.deadline = now + s.timeout * (s.phase == Phase::Dally ? 2 : 1);
                } else s.deadline = now + s.timeout;
            }
            return r;
        }
        if (s.phase != Phase::Reading || s.awaitZero || s.nextRead > s.windowEnd) return Result::NotReady;
        const uint64_t offset = uint64_t(s.nextRead - 1) * s.options.block;
        const auto remaining = s.total - offset;
        const size_t n = size_t(remaining < s.options.block ? remaining : s.options.block);
        putBE16(payload, DATA); putBE16(payload + 2, uint16_t(s.nextRead));
        r = files.read(s.handle, s.options.seek + offset, payload + 4, n);
        if (r != Result::OK) return fail(s, Undefined);
        r = send(QRTR::Data, s.port, s.node, s.peer, payload, n + 4, now);
        if (r == Result::OK) {
            s.lastSent = s.nextRead++; s.initialPublished = true; s.deadline = now + s.timeout;
        }
        return r;
    }
public:
    Server(Endpoint &endpoint, Files &backing, QRTR::PortAllocator *sharedPorts = nullptr)
        : link(endpoint), files(backing), ports(sharedPorts ? *sharedPorts : ownedPorts) {}
    ~Server() { for (auto *s : sessions) { if (s) closeFile(*s); delete s; } }
    Result start(uint64_t generation, uint32_t node, uint32_t port, uint64_t now) {
        if (active || !generation || generation <= epoch || node == QRTR::Broadcast || !port || port >= 0x4000 ||
            now > UINT64_MAX - UINT64_C(2000000000)) return Result::Invalid;
        const auto r = files.start(generation); if (r != Result::OK) return r;
        for (auto &s : sessions) { delete s; s = nullptr; }
        for (auto &f : flows) f = {};
        for (auto &r : resumes) r = {};
        errorReply.pending = false; epoch = generation; local = node; listener = port;
        lastTime = now; active = announce = true; announced = false; return Result::OK;
    }
    void stopped() {
        active = false;
        for (auto &s : sessions) { if (s) closeFile(*s); delete s; s = nullptr; }
        files.stopped();
    }
    uint32_t completed() const { return completedCount; }
    uint32_t timedOut() const { return timeoutCount; }
    uint32_t rejected() const { return rejectedCount; }
    bool published() const { return active && announced; }
    bool owns(const QRTR::Packet &p) const {
        if (!active || (p.type != QRTR::Data && p.type != QRTR::ResumeTX) || p.destinationNode != local) return false;
        if (p.destinationPort == listener) return true;
        for (const auto *s : sessions) if (s && p.destinationPort == s->port) return true;
        return false;
    }
    Result feed(uint64_t generation, const void *data, size_t bytes, uint64_t now) {
        if (!active || generation != epoch) return Result::Stale;
        auto r = clock(now); if (r != Result::OK) return r;
        QRTR::Packet p; r = QRTR::parse(data, bytes, p); if (r != Result::OK) return r;
        if (p.destinationNode != local && p.destinationNode != QRTR::Broadcast) return Result::NotReady;
        if (p.type == QRTR::ResumeTX) {
            if (!owns(p) || p.confirm || p.bytes != 20 || read32(p.payload) != QRTR::ResumeTX ||
                read32(p.payload + 4) != p.sourceNode || read32(p.payload + 8) != p.sourcePort) return Result::Invalid;
            auto *f = flow(p.sourceNode, p.sourcePort, false); if (!f) return Result::NotReady;
            f->pending = 0; return Result::OK;
        }
        if (p.type != QRTR::Data) {
            if (p.confirm || p.bytes != 20 || read32(p.payload) != p.type ||
                p.sourcePort != QRTR::Control || p.destinationPort != QRTR::Control) return Result::NotReady;
            if (p.type == QRTR::Hello || p.type == QRTR::Ping || (p.type == QRTR::NewLookup &&
                (!read32(p.payload + 4) || read32(p.payload + 4) == Service) &&
                (!read32(p.payload + 8) || read32(p.payload + 8) == 1))) announce = true;
            const bool whole = p.type == QRTR::Bye || p.type == QRTR::Exit;
            if (whole || p.type == QRTR::DeleteClient) {
                const uint32_t node = whole ? p.sourceNode : read32(p.payload + 4);
                const uint32_t port = whole ? 0 : read32(p.payload + 8);
                if (node != p.sourceNode || (!whole && (!port || port >= QRTR::Control))) return Result::Invalid;
                for (auto *s : sessions) if (s && s->node == node && (whole || s->peer == port)) {
                    r = retire(*s); if (r != Result::OK) return r;
                }
                for (auto &f : flows) if (f.used && f.node == node && (whole || f.port == port)) f = {};
                for (auto &item : resumes) if (item.used && item.node == node && (whole || item.peer == port)) item = {};
                if (errorReply.pending && errorReply.node == node && (whole || errorReply.peer == port)) errorReply.pending = false;
            }
            return Result::OK;
        }
        if (!owns(p) || !announced) return Result::NotReady;
        if (p.sourceNode == local || !p.sourcePort || p.sourcePort >= QRTR::Control) return Result::Invalid;
        if (now > UINT64_MAX - UINT64_C(2000000000)) return Result::Invalid;
        if (p.confirm && !resumeAvailable(p)) return Result::Busy;
        if (p.destinationPort == listener) r = begin(p, now);
        else {
            r = Result::NotReady;
            for (auto *s : sessions) if (s && s->port == p.destinationPort) { r = receive(*s, p, now); break; }
        }
        if (r == Result::OK && p.confirm) addResume(p);
        return r;
    }
    Result progress(uint64_t generation, uint64_t now) {
        if (!active || generation != epoch) return Result::Stale;
        auto r = clock(now); if (r != Result::OK) return r;
        // Deadlines also run during sustained discovery or RX confirmations.
        for (auto *s : sessions) if (s) { r = tick(*s, now); if (r != Result::OK) return r; }
        // RX flow confirmations must drain even while a transfer's TX window
        // waits for a peer RESUME. No file I/O is repeated to send a reply.
        for (auto &item : resumes) if (item.used) {
            uint8_t data[20]{}; write32(data, QRTR::ResumeTX); write32(data + 4, local); write32(data + 8, item.port);
            r = send(QRTR::ResumeTX, item.port, item.node, item.peer, data, sizeof(data), now);
            if (r == Result::OK) item.used = false;
            return r;
        }
        if (announce) {
            uint8_t data[20]{}; write32(data, QRTR::NewServer); write32(data + 4, Service); write32(data + 8, 1);
            write32(data + 12, local); write32(data + 16, listener);
            r = send(QRTR::NewServer, QRTR::Control, QRTR::Broadcast, QRTR::Control, data, sizeof(data), now);
            if (r == Result::OK) { announce = false; announced = true; }
            return r;
        }
        bool busy = false;
        if (errorReply.pending) {
            r = send(QRTR::Data, errorReply.port, errorReply.node, errorReply.peer, errorReply.payload, errorReply.bytes, now);
            if (r == Result::OK) { errorReply.pending = false; return r; }
            if (r != Result::Busy && r != Result::NotReady) return r;
            busy = r == Result::Busy;
        }
        for (unsigned n = 0; n < MaxSessions; ++n) {
            const unsigned i = (cursor + n) % MaxSessions;
            auto *s = sessions[i]; if (!s) continue;
            bool removed = false;
            r = progress(*s, now, removed);
            if (r == Result::OK) {
                if (removed) { delete s; sessions[i] = nullptr; }
                cursor = (i + 1) % MaxSessions; return r;
            }
            if (r != Result::Busy && r != Result::NotReady) return r;
            busy |= r == Result::Busy;
        }
        return busy ? Result::Busy : Result::NotReady;
    }
};
}} // namespace Baseband::TFTP
