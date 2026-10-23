// SPDX-License-Identifier: GPL-2.0-only
// AP service advertisement/request/response over the same native QRTR link.
#pragma once
#include "BasebandQRTR.h"

namespace Baseband { namespace QRTR {
constexpr size_t ServiceRequestBytes = 4096, ServicePeers = 32;
constexpr uint64_t ServiceRequestTimeoutUS = 5000000;
struct ServiceRequest {
    uint32_t node = 0, port = 0;
    QMI::Message message{};
};
struct PeerRemoved { uint32_t node = 0, port = 0; bool valid = false, wholeNode = false; };
template<class Endpoint> class ServiceHost {
    Endpoint &link;
    uint64_t epoch = 0, lastTime = 0, deadline = 0;
    uint32_t local = 1, port = 0, service = 0, instance = 0;
    bool active = false, announce = false, announced = false, pending = false, resume = false;
    uint32_t peerNode = 0, peerPort = 0, resumeNode = 0, resumePort = 0;
    uint8_t incoming[ServiceRequestBytes]{}, wire[ServiceRequestBytes + 32]{};
    size_t incomingBytes = 0;
    Flow flows[ServicePeers]{};
    Result clock(uint64_t now) {
        if (now < lastTime) { active = false; return Result::Stale; }
        lastTime = now;
        if (pending && now >= deadline) { active = false; return Result::Timeout; }
        return Result::OK;
    }
    Flow *flow(uint32_t node, uint32_t endpoint, bool create) {
        Flow *empty = nullptr;
        for (auto &f : flows) {
            if (f.used && f.node == node && f.port == endpoint) return &f;
            if (!f.used && !empty) empty = &f;
        }
        if (!create || !empty) return nullptr;
        *empty = {node, endpoint, 0, true}; return empty;
    }
    Result send(const Packet &p, uint64_t now) {
        size_t bytes = 0; auto r = encode(wire, sizeof(wire), p, bytes);
        if (r == Result::OK) r = link.send(epoch, wire, bytes, now);
        if (r == Result::Quarantined) active = false;
        return r;
    }
public:
    explicit ServiceHost(Endpoint &endpoint) : link(endpoint) {}
    Result start(uint64_t generation, uint32_t node, uint32_t endpoint, uint32_t identity,
                 uint8_t version, uint32_t inst, uint64_t now) {
        if (active || !generation || generation <= epoch || node == Broadcast || !endpoint || endpoint >= Control ||
            !identity || inst > 0xffffff) return Result::Invalid;
        epoch = generation; local = node; port = endpoint; service = identity; instance = (inst << 8) | version;
        lastTime = now; active = announce = true; announced = pending = resume = false;
        for (auto &f : flows) f = {};
        return Result::OK;
    }
    void disconnect() { active = pending = false; }
    bool hasRequest() const { return active && pending; }
    bool published() const { return active && announced; }
    uint32_t localPort() const { return port; }
    bool owns(const Packet &p) const {
        return active && (p.type == Data || p.type == ResumeTX) &&
            p.destinationNode == local && p.destinationPort == port;
    }
    // Control messages also go to the ordinary client Router. Only DATA and
    // RESUME addressed to this port are consumed exclusively by this object.
    Result feed(uint64_t generation, const void *data, size_t bytes, uint64_t now, PeerRemoved *removed = nullptr) {
        if (removed) *removed = {};
        if (!active || generation != epoch) return Result::Stale;
        auto r = clock(now); if (r != Result::OK) return r;
        Packet p; r = parse(data, bytes, p); if (r != Result::OK) return r;
        if (p.destinationNode != local && p.destinationNode != Broadcast) return Result::NotReady;
        if (p.type == ResumeTX) {
            if (p.destinationNode != local || p.destinationPort != port) return Result::NotReady;
            if (p.confirm || p.bytes != 20 || read32(p.payload) != ResumeTX ||
                read32(p.payload + 4) != p.sourceNode || read32(p.payload + 8) != p.sourcePort) return Result::Invalid;
            auto *f = flow(p.sourceNode, p.sourcePort, false); if (!f) return Result::NotReady;
            f->pending = 0; return Result::OK;
        }
        if (p.type != Data) {
            if (p.confirm || p.bytes != 20 || read32(p.payload) != p.type ||
                p.sourcePort != Control || p.destinationPort != Control) return Result::NotReady;
            if (p.type == Hello || p.type == Ping || (p.type == NewLookup &&
                (!read32(p.payload + 4) || read32(p.payload + 4) == service) &&
                (!read32(p.payload + 8) || read32(p.payload + 8) == instance))) announce = true;
            const bool wholeNode = p.type == Bye || p.type == Exit;
            if (wholeNode || p.type == DeleteClient) {
                const uint32_t node = wholeNode ? p.sourceNode : read32(p.payload + 4);
                const uint32_t endpoint = wholeNode ? 0 : read32(p.payload + 8);
                if (node != p.sourceNode || (!wholeNode && (!endpoint || endpoint >= Control))) return Result::Invalid;
                for (auto &f : flows) if (f.used && f.node == node && (wholeNode || f.port == endpoint)) f = {};
                if (pending && peerNode == node && (wholeNode || peerPort == endpoint)) pending = false;
                if (resume && resumeNode == node && (wholeNode || resumePort == endpoint)) resume = false;
                if (removed) *removed = {node, endpoint, true, wholeNode};
            }
            return Result::OK;
        }
        if (p.destinationNode != local || p.destinationPort != port) return Result::NotReady;
        if (p.sourceNode == local || !p.sourcePort || p.sourcePort >= Control || p.bytes > sizeof(incoming)) return Result::Invalid;
        if (!announced) return Result::NotReady;
        if (pending || (p.confirm && resume)) return Result::Busy;
        if (now > UINT64_MAX - ServiceRequestTimeoutUS) return Result::Invalid;
        QMI::Message request;
        r = QMI::parse(p.payload, p.bytes, request, true);
        if (r != Result::OK || request.type != QMI::Request) return Result::Invalid;
        if (!flow(p.sourceNode, p.sourcePort, true)) return Result::NoMemory;
        for (unsigned i = 0; i < p.bytes; ++i) incoming[i] = p.payload[i];
        incomingBytes = p.bytes; peerNode = p.sourceNode; peerPort = p.sourcePort;
        deadline = now + ServiceRequestTimeoutUS; pending = true;
        if (p.confirm) { resume = true; resumeNode = p.sourceNode; resumePort = p.sourcePort; }
        return Result::OK;
    }
    Result request(uint64_t generation, ServiceRequest &out) const {
        out = {};
        if (!active || generation != epoch) return Result::Stale;
        if (!pending) return Result::NotReady;
        out.node = peerNode; out.port = peerPort;
        return QMI::parse(incoming, incomingBytes, out.message, true);
    }
    Result respond(uint64_t generation, const void *data, size_t bytes, uint64_t now) {
        if (!active || generation != epoch) return Result::Stale;
        auto r = clock(now); if (r != Result::OK) return r;
        if (!pending) return Result::NotReady;
        if (resume || announce) return Result::Busy;
        QMI::Message reply, request;
        if (QMI::parse(data, bytes, reply, true) != Result::OK || reply.type != QMI::Response ||
            QMI::parse(incoming, incomingBytes, request, true) != Result::OK ||
            reply.transaction != request.transaction || reply.id != request.id) return Result::Invalid;
        auto *f = flow(peerNode, peerPort, false); if (!f) return Result::Invalid;
        if (f->pending >= 10) return Result::Busy;
        r = send({Data, local, port, peerNode, peerPort, f->pending + 1 == 5,
                  static_cast<const uint8_t *>(data), uint32_t(bytes)}, now);
        if (r == Result::OK) { ++f->pending; pending = false; }
        return r;
    }
    Result progress(uint64_t generation, uint64_t now) {
        if (!active || generation != epoch) return Result::Stale;
        auto r = clock(now); if (r != Result::OK) return r;
        if (!resume && !announce) return Result::NotReady;
        uint8_t payload[20]{};
        if (resume) {
            write32(payload, ResumeTX); write32(payload + 4, local); write32(payload + 8, port);
            r = send({ResumeTX, local, port, resumeNode, resumePort, false, payload, sizeof(payload)}, now);
            if (r == Result::OK) resume = false;
        } else {
            write32(payload, NewServer); write32(payload + 4, service); write32(payload + 8, instance);
            write32(payload + 12, local); write32(payload + 16, port);
            r = send({NewServer, local, Control, Broadcast, Control, false, payload, sizeof(payload)}, now);
            if (r == Result::OK) { announce = false; announced = true; }
        }
        return r;
    }
};

// Share one IPCRTR endpoint between AP clients and a local service. Control
// traffic reaches both, while DATA/RESUME use their destination port. Busy
// means the owner retains this datagram but still calls progressService: a
// pending reply or RX confirmation must be able to drain before retrying it.
template<class Endpoint, class Service> Result feedService(Router<Endpoint> &router, Service &service,
    uint64_t generation, const void *data, size_t bytes, uint64_t now, Event &event) {
    event = {}; Packet packet;
    auto r = parse(data, bytes, packet); if (r != Result::OK) return r;
    if (service.owns(packet)) return service.feed(generation, data, bytes, now);
    if (packet.type != Data && packet.type != ResumeTX) {
        r = service.feed(generation, data, bytes, now);
        if (r != Result::OK && r != Result::NotReady && r != Result::Unsupported) return r;
    }
    return router.feed(generation, data, bytes, now, event);
}
template<class Endpoint, class Service> Result progressService(Router<Endpoint> &router, Service &service,
    uint64_t generation, uint64_t now, Event &event) {
    const auto a = router.progress(generation, now, event);
    if (a != Result::OK && a != Result::Busy && a != Result::NotReady) return a;
    const auto b = service.progress(generation, now);
    if (b != Result::OK && b != Result::Busy && b != Result::NotReady) return b;
    return a == Result::OK || b == Result::OK ? Result::OK : a == Result::Busy || b == Result::Busy ? Result::Busy : Result::NotReady;
}
// RMTFS uses QMI while TFTP uses raw datagrams and fresh transfer ports.
// Dispatch DATA once; all three endpoints still observe nameservice teardown.
template<class Endpoint, class A, class B> Result feedServices(Router<Endpoint> &router, A &first, B &second,
    uint64_t generation, const void *data, size_t bytes, uint64_t now, Event &event) {
    event = {}; Packet packet;
    auto r = parse(data, bytes, packet); if (r != Result::OK) return r;
    if (first.owns(packet)) return first.feed(generation, data, bytes, now);
    if (second.owns(packet)) return second.feed(generation, data, bytes, now);
    if (packet.type != Data && packet.type != ResumeTX) {
        r = first.feed(generation, data, bytes, now);
        if (r != Result::OK && r != Result::NotReady && r != Result::Unsupported) return r;
        r = second.feed(generation, data, bytes, now);
        if (r != Result::OK && r != Result::NotReady && r != Result::Unsupported) return r;
    }
    return router.feed(generation, data, bytes, now, event);
}
template<class Endpoint, class A, class B> Result progressServices(Router<Endpoint> &router, A &first, B &second,
    unsigned &cursor, uint64_t generation, uint64_t now, Event &event) {
    event = {}; bool sent = false, busy = false;
    // Rotate who can claim the one shared IPCRTR transmit buffer first.
    for (unsigned i = 0; i < 3; ++i) {
        const unsigned current = (cursor + i) % 3;
        const auto r = current == 0 ? router.progress(generation, now, event) :
            current == 1 ? first.progress(generation, now) : second.progress(generation, now);
        if (r != Result::OK && r != Result::Busy && r != Result::NotReady) return r;
        sent |= r == Result::OK; busy |= r == Result::Busy;
    }
    cursor = (cursor + 1) % 3;
    return sent ? Result::OK : busy ? Result::Busy : Result::NotReady;
}
}} // namespace Baseband::QRTR
