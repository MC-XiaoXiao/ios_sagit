// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "WLANWLFW.h"

namespace WLAN { namespace QRTR {
constexpr uint32_t ControlPort = 0xfffffffe, BroadcastNode = 0xffffffff;
constexpr uint32_t Data = 1, Hello = 2, Bye = 3, NewServer = 4, DeleteServer = 5, DeleteClient = 6, ResumeTX = 7, NewLookup = 10;
constexpr size_t HeaderBytes = 32, MaximumPacket = HeaderBytes + QMI::HeaderBytes + QMI::MaximumPayload + 3;
struct Packet {
    uint32_t type, sourceNode, sourcePort, destinationNode, destinationPort;
    bool confirm;
    const uint8_t *payload; uint32_t length;
};
inline Result parse(const void *data,size_t bytes,Packet &packet) {
    if (!data || bytes < HeaderBytes) return Result::Invalid;
    const auto *p = static_cast<const uint8_t *>(data);
    if (read32(p) != 1) return Result::Unsupported;
    const uint32_t length = read32(p + 20), confirm = read32(p + 16), type = read32(p + 4);
    if (confirm > 1 || length > MaximumPacket - HeaderBytes ||
        ((uint64_t(length) + 3) & ~3ULL) != bytes - HeaderBytes || !type) return Result::Invalid;
    packet = {type,read32(p + 8),read32(p + 12),read32(p + 24),read32(p + 28),confirm != 0,p + HeaderBytes,length};
    return Result::Success;
}
inline Result encode(void *data,size_t capacity,const Packet &packet,size_t &written) {
    written = 0;
    const uint64_t padded = (uint64_t(packet.length) + 3) & ~3ULL;
    if (!data || !packet.type || (packet.length && !packet.payload) || capacity < HeaderBytes ||
        padded > capacity - HeaderBytes || packet.length > MaximumPacket - HeaderBytes) return Result::Invalid;
    auto *p = static_cast<uint8_t *>(data);
    // The payload must have distinct storage. Send and receive buffers in
    // Session are separate, including when a receive requires a RESUME_TX.
    write32(p,1); write32(p + 4,packet.type); write32(p + 8,packet.sourceNode); write32(p + 12,packet.sourcePort);
    write32(p + 16,packet.confirm); write32(p + 20,packet.length);
    write32(p + 24,packet.destinationNode); write32(p + 28,packet.destinationPort);
    for (size_t i = 0; i < padded; ++i) p[HeaderBytes + i] = i < packet.length ? packet.payload[i] : 0;
    written = HeaderBytes + size_t(padded); return Result::Success;
}

// Endpoint supplies fresh node-wide ports, bounded GLINK datagrams, a
// monotonic millisecond clock, and SCM MSA ownership operations. This is the QRTR/QMI layer, not an RPM
// intentless GLINK substitute. Its buffers must live off the kernel stack.
template<class Endpoint> class Session {
    Endpoint &io;
    uint32_t node, port = 0, lastPort = 0, remoteNode = 0, remotePort = 0;
    uint64_t epoch = 0, lastEpoch = 0;
    bool ready = false, firmwareReady = false, msaReady = false;
    uint8_t sendBuffer[MaximumPacket]{}, receiveBuffer[MaximumPacket]{}, qmi[QMI::HeaderBytes + QMI::MaximumPayload]{};
    QMI::Transaction transaction;
    Result deadline(uint64_t timeout,uint64_t &end) {
        const auto now = io.now();
        if (!timeout || timeout > UINT64_MAX - now) return Result::Invalid;
        end = now + timeout; return Result::Success;
    }
    Result send(uint32_t type,uint32_t peerNode,uint32_t peerPort,const void *payload,uint32_t bytes,uint64_t end,uint32_t sourcePort = 0) {
        size_t written = 0;
        const Packet packet{type,node,sourcePort ? sourcePort : port,peerNode,peerPort,false,static_cast<const uint8_t *>(payload),bytes};
        auto result = encode(sendBuffer,sizeof(sendBuffer),packet,written);
        return result == Result::Success ? io.send(sendBuffer,written,end) : result;
    }
    Result receive(uint64_t end,Packet &packet) {
        if (io.now() >= end) return Result::Timeout;
        size_t bytes = 0;
        auto result = io.receive(receiveBuffer,sizeof(receiveBuffer),bytes,end);
        if (result != Result::Success) return result;
        if (io.now() >= end) return Result::Timeout;
        if (bytes > sizeof(receiveBuffer)) return Result::Invalid;
        result = parse(receiveBuffer,bytes,packet);
        if (result != Result::Success) return result;
        if (packet.destinationNode != node && packet.destinationNode != BroadcastNode) return Result::NotReady;
        if (packet.destinationPort != port && packet.destinationPort != ControlPort) return Result::NotReady;
        if (packet.confirm) {
            if (packet.type != Data || packet.destinationPort != port) return Result::Invalid;
            uint8_t resume[20]{};
            write32(resume,ResumeTX); write32(resume + 4,node); write32(resume + 8,port);
            result = send(ResumeTX,packet.sourceNode,packet.sourcePort,resume,sizeof(resume),end);
        }
        return result;
    }
    Result control(const Packet &packet) {
        if (packet.sourcePort != ControlPort || packet.length != 20 || read32(packet.payload) != packet.type) return Result::Invalid;
        const auto *p = packet.payload;
        if (packet.type == Bye && ready && packet.sourceNode == remoteNode) { disconnect(); return Result::Fault; }
        if (packet.type != NewServer && packet.type != DeleteServer) return Result::NotReady;
        if (read32(p + 4) != QMI::Service || read32(p + 8) != QMI::Version ||
            !packet.sourceNode || packet.sourceNode == BroadcastNode || read32(p + 12) != packet.sourceNode ||
            !read32(p + 16) || read32(p + 16) >= ControlPort) return Result::NotReady;
        if (packet.type == DeleteServer) {
            if (ready && remoteNode == read32(p + 12) && remotePort == read32(p + 16)) { disconnect(); return Result::Fault; }
            return Result::NotReady;
        }
        if (ready && (remoteNode != read32(p + 12) || remotePort != read32(p + 16))) return Result::Busy;
        remoteNode = read32(p + 12); remotePort = read32(p + 16); ready = true; return Result::Success;
    }
    Result message(const Packet &packet,QMI::Message &out) {
        if (!ready || packet.type != Data || packet.sourceNode != remoteNode || packet.sourcePort != remotePort ||
            packet.destinationNode != node || packet.destinationPort != port) return Result::NotReady;
        auto result = QMI::parse(packet.payload,packet.length,out);
        if (result != Result::Success) return result;
        if (out.type == QMI::Indication) {
            if (out.transaction || ((out.id == QMI::FirmwareReady || out.id == QMI::MSAReadyIndication) && out.length)) return Result::Invalid;
            if (out.id == QMI::FirmwareReady) firmwareReady = true;
            else if (out.id == QMI::MSAReadyIndication) msaReady = true;
        }
        return Result::Success;
    }
public:
    Session(Endpoint &endpoint,uint32_t localNode) : io(endpoint),node(localNode) {}
    void disconnect() {
        transaction.cancel(); ready = firmwareReady = msaReady = false; epoch = 0;
        port = remoteNode = remotePort = 0;
    }
    bool connected() const { return ready; }
    bool hasPort() const { return port != 0; }
    Result connect(uint64_t generation,uint64_t timeout = 5000) {
        if (ready || transaction.busy()) return Result::Busy;
        if (!generation || generation <= lastEpoch || !node || node == BroadcastNode) return Result::Invalid;
        uint64_t end; auto result = deadline(timeout,end); if (result != Result::Success) return result;
        uint32_t fresh = 0;
        result = io.allocatePort(node,generation,fresh);
        if (result != Result::Success) return result;
        if (!fresh || fresh <= lastPort || fresh >= ControlPort) return Result::Invalid;
        port = lastPort = fresh;
        epoch = lastEpoch = generation;
        // Raw GLINK discovery uses HELLO + NEW_SERVER announcements.
        // NEW_LOOKUP is a local nameservice operation, not a modem request.
        uint8_t hello[20]{}; write32(hello,Hello);
        result = send(Hello,BroadcastNode,ControlPort,hello,sizeof(hello),end,ControlPort);
        if (result != Result::Success) return result;
        while (io.now() < end) {
            Packet packet{}; result = receive(end,packet);
            if (result == Result::NotReady) continue;
            if (result != Result::Success) return result;
            result = control(packet);
            if (result == Result::Success) return result;
            if (result != Result::NotReady) return result;
        }
        return Result::Timeout;
    }
    Result close() {
        if (!port) return Result::Success;
        uint64_t end; auto result = deadline(5000,end); if (result != Result::Success) return result;
        uint8_t client[20]{}; write32(client,DeleteClient); write32(client + 4,node); write32(client + 8,port);
        result = send(DeleteClient,BroadcastNode,ControlPort,client,sizeof(client),end,ControlPort);
        if (result == Result::Success) disconnect();
        return result;
    }
    Result exchange(uint64_t generation,uint16_t id,const void *payload,size_t bytes,QMI::Message &response) {
        if (!ready || generation != epoch) return Result::NotReady;
        uint64_t end; auto result = deadline(5000,end); if (result != Result::Success) return result;
        uint16_t sequence = 0;
        result = transaction.begin(epoch,id,io.now(),5000,sequence);
        if (result != Result::Success) return result;
        size_t written = 0; result = QMI::encodeRequest(qmi,sizeof(qmi),sequence,id,payload,bytes,written);
        if (result == Result::Success) result = send(Data,remoteNode,remotePort,qmi,uint32_t(written),end);
        if (result != Result::Success) { transaction.cancel(); return result; }
        while (io.now() < end) {
            Packet packet{}; result = receive(end,packet);
            if (result == Result::NotReady) continue;
            if (result != Result::Success) break;
            if (packet.type != Data) {
                result = control(packet); if (result == Result::Fault) break;
                continue;
            }
            QMI::Message incoming{}; result = message(packet,incoming);
            if (result == Result::NotReady || (result == Result::Success && incoming.type == QMI::Indication)) continue;
            if (result != Result::Success) break;
            uint16_t error = 0; result = transaction.receive(epoch,io.now(),incoming,error);
            if (result == Result::NotReady) continue;
            if (result != Result::Invalid) { response = incoming; return result; }
            break;
        }
        transaction.cancel(); return io.now() >= end || result == Result::Success ? Result::Timeout : result;
    }
    Result waitIndication(uint64_t generation,uint16_t id) {
        if (!ready || generation != epoch) return Result::NotReady;
        if (id != QMI::FirmwareReady && id != QMI::MSAReadyIndication) return Result::Invalid;
        uint64_t end; auto result = deadline(10000,end); if (result != Result::Success) return result;
        while (!(id == QMI::FirmwareReady ? firmwareReady : msaReady)) {
            Packet packet{}; result = receive(end,packet);
            if (result == Result::NotReady) continue;
            if (result != Result::Success) return result;
            if (packet.type != Data) { result = control(packet); if (result == Result::Fault) return result; continue; }
            QMI::Message incoming{}; result = message(packet,incoming);
            if (result != Result::Success && result != Result::NotReady) return result;
        }
        return Result::Success;
    }
    Result shareMSA(uint64_t generation,const MSARegion *regions,unsigned count) {
        return ready && generation == epoch ? io.shareMSA(generation,regions,count) : Result::NotReady;
    }
    Result unshareMSA(uint64_t generation,const MSARegion *regions,unsigned count) {
        // Reclaim can be attempted after service loss, but only the endpoint's
        // secure ownership operation can confirm that remote access is gone.
        return generation == lastEpoch ? io.unshareMSA(generation,regions,count) : Result::NotReady;
    }
};
}}
