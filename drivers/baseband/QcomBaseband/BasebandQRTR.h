// SPDX-License-Identifier: GPL-2.0-only
// Native IPCRTR control/data layout, Linux v6.12 net/qrtr/af_qrtr.c.
#pragma once
#include "BasebandQMI.h"

namespace Baseband { namespace QRTR {
constexpr uint32_t Control = 0xfffffffe, Broadcast = 0xffffffff;
constexpr size_t MaxPacket = 65536, MaxServers = 128, MaxClients = 32;
enum Type : uint32_t { Data = 1, Hello, Bye, NewServer, DeleteServer, DeleteClient, ResumeTX,
                       Exit, Ping, NewLookup, DeleteLookup };
struct Packet {
    uint32_t type = 0, sourceNode = 0, sourcePort = 0, destinationNode = 0, destinationPort = 0;
    bool confirm = false;
    const uint8_t *payload = nullptr;
    uint32_t bytes = 0;
};
inline Result parse(const void *buffer, size_t bytes, Packet &out) {
    out = {};
    if (!buffer || bytes < 16 || bytes > MaxPacket || (bytes & 3)) return Result::Invalid;
    const auto *p = static_cast<const uint8_t *>(buffer);
    size_t header = 0;
    if (p[0] == 1) {
        if (bytes < 32 || read32(p) != 1 || read32(p + 16) > 1) return Result::Invalid;
        out = {read32(p + 4), read32(p + 8), read32(p + 12), read32(p + 24), read32(p + 28),
               read32(p + 16) != 0, nullptr, read32(p + 20)};
        header = 32;
    } else if (p[0] == 3) {
        if (p[2] & ~1u) return Result::Unsupported;
        header = 16 + p[3];
        out = {p[1], read16(p + 8), read16(p + 10), read16(p + 12), read16(p + 14),
               bool(p[2] & 1), nullptr, read32(p + 4)};
        if (out.sourcePort == 0xfffe) out.sourcePort = Control;
        if (out.destinationPort == 0xfffe) out.destinationPort = Control;
    } else return Result::Unsupported;
    if (out.destinationPort == 0xffff) out.destinationPort = Control; // Legacy v1 nameservice.
    if (header > bytes || !out.bytes || ((uint64_t(out.bytes) + 3) & ~3ULL) != bytes - header ||
        out.type < Data || out.type > DeleteLookup || out.sourceNode == Broadcast ||
        (out.type != Data && out.type != ResumeTX && out.destinationPort != Control)) return Result::Invalid;
    out.payload = p + header;
    return Result::OK;
}
inline Result encode(void *buffer, size_t capacity, const Packet &packet, size_t &written) {
    written = 0;
    const size_t aligned = (uint64_t(packet.bytes) + 3) & ~3ULL;
    if (!buffer || !packet.payload || !packet.bytes || packet.type < Data || packet.type > DeleteLookup ||
        packet.sourceNode == Broadcast || capacity < 32 || aligned > capacity - 32 || aligned > MaxPacket - 32)
        return Result::Invalid;
    auto *p = static_cast<uint8_t *>(buffer);
    write32(p, 1); write32(p + 4, packet.type); write32(p + 8, packet.sourceNode);
    write32(p + 12, packet.sourcePort); write32(p + 16, packet.confirm);
    write32(p + 20, packet.bytes); write32(p + 24, packet.destinationNode); write32(p + 28, packet.destinationPort);
    for (size_t i = 0; i < aligned; ++i) p[32 + i] = i < packet.bytes ? packet.payload[i] : 0;
    written = 32 + aligned; return Result::OK;
}
struct Server { uint32_t service = 0, instance = 0, node = 0, port = 0; bool used = false; };
enum class EventType : uint32_t { None, ServerAdded, ServerRemoved, Response, Indication, Timeout, Restart };
struct Event {
    EventType type = EventType::None;
    uint32_t client = 0, service = 0;
    QMI::Message message{};
    Result result = Result::OK;
    uint16_t qmiError = 0;
};
struct Client {
    uint32_t port = 0;
    Server server{};
    uint16_t next = 0, pending = 0, message = 0;
    uint64_t deadline = 0;
    bool used = false, connected = false;
};
struct Flow { uint32_t node = 0, port = 0, pending = 0; bool used = false; };
// Clients and raw remote-file transfers share this node-wide namespace.
// Never recycle a port while an old remote response can still be in flight.
class PortAllocator {
    uint32_t next = 0x4000;
public:
    Result allocate(uint32_t &port) {
        port = 0;
        if (next >= 0xfffd) return Result::NoMemory;
        port = ++next; return Result::OK;
    }
};

// Endpoint is GLINK::Link (send copies the complete datagram into retained
// storage). The owner drives Link::pump, feeds each completed datagram here,
// consumes Event synchronously, then calls Link::accept. No QMUX client-ID
// allocation is needed on QRTR; each client owns a fresh node-wide port.
template<class Endpoint> class Router {
    Endpoint &link;
    Server servers[MaxServers]{};
    Client clients[MaxClients]{};
    Flow flows[MaxServers]{};
    uint32_t local = 1, resumeNode = 0, resumePort = 0, resumePeerPort = 0;
    PortAllocator ownedPorts;
    PortAllocator &ports;
    uint64_t epoch = 0, lastTime = 0;
    bool helloPending = false, resumePending = false, active = false;
    uint8_t wire[MaxPacket]{}, message[MaxPacket - 32]{};
    Client *find(uint32_t port) {
        for (auto &c : clients) if (c.used && c.port == port) return &c;
        return nullptr;
    }
    Flow *flow(uint32_t node, uint32_t port, bool create) {
        Flow *free = nullptr;
        for (auto &f : flows) {
            if (f.used && f.node == node && f.port == port) return &f;
            if (!f.used && !free) free = &f;
        }
        if (!create || !free) return nullptr;
        *free = {node, port, 0, true}; return free;
    }
    Result clock(uint64_t now) {
        if (now < lastTime) { active = false; return Result::Invalid; }
        lastTime = now; return Result::OK;
    }
    Result send(const Packet &packet, uint64_t now) {
        size_t bytes = 0; const auto r = encode(wire, sizeof(wire), packet, bytes);
        return r == Result::OK ? link.send(epoch, wire, bytes, now) : r;
    }
    void detach(uint32_t node, uint32_t port, bool wholeNode) {
        for (auto &s : servers) if (s.used && s.node == node && (wholeNode || s.port == port)) s.used = false;
        for (auto &c : clients) if (c.used && c.server.node == node && (wholeNode || c.server.port == port)) {
            c.connected = false; c.pending = 0;
        }
        for (auto &f : flows) if (f.used && f.node == node && (wholeNode || f.port == port)) f = {};
    }
public:
    explicit Router(Endpoint &endpoint, PortAllocator *sharedPorts = nullptr)
        : link(endpoint), ports(sharedPorts ? *sharedPorts : ownedPorts) {}
    Result start(uint64_t generation, uint32_t node, uint64_t now) {
        if (!generation || generation <= epoch || node == Broadcast) return Result::Invalid;
        if (active) return Result::Busy;
        for (auto &s : servers) s = {};
        for (auto &c : clients) c = {};
        for (auto &f : flows) f = {};
        epoch = generation; local = node; lastTime = now;
        resumePending = false; helloPending = active = true;
        return Result::OK;
    }
    void disconnect() { active = false; for (auto &c : clients) { c.connected = false; c.pending = 0; } }
    bool clientReady(uint64_t generation, uint32_t port) const {
        if (!active || generation != epoch) return false;
        for (const auto &c : clients) if (c.used && c.port == port) return c.connected;
        return false;
    }
    Result lookup(uint64_t generation, uint32_t service, uint8_t version, uint32_t instance, uint64_t now) {
        if (!active || generation != epoch) return Result::Stale;
        if (!service || instance > 0xffffff) return Result::Invalid;
        auto r = clock(now); if (r != Result::OK) return r;
        if (resumePending || helloPending) return Result::Busy;
        uint8_t payload[20]{}; write32(payload, NewLookup); write32(payload + 4, service);
        write32(payload + 8, (instance << 8) | version);
        return send({NewLookup, local, Control, Broadcast, Control, false, payload, sizeof(payload)}, now);
    }
    Result open(uint64_t generation, uint32_t service, uint8_t version, uint32_t instance, uint32_t &client) {
        client = 0;
        if (!active || generation != epoch) return Result::Stale;
        if (!service || instance > 0xffffff) return Result::Invalid;
        const Server *selected = nullptr;
        for (const auto &s : servers)
            if (s.used && s.service == service && s.instance == ((instance << 8) | version)) {
                if (selected) return Result::Busy; // Explicit selection required across duplicate nodes.
                selected = &s;
            }
        if (!selected) return Result::NotReady;
        // Keep ports monotonic across service restarts/generations; old replies
        // cannot become replies to a recycled 16-bit QMI transaction number.
        for (auto &c : clients) if (!c.used) {
            uint32_t port = 0;
            const auto r = ports.allocate(port); if (r != Result::OK) return r;
            c = {}; c.server = *selected; c.port = port; c.used = c.connected = true;
            client = c.port; return Result::OK;
        }
        return Result::NoMemory;
    }
    Result request(uint64_t generation, uint32_t client, uint16_t id, const void *data, size_t bytes,
                   uint64_t now, uint64_t timeout = 5000000) {
        if (!active || generation != epoch) return Result::Stale;
        auto *c = find(client);
        if (!c || !c->connected) return Result::NotReady;
        if (!timeout || timeout > UINT64_MAX - now) return Result::Invalid;
        auto r = clock(now); if (r != Result::OK) return r;
        if (c->pending || resumePending || helloPending) return Result::Busy;
        auto *f = flow(c->server.node, c->server.port, true);
        if (!f) return Result::NoMemory;
        if (f->pending >= 10) return Result::Busy;
        if (c->next == UINT16_MAX) return Result::NoMemory; // Open a fresh client port.
        size_t written = 0;
        const uint16_t sequence = ++c->next;
        r = QMI::request(message, sizeof(message), sequence, id, data, bytes, written);
        if (r == Result::OK) r = send({Data, local, c->port, c->server.node, c->server.port,
                                    f->pending + 1 == 5, message, uint32_t(written)}, now);
        if (r == Result::OK) {
            ++f->pending; c->pending = sequence; c->message = id; c->deadline = now + timeout;
        }
        else if (r == Result::Quarantined) disconnect();
        return r;
    }
    Result close(uint64_t generation, uint32_t client, uint64_t now) {
        if (!active || generation != epoch) return Result::Stale;
        auto *c = find(client); if (!c) return Result::Invalid;
        auto r = clock(now); if (r != Result::OK) return r;
        if (c->pending) return Result::Busy;
        uint8_t payload[20]{}; write32(payload, DeleteClient);
        write32(payload + 4, local); write32(payload + 8, c->port);
        r = send({DeleteClient, local, Control, Broadcast, Control, false, payload, sizeof(payload)}, now);
        if (r == Result::OK) *c = {};
        return r;
    }
    Result progress(uint64_t generation, uint64_t now, Event &event) {
        event = {};
        if (!active || generation != epoch) return Result::Stale;
        auto r = clock(now); if (r != Result::OK) return r;
        for (auto &c : clients) if (c.used && c.pending && now >= c.deadline) {
            c.pending = 0; event.type = EventType::Timeout; event.client = c.port;
            event.service = c.server.service; event.result = Result::Timeout; return Result::OK;
        }
        if (!helloPending && !resumePending) return Result::NotReady;
        uint8_t payload[20]{};
        const uint32_t type = helloPending ? Hello : ResumeTX;
        write32(payload, type);
        if (!helloPending) { write32(payload + 4, local); write32(payload + 8, resumePort); }
        r = send({type, local, helloPending ? Control : resumePort,
                  helloPending ? Broadcast : resumeNode, helloPending ? Control : resumePeerPort,
                  false, payload, sizeof(payload)}, now);
        if (r == Result::OK) { if (helloPending) helloPending = false; else resumePending = false; }
        else if (r == Result::Quarantined) disconnect();
        return r;
    }
    Result feed(uint64_t generation, const void *data, size_t bytes, uint64_t now, Event &event) {
        event = {};
        if (!active || generation != epoch) return Result::Stale;
        auto r = clock(now); if (r != Result::OK) return r;
        Packet p{}; r = parse(data, bytes, p); if (r != Result::OK) return r;
        if (p.destinationNode != local && p.destinationNode != Broadcast) return Result::NotReady;
        if (p.type != Data) {
            if (p.confirm || p.bytes != 20 || read32(p.payload) != p.type) return Result::Invalid;
            if (p.type == ResumeTX) {
                if (p.destinationNode != local || read32(p.payload + 4) != p.sourceNode ||
                    read32(p.payload + 8) != p.sourcePort) return Result::Invalid;
                auto *f = flow(p.sourceNode, p.sourcePort, false);
                if (!f) return Result::NotReady;
                f->pending = 0; return Result::OK;
            }
            if (p.sourcePort != Control || p.destinationPort != Control) return Result::Invalid;
            if (p.type == Hello || p.type == Ping) return Result::OK;
            if (p.type == Bye || p.type == Exit) {
                detach(p.sourceNode, 0, true); event.type = EventType::Restart; return Result::OK;
            }
            if (p.type != NewServer && p.type != DeleteServer) return Result::Unsupported;
            const Server incoming{read32(p.payload + 4), read32(p.payload + 8),
                                  read32(p.payload + 12), read32(p.payload + 16), true};
            if (!incoming.service || incoming.node == local || incoming.node == Broadcast ||
                !incoming.port || incoming.port >= Control) return Result::Invalid;
            Server *found = nullptr, *free = nullptr;
            for (auto &s : servers) {
                if (!s.used && !free) free = &s;
                if (s.used && s.service == incoming.service && s.instance == incoming.instance &&
                    s.node == incoming.node && s.port == incoming.port) found = &s;
            }
            if (p.type == DeleteServer) {
                if (!found) return Result::NotReady;
                detach(incoming.node, incoming.port, false); event.type = EventType::ServerRemoved;
            } else {
                if (found) return Result::OK;
                if (!free) return Result::NoMemory;
                *free = incoming; event.type = EventType::ServerAdded;
            }
            event.service = incoming.service; return Result::OK;
        }
        if (p.destinationNode != local) return Result::Invalid;
        auto *c = find(p.destinationPort);
        if (!c || !c->connected || p.sourceNode != c->server.node || p.sourcePort != c->server.port)
            return Result::NotReady;
        QMI::Message incoming{}; r = QMI::parse(p.payload, p.bytes, incoming);
        if (r != Result::OK || incoming.type == QMI::Request) return Result::Invalid;
        if (p.confirm && resumePending) return Result::Busy;
        if (incoming.type == QMI::Response) {
            if (!c->pending || incoming.transaction != c->pending || incoming.id != c->message)
                return Result::Stale;
            if (now >= c->deadline) return Result::Timeout;
            r = QMI::response(incoming, event.qmiError);
            if (r == Result::Invalid) return r;
            c->pending = 0; event.type = EventType::Response;
        } else event.type = EventType::Indication;
        if (p.confirm) {
            resumePending = true; resumeNode = p.sourceNode; resumePort = c->port; resumePeerPort = p.sourcePort;
        }
        event.client = c->port; event.service = c->server.service; event.message = incoming; event.result = r;
        return Result::OK;
    }
};
}} // namespace Baseband::QRTR
