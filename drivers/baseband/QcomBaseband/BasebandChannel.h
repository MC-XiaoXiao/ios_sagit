// SPDX-License-Identifier: GPL-2.0-only
// Serialized GLINK v1 channel; based on the Linux v6.12 wire ABI.
#pragma once
#include "BasebandGLINK.h"

namespace Baseband { namespace GLINK {
enum class ChannelState : uint32_t { Offline, Negotiating, Opening, Ready, Closed, Faulted };

// TX::send(data, length, readNotification) must return Busy only if nothing
// was published. A failure after publication must return Quarantined.
// The owner allocates this object on the heap, routes one whole validated
// frame at a time, and serializes feed/progress/queue/accept under its gate.
// A standalone channel negotiates VERSION for legacy callers. A multiplexer
// starts channels with the one connection's negotiated version/features.
template<class TX> class Channel {
    TX &tx;
    uint64_t epoch = 0, handshakeEnd = 0, sendEnd = 0, requestEnd = 0, lastTime = 0;
    uint16_t local = 0, peer = 0;
    uint32_t features = IntentReuse, sendingID = 0;
    ChannelState phase = ChannelState::Offline;
    Result failure = Result::OK;
    RemoteIntents remote;
    ReceiveIntent receive;
    uint8_t incoming[MaxDatagram]{}, outgoing[MaxDatagram]{}, wire[16 + 8192]{};
    size_t outgoingBytes = 0, outgoingOffset = 0;
    bool versionPending = false, versionAckPending = false, versionSent = false, negotiated = false;
    bool openSent = false, openAckPending = false, openAckSent = false, openAckReceived = false;
    bool advertised = false, donePending = false, closeAckPending = false;
    bool requestPending = false, requestSent = false, requestAckPending = false;
    uint32_t requestedBytes = 0;
    bool requestGranted = false;
    uint8_t name[32]{};
    size_t nameBytes = 0;
    bool delegated = false, remoteClosed = false, remoteCloseAckSent = false;
    bool localClosePending = false, localCloseSent = false, localCloseAcked = false;
    uint64_t closeEnd = 0;

    Result fail(Result why) {
        phase = ChannelState::Faulted; failure = why; return why;
    }
    Result publish(size_t bytes, bool notification = false) {
        const auto result = tx.send(wire, bytes, notification);
        if (result != Result::OK && result != Result::Busy) return fail(result);
        return result;
    }
    Result control(Command command, uint16_t id, uint32_t argument) {
        header(wire, command, id, argument); return publish(8);
    }
    bool operational() const { return phase == ChannelState::Ready; }
    Result clock(uint64_t now) {
        if (now < lastTime) return fail(Result::Invalid);
        lastTime = now;
        if ((phase == ChannelState::Negotiating || phase == ChannelState::Opening) && now >= handshakeEnd)
            return fail(Result::Timeout);
        if ((outgoingBytes && now >= sendEnd) || (requestSent && now >= requestEnd))
            return fail(Result::Timeout);
        if (phase == ChannelState::Closed && !retired() && now >= closeEnd) return fail(Result::Timeout);
        return Result::OK;
    }
    Result begin(uint64_t generation, uint16_t localChannel, uint64_t now, uint64_t timeout,
                 bool external, uint32_t negotiatedFeatures) {
        if (!nameBytes || !generation || generation <= epoch || !localChannel || !timeout || timeout > UINT64_MAX - now ||
            (negotiatedFeatures & ~IntentReuse)) return Result::Invalid;
        if (phase != ChannelState::Offline && phase != ChannelState::Closed && phase != ChannelState::Faulted)
            return Result::Busy;
        epoch = generation; local = localChannel; peer = 0; lastTime = now; handshakeEnd = now + timeout;
        features = negotiatedFeatures; failure = Result::OK; delegated = external;
        phase = external ? ChannelState::Opening : ChannelState::Negotiating;
        versionPending = !external; negotiated = external;
        versionAckPending = versionSent = openSent = openAckPending = openAckSent = openAckReceived = false;
        advertised = donePending = closeAckPending = requestPending = requestSent = requestAckPending = false;
        remoteClosed = remoteCloseAckSent = localClosePending = localCloseSent = localCloseAcked = false;
        outgoingBytes = outgoingOffset = sendingID = requestedBytes = 0; closeEnd = 0;
        return Result::OK;
    }
public:
    explicit Channel(TX &backend, const char *channelName = "IPCRTR") : tx(backend) {
        if (!channelName) return;
        for (size_t i = 0; i < sizeof(name); ++i) {
            name[i] = uint8_t(channelName[i]);
            if (!name[i]) { if (i) nameBytes = i + 1; return; }
        }
    }
    ChannelState state() const { return phase; }
    uint64_t generation() const { return epoch; }
    bool transmitIdle() const { return !outgoingBytes; }
    uint32_t negotiatedFeatures() const { return features; }
    uint16_t remoteChannel() const { return peer; }
    uint16_t localChannel() const { return local; }
    bool peerReserved() const { return peer && !(remoteClosed && remoteCloseAckSent); }
    bool retired() const {
        return phase == ChannelState::Closed && remoteClosed && remoteCloseAckSent &&
            (!openSent || (localCloseSent && localCloseAcked));
    }
    bool matchesOpen(const Frame &frame) const {
        if (!nameBytes || frame.command != Open || frame.payloadBytes != nameBytes || !frame.payload) return false;
        for (size_t i = 0; i < nameBytes; ++i) if (frame.payload[i] != name[i]) return false;
        return true;
    }
    // A new generation is allowed only after the owner has stopped the old
    // remote and retired its FIFO mappings. This does not reset shared RAM.
    Result start(uint64_t generation, uint16_t localChannel, uint64_t now, uint64_t timeout = 5000000) {
        return begin(generation, localChannel, now, timeout, false, IntentReuse);
    }
    Result startNegotiated(uint64_t generation, uint16_t localChannel, uint64_t now, uint32_t connectionFeatures,
                           uint64_t timeout = 5000000) {
        return begin(generation, localChannel, now, timeout, true, connectionFeatures);
    }
    Result feed(uint64_t generation, const Frame &frame, uint64_t now) {
        if (generation != epoch || !epoch) return Result::Stale;
        if (phase == ChannelState::Faulted) return failure;
        if (phase == ChannelState::Offline) return Result::NotReady;
        auto result = clock(now); if (result != Result::OK) return result;
        if (frame.command == CloseAck) {
            if (frame.channel != local) return Result::Unsupported;
            if (!localCloseSent || frame.argument) return fail(Result::Invalid);
            localCloseAcked = true; return Result::OK;
        }
        if (frame.command == Close && frame.channel == peer) {
            if (frame.argument || now > UINT64_MAX - 5000000) return fail(Result::Invalid);
            if (!remoteClosed) {
                remoteClosed = closeAckPending = true; phase = ChannelState::Closed;
                localClosePending = openSent && !localCloseSent;
                outgoingBytes = 0; requestPending = requestSent = false; closeEnd = now + 5000000;
            }
            return Result::OK;
        }
        // Closing endpoints discard already queued traffic; it cannot become
        // input to a new endpoint with a fresh local channel identity.
        if (phase == ChannelState::Closed) return Result::OK;
        if (frame.command == Version || frame.command == VersionAck) {
            if (delegated) return Result::Unsupported;
            // Mid-session renegotiation would invalidate intent reuse and
            // cannot be treated as a transparent remote restart.
            if (phase != ChannelState::Negotiating && phase != ChannelState::Opening)
                return fail(Result::Invalid);
            if (!frame.channel) return fail(Result::Unsupported);
            if (frame.command == Version) {
                if (openSent && (frame.channel != 1 || (features & frame.argument) != features))
                    return fail(Result::Invalid);
                if (frame.channel == 1 && (features & frame.argument) != features) {
                    features &= frame.argument;
                    negotiated = false; phase = ChannelState::Negotiating; versionPending = true;
                }
                versionAckPending = true; return Result::OK;
            }
            if (!versionSent || (openSent && (frame.channel != 1 || frame.argument != features)))
                return fail(Result::Invalid);
            if (frame.channel == 1 && frame.argument == features) {
                negotiated = true; phase = ChannelState::Opening; return Result::OK;
            }
            if (frame.channel == 1) features &= frame.argument;
            versionPending = true; return Result::OK;
        }
        if (frame.command == ReadNotify) return Result::OK; // Owner kicks RX after consume.
        if (frame.command == Open) {
            if (!matchesOpen(frame)) return Result::Unsupported;
            if (peer || !frame.channel) return fail(Result::Invalid);
            peer = frame.channel;
            result = remote.reset(epoch, peer);
            if (result == Result::OK) result = receive.bind(incoming, sizeof(incoming), 1, peer, epoch);
            if (result != Result::OK) return fail(result);
            openAckPending = true; return Result::OK;
        }
        if (frame.command == OpenAck) {
            if (frame.channel != local) return Result::Unsupported;
            if (!openSent || openAckReceived || frame.argument) return fail(Result::Invalid);
            openAckReceived = true; return Result::OK;
        }
        if (!peer || frame.channel != peer) return Result::Unsupported;
        // A peer may announce intents immediately after OPEN_ACK, before
        // our own receive-intent advertisement is scheduled.
        if (!openAckSent || !openAckReceived) return Result::NotReady;
        switch (frame.command) {
        case Intent:
            result = remote.announce(epoch, frame);
            break;
        case RXDone: case RXDoneReuse:
            if (outgoingBytes && frame.argument == sendingID) return fail(Result::Invalid);
            result = remote.complete(epoch, frame, features & IntentReuse);
            break;
        case TXData: case TXDataContinue:
            result = receive.receive(epoch, frame);
            break;
        case IntentRequest:
            if (requestAckPending) return Result::Busy;
            // One permanently provisioned receive slot. Never promise another
            // buffer while it is still advertised, receiving, or awaiting ACK.
            requestGranted = !advertised && !donePending && frame.argument <= sizeof(incoming);
            requestAckPending = true;
            return Result::OK;
        case IntentRequestAck:
            if (!requestSent) return fail(Result::Invalid);
            requestSent = false; requestedBytes = 0; return Result::OK;
        case Signals: return Result::OK;
        default: return Result::Unsupported;
        }
        return result == Result::OK ? result : fail(result);
    }
    // Executes at most one publication. Busy preserves the exact operation.
    Result progress(uint64_t now) {
        if (phase == ChannelState::Faulted) return failure;
        if (phase == ChannelState::Offline) return Result::NotReady;
        auto result = clock(now); if (result != Result::OK) return result;
        if (closeAckPending) {
            result = control(CloseAck, peer, 0);
            if (result == Result::OK) { closeAckPending = false; remoteCloseAckSent = true; }
            return result;
        }
        if (localClosePending) {
            result = control(Close, local, 0);
            if (result == Result::OK) { localClosePending = false; localCloseSent = true; }
            return result;
        }
        if (phase == ChannelState::Closed) return Result::NotReady;
        if (versionAckPending) {
            result = control(VersionAck, 1, features);
            if (result == Result::OK) versionAckPending = false;
            return result;
        }
        if (versionPending) {
            result = control(Version, 1, features);
            if (result == Result::OK) { versionPending = false; versionSent = true; }
            return result;
        }
        if (!negotiated) return Result::NotReady;
        if (openAckPending) {
            result = control(OpenAck, peer, 0);
            if (result == Result::OK) { openAckPending = false; openAckSent = true; }
            return result;
        }
        if (!openSent) {
            header(wire, Open, local, uint32_t(nameBytes));
            for (size_t i = 0; i < nameBytes; ++i) wire[8 + i] = name[i];
            result = publish(8 + nameBytes);
            if (result == Result::OK) openSent = true;
            return result;
        }
        if (!openAckSent || !openAckReceived) return Result::NotReady;
        if (donePending) {
            result = control(features & IntentReuse ? RXDoneReuse : RXDone, local, 1);
            if (result == Result::OK) {
                result = receive.acknowledged(epoch, features & IntentReuse);
                if (result != Result::OK) return fail(result);
                donePending = false; advertised = features & IntentReuse;
            }
            return result;
        }
        if (!advertised) {
            header(wire, Intent, local, 1);
            write32(wire + 8, sizeof(incoming)); write32(wire + 12, 1);
            result = publish(16);
            if (result == Result::OK) {
                result = receive.markAdvertised(epoch);
                if (result != Result::OK) return fail(result);
                advertised = true; phase = ChannelState::Ready;
            }
            return result;
        }
        if (requestAckPending) {
            result = control(IntentRequestAck, local, requestGranted);
            if (result == Result::OK) requestAckPending = false;
            return result;
        }
        if (requestPending) {
            if (now > UINT64_MAX - 5000000) return fail(Result::Invalid);
            result = control(IntentRequest, local, requestedBytes);
            if (result == Result::OK) {
                requestPending = false; requestSent = true; requestEnd = now + 5000000;
            }
            return result;
        }
        if (!outgoingBytes) return Result::NotReady;
        const size_t chunk = outgoingBytes - outgoingOffset > 8192 ? 8192 : outgoingBytes - outgoingOffset;
        header(wire, outgoingOffset ? TXDataContinue : TXData, local, sendingID);
        write32(wire + 8, uint32_t(chunk));
        write32(wire + 12, uint32_t(outgoingBytes - outgoingOffset - chunk));
        for (size_t i = 0; i < chunk; ++i) wire[16 + i] = outgoing[outgoingOffset + i];
        result = publish(16 + chunk);
        if (result == Result::OK) {
            outgoingOffset += chunk;
            if (outgoingOffset == outgoingBytes) outgoingBytes = 0;
        }
        return result;
    }
    // OK means accepted into retained storage, not remotely acknowledged.
    // Busy with no intent schedules a request and does not accept the data.
    Result queue(uint64_t generation, const void *data, size_t bytes, uint64_t now) {
        if (generation != epoch || !epoch) return Result::Stale;
        if (!operational()) return Result::NotReady;
        if (!data || !bytes || bytes > sizeof(outgoing) || now > UINT64_MAX - 5000000)
            return Result::Invalid;
        auto result = clock(now); if (result != Result::OK) return result;
        if (outgoingBytes) return Result::Busy;
        uint32_t id = 0;
        result = remote.reserve(epoch, bytes, id);
        if (result == Result::Busy && !requestPending && !requestSent) {
            requestedBytes = uint32_t(bytes); requestPending = true;
        }
        if (result != Result::OK) return result;
        const auto *p = static_cast<const uint8_t *>(data);
        for (size_t i = 0; i < bytes; ++i) outgoing[i] = p[i];
        outgoingBytes = bytes; outgoingOffset = 0; sendingID = id; sendEnd = now + 5000000;
        return Result::OK;
    }
    Result datagram(uint64_t generation, const uint8_t *&data, size_t &bytes) const {
        data = nullptr; bytes = 0;
        if (generation != epoch || !epoch) return Result::Stale;
        if (!operational() || donePending) return Result::NotReady;
        return receive.datagram(generation, data, bytes);
    }
    // The consumer must copy/deliver first. Repeated calls cannot queue two
    // RX_DONEs, and a full transmit FIFO retains the receive buffer.
    Result accept(uint64_t generation) {
        const uint8_t *data = nullptr; size_t bytes = 0;
        auto result = datagram(generation, data, bytes);
        if (result == Result::OK) donePending = true;
        return result;
    }
    Result close(uint64_t generation, uint64_t now) {
        if (!epoch || generation != epoch) return Result::Stale;
        if (phase == ChannelState::Faulted) return failure;
        if (phase == ChannelState::Closed) return Result::OK;
        if (!openSent || !peer) return Result::NotReady;
        auto r = clock(now); if (r != Result::OK) return r;
        if (now > UINT64_MAX - 5000000) return Result::Invalid;
        phase = ChannelState::Closed; localClosePending = !localCloseSent;
        outgoingBytes = 0; requestPending = requestSent = false; closeEnd = now + 5000000;
        return Result::OK;
    }
};
}} // namespace Baseband::GLINK
