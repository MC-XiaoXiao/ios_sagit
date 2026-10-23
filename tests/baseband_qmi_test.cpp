// SPDX-License-Identifier: GPL-2.0-only
#include "BasebandQRTR.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
using namespace Baseband;
namespace R = Baseband::QRTR;
struct Endpoint {
    Result result = Result::OK;
    std::vector<uint8_t> wire;
    Result send(uint64_t generation, const void *p, size_t n, uint64_t) {
        assert(generation);
        if (result != Result::OK) return result;
        const auto *b = static_cast<const uint8_t *>(p); wire.assign(b, b + n); return Result::OK;
    }
    R::Packet last() {
        R::Packet p{}; assert(R::parse(wire.data(), wire.size(), p) == Result::OK); return p;
    }
};
int main() {
    Endpoint io;
    auto router = std::make_unique<R::Router<Endpoint>>(io);
    R::Event event;
    assert(router->start(1, 1, 0) == Result::OK);
    assert(router->progress(1, 0, event) == Result::OK && io.last().type == R::Hello);
    std::vector<uint8_t> incoming(256);
    uint8_t payload[128]{};
    auto feed = [&](uint32_t type, uint32_t node, uint32_t src, uint32_t dst,
                    size_t length, uint64_t now, bool confirm = false) {
        size_t bytes = 0;
        R::Packet p{type, node, src, 1, dst, confirm, payload, uint32_t(length)};
        assert(R::encode(incoming.data(), incoming.size(), p, bytes) == Result::OK);
        return router->feed(1, incoming.data(), bytes, now, event);
    };
    write32(payload, R::NewServer); write32(payload + 4, QMI::DMS);
    write32(payload + 8, 1); write32(payload + 12, 0); write32(payload + 16, 7);
    assert(feed(R::NewServer, 0, R::Control, R::Control, 20, 1) == Result::OK);
    assert(event.type == R::EventType::ServerAdded && event.service == QMI::DMS);
    uint32_t client = 0;
    assert(router->open(1, QMI::DMS, 1, 0, client) == Result::OK && client == 0x4001);
    // A real modem's QRTR node can be zero; SMEM host PID and QRTR node differ.
    assert(router->request(1, client, 0x2d, nullptr, 0, 2) == Result::OK);
    assert(io.last().destinationNode == 0 && io.last().destinationPort == 7);
    QMI::Message request{}; assert(QMI::parse(io.last().payload, io.last().bytes, request) == Result::OK);
    assert(request.id == 0x2d && request.type == QMI::Request && request.transaction == 1);
    auto response = [&](uint16_t sequence, uint16_t error = 0) {
        memset(payload, 0, sizeof(payload)); payload[0] = QMI::Response;
        write16(payload + 1, sequence); write16(payload + 3, 0x2d); write16(payload + 5, 11);
        payload[7] = 2; write16(payload + 8, 4); write16(payload + 10, error ? 1 : 0);
        write16(payload + 12, error);
        payload[14] = 1; write16(payload + 15, 1); payload[17] = 0; // DMS operating mode online.
    };
    response(2);
    assert(feed(R::Data, 0, 7, client, 18, 3) == Result::Stale);
    response(1);
    assert(feed(R::Data, 0, 8, client, 18, 3) == Result::NotReady); // Wrong service endpoint.
    assert(feed(R::Data, 0, 7, client, 18, 3, true) == Result::OK);
    assert(event.type == R::EventType::Response && event.client == client && event.result == Result::OK);
    assert(router->request(1, client, 0x2d, nullptr, 0, 3) == Result::Busy); // RESUME first.
    io.result = Result::Busy;
    assert(router->progress(1, 4, event) == Result::Busy);
    io.result = Result::OK;
    assert(router->progress(1, 4, event) == Result::OK);
    assert(io.last().type == R::ResumeTX && io.last().sourcePort == client && io.last().destinationPort == 7);
    assert(read32(io.last().payload + 4) == 1 && read32(io.last().payload + 8) == client);
    assert(feed(R::Data, 0, 7, client, 18, 4) == Result::Stale); // Duplicate response.
    for (uint16_t sequence = 2; sequence <= 10; ++sequence) {
        assert(router->request(1, client, 0x2d, nullptr, 0, 5) == Result::OK);
        assert(io.last().confirm == (sequence == 5));
        response(sequence, sequence == 3 ? 16 : 0);
        assert(feed(R::Data, 0, 7, client, 18, 5) == Result::OK);
        assert(event.qmiError == (sequence == 3 ? 16 : 0));
        assert(event.result == (sequence == 3 ? Result::IO : Result::OK));
    }
    assert(router->request(1, client, 0x2d, nullptr, 0, 5) == Result::Busy); // Remote RX window full.
    memset(payload, 0, sizeof(payload)); write32(payload, R::ResumeTX); write32(payload + 4, 0); write32(payload + 8, 7);
    assert(feed(R::ResumeTX, 0, 7, client, 20, 5) == Result::OK);
    assert(router->request(1, client, 0x2d, nullptr, 0, 6, 10) == Result::OK);
    assert(router->close(1, client, 6) == Result::Busy);
    assert(router->progress(1, 16, event) == Result::OK && event.type == R::EventType::Timeout);
    assert(router->close(1, client, 16) == Result::OK && io.last().type == R::DeleteClient);
    uint32_t fresh;
    assert(router->open(1, QMI::DMS, 1, 0, fresh) == Result::OK && fresh > client);
    response(1);
    assert(feed(R::Data, 0, 7, client, 18, 16) == Result::NotReady);
    write32(payload, R::DeleteServer); write32(payload + 4, QMI::DMS);
    write32(payload + 8, 1); write32(payload + 12, 0); write32(payload + 16, 7);
    assert(feed(R::DeleteServer, 0, R::Control, R::Control, 20, 17) == Result::OK);
    assert(event.type == R::EventType::ServerRemoved);
    assert(router->request(1, fresh, 0x2d, nullptr, 0, 17) == Result::NotReady);
    router->disconnect();
    assert(router->start(2, 1, 0) == Result::OK);
    assert(router->feed(1, incoming.data(), 52, 0, event) == Result::Stale);

    // Version 3 is the wire discriminator of Linux's qrtr_hdr_v2, including
    // optional header bytes and the 16-bit control-port encoding.
    uint8_t v2[40]{}; v2[0] = 3; v2[1] = R::Hello; v2[3] = 4;
    write32(v2 + 4, 20); write16(v2 + 8, 0); write16(v2 + 10, 0xfffe);
    write16(v2 + 12, 1); write16(v2 + 14, 0xfffe); write32(v2 + 20, R::Hello);
    R::Packet parsed{};
    assert(R::parse(v2, sizeof(v2), parsed) == Result::OK && parsed.sourceNode == 0 && parsed.sourcePort == R::Control);
    v2[3] = 255; assert(R::parse(v2, sizeof(v2), parsed) == Result::Invalid);
    v2[3] = 4; v2[2] = 128; assert(R::parse(v2, sizeof(v2), parsed) == Result::Unsupported);
    response(1); QMI::Message valid;
    assert(QMI::parse(payload, 18, valid) == Result::OK);
    payload[8] = 127; assert(QMI::parse(payload, 18, valid) == Result::Invalid);
    response(1); payload[14] = 2;
    assert(QMI::parse(payload, 18, valid) == Result::OK);
    uint16_t error;
    assert(QMI::response(valid, error) == Result::Invalid); // Duplicate result TLV.
    std::puts("baseband QRTR discovery/client/flow/QMI correlation checks passed");
}
