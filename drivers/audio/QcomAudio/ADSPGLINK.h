// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "ADSPProtocol.h"
#include "../../baseband/QcomBaseband/BasebandGLINK.h"

namespace ADSP {
// One APR rpmsg channel over the intent-based native GLINK transport. IO owns
// the real SMEM rings (send/available/peek/consume/kick); Sink copies/accepts APR
// packets synchronously. Calls are serialized by the kernel adapter.
template<class IO, class Sink> class Link {
    IO &io;
    Sink &sink;
    static constexpr uint16_t Local = 1;
    static constexpr unsigned ReceiveSlots = 8, ControlSlots = 32;
    struct Control { uint8_t data[80]{}; size_t bytes = 0; } controls[ControlSlots];
    struct Receive {
        Baseband::GLINK::ReceiveIntent intent;
        uint8_t data[MaxPacket]{};
        bool idle = false;
    } receives[ReceiveSlots];
    Baseband::GLINK::RemoteIntents remoteIntents;
    uint64_t epoch = 0;
    uint16_t remote = 0;
    uint32_t features = 1;
    unsigned first = 0, count = 0;
    bool failed = false, negotiated = false, openSent = false, openAck = false;
    bool advertised = false, intentRequested = false;
    bool closing = false, closeAck = false, remoteClosed = false;
    uint8_t rxFrame[MaxPacket + 16]{}, txFrame[MaxPacket + 16]{};
    Result fault(Result r) { failed = true; return r; }
    Result queue(uint16_t cmd, uint16_t channel, uint32_t argument,
                 const void *extra = nullptr, size_t bytes = 0) {
        if (count == ControlSlots) return Result::Busy;
        if (bytes > 72 || (bytes && !extra)) return Result::Invalid;
        auto &c = controls[(first + count) % ControlSlots];
        Baseband::GLINK::header(c.data, Baseband::GLINK::Command(cmd), channel, argument);
        const auto *p = static_cast<const uint8_t *>(extra);
        for (size_t i = 0; i < bytes; ++i) c.data[8 + i] = p[i];
        c.bytes = 8 + bytes; ++count;
        return Result::OK;
    }
    Result flush() {
        while (count) {
            const auto &c = controls[first];
            Result r = io.send(c.data, c.bytes);
            if (r == Result::Busy) return r;
            if (r != Result::OK) return fault(r);
            first = (first + 1) % ControlSlots; --count;
        }
        return Result::OK;
    }
    Result advanceOpen() {
        if (negotiated && !openSent) {
            static const char name[] = "apr_audio_svc";
            Result r = queue(Baseband::GLINK::Open, Local, sizeof(name), name, sizeof(name));
            if (r != Result::OK) return r;
            openSent = true;
        }
        if (negotiated && openAck && remote && !advertised) {
            uint8_t pairs[ReceiveSlots * 8]{};
            for (unsigned i = 0; i < ReceiveSlots; ++i) {
                write32(pairs + i * 8, MaxPacket); write32(pairs + i * 8 + 4, i + 1);
            }
            Result r = queue(Baseband::GLINK::Intent, Local, ReceiveSlots, pairs, sizeof(pairs));
            if (r != Result::OK) return r;
            for (auto &receive : receives) {
                if (receive.intent.markAdvertised(epoch) != Result::OK) return fault(Result::Invalid);
                receive.idle = true;
            }
            advertised = true;
        }
        return Result::OK;
    }
    Result handle(const Baseband::GLINK::Frame &f) {
        using namespace Baseband::GLINK;
        // Reserve all control replies before any receive-side state changes.
        // Backpressure leaves the RX cursor and intent contents unchanged.
        if (ControlSlots - count < 2) return Result::Busy;
        switch (f.command) {
        case Version:
            if (!f.channel) return Result::Unsupported;
            if (ready()) return Result::Stale; // peer restarted; no silent epoch reuse
            features &= f.argument & IntentReuse;
            return queue(VersionAck, 1, features);
        case VersionAck:
            if (f.channel != 1 || (f.argument & ~IntentReuse)) return Result::Unsupported;
            if (f.argument != features) {
                features &= f.argument;
                return queue(Version, 1, features);
            }
            negotiated = true;
            return Result::OK;
        case Open: {
            static const char name[] = "apr_audio_svc";
            bool match = f.payloadBytes == sizeof(name);
            for (size_t i = 0; match && i < sizeof(name); ++i) match = f.payload[i] == uint8_t(name[i]);
            if (!match) return Result::OK; // other rpmsg services are not claimed
            if (remote) return Result::Invalid;
            Result r = queue(OpenAck, f.channel, 0);
            if (r != Result::OK) return r;
            remote = f.channel;
            if (remoteIntents.reset(epoch, remote) != Result::OK) return Result::Invalid;
            for (unsigned i = 0; i < ReceiveSlots; ++i)
                if (receives[i].intent.bind(receives[i].data, MaxPacket, i + 1, remote, epoch) != Result::OK)
                    return Result::Invalid;
            return Result::OK;
        }
        case OpenAck:
            if (!openSent || f.channel != Local || f.argument) return Result::Invalid;
            openAck = true; return Result::OK;
        case ReadNotify:
            if (f.channel || f.argument) return Result::Invalid;
            return Result::OK; // IO.consume publishes tail before the kick
        case Baseband::GLINK::Close:
            if (f.channel == remote) {
                if (!closing) return Result::Stale;
                if (remoteClosed) return Result::Invalid;
                Result r = queue(CloseAck, remote, 0);
                if (r == Result::OK) remoteClosed = true;
                return r;
            }
            return queue(CloseAck, f.channel, 0);
        case CloseAck:
            if (!closing || f.channel != Local || f.argument) return Result::Invalid;
            closeAck = true; return Result::OK;
        default: break;
        }
        if (!remote || f.channel != remote) return Result::Invalid;
        switch (f.command) {
        case Intent: {
            Result r = remoteIntents.announce(epoch, f);
            if (r == Result::OK) intentRequested = false;
            return r;
        }
        case RXDone: case RXDoneReuse:
            return remoteIntents.complete(epoch, f, features & IntentReuse);
        case IntentRequest: {
            bool free = false;
            for (const auto &r : receives) free = free || r.idle;
            // Existing credits are advertised at channel open. When all are
            // occupied, deny rather than promise an allocation we cannot own.
            return queue(IntentRequestAck, Local, free && f.argument <= MaxPacket ? 1 : 0);
        }
        case IntentRequestAck:
            if (!intentRequested) return Result::Invalid;
            // Only an actual INTENT announcement creates usable credit.
            if (!f.argument) intentRequested = false;
            return Result::OK;
        case Signals: return Result::OK;
        case TXData: case TXDataContinue: {
            if (!advertised || !f.argument || f.argument > ReceiveSlots) return Result::Invalid;
            auto &r = receives[f.argument - 1];
            Result result = r.intent.receive(epoch, f);
            if (result != Result::OK) return result;
            r.idle = false;
            const uint8_t *packet = nullptr; size_t bytes = 0;
            result = r.intent.datagram(epoch, packet, bytes);
            if (result == Result::NotReady) return Result::OK;
            if (result != Result::OK) return result;
            result = sink.receive(packet, bytes);
            if (result != Result::OK && result != Result::Stale) return result;
            const bool reuse = features & IntentReuse;
            result = queue(reuse ? RXDoneReuse : RXDone, Local, f.argument);
            if (result != Result::OK) return result;
            if (r.intent.acknowledged(epoch, reuse) != Result::OK) return Result::Invalid;
            if (!reuse) {
                uint8_t pair[8]; write32(pair, MaxPacket); write32(pair + 4, f.argument);
                result = queue(Intent, Local, 1, pair, sizeof(pair));
                if (result != Result::OK || r.intent.markAdvertised(epoch) != Result::OK) return Result::Invalid;
            }
            r.idle = true;
            return Result::OK;
        }
        default: return Result::Unsupported;
        }
    }
public:
    Link(IO &backend, Sink &receiver) : io(backend), sink(receiver) {}
    bool ready() const { return !failed && !closing && negotiated && openAck && remote && advertised; }
    bool closed() const { return !failed && closing && closeAck && remoteClosed && !count; }
    bool quarantined() const { return failed; }
    uint64_t generation() const { return epoch; }
    Result begin(uint64_t generation) {
        if (epoch || !generation) return Result::Invalid;
        epoch = generation;
        return queue(Baseband::GLINK::Version, 1, features);
    }
    Result close() {
        if (failed) return Result::Quarantined;
        if (closing) return Result::OK;
        if (!openSent || !openAck || !remote) return Result::NotReady;
        Result r = queue(Baseband::GLINK::Close, Local, 0);
        if (r == Result::OK) closing = true;
        return r;
    }
    Result poll(unsigned budget = 32) {
        if (failed) return Result::Quarantined;
        if (!epoch) return Result::NotReady;
        for (unsigned i = 0; i < budget; ++i) {
            Result r = advanceOpen();
            if (r != Result::OK && r != Result::Busy) return fault(r);
            r = flush();
            if (r != Result::OK && r != Result::Busy) return r;
            size_t available = 0;
            r = io.available(available);
            if (r != Result::OK) return fault(r);
            if (available < 8) return Result::OK;
            size_t bytes = available < sizeof(rxFrame) ? available : sizeof(rxFrame);
            r = io.peek(rxFrame, bytes);
            if (r != Result::OK) return fault(r);
            Baseband::GLINK::Frame frame{};
            r = Baseband::GLINK::parse(rxFrame, bytes, frame);
            if (r == Result::NotReady) {
                // A chunk larger than our bounded staging buffer can never
                // become acceptable by waiting for more FIFO bytes.
                if (bytes >= 16 && (read16(rxFrame) == Baseband::GLINK::TXData ||
                    read16(rxFrame) == Baseband::GLINK::TXDataContinue) &&
                    read32(rxFrame + 8) > MaxPacket) return fault(Result::Invalid);
                return Result::OK;
            }
            if (r != Result::OK) return fault(r);
            r = handle(frame);
            if (r == Result::Busy) return Result::OK;
            if (r != Result::OK) return fault(r);
            r = io.consume(frame.wireBytes);
            if (r != Result::OK) return fault(r);
        }
        return Result::OK; // bounded work; IRQ/poll will resume draining
    }
    Result send(const void *data, size_t bytes) {
        if (!ready()) return failed ? Result::Quarantined : Result::NotReady;
        if (!data || !bytes || bytes > MaxPacket) return Result::Invalid;
        Result r = flush();
        if (r != Result::OK) return r;
        uint32_t id = 0;
        r = remoteIntents.reserve(epoch, bytes, id);
        if (r == Result::Busy && !intentRequested) {
            r = queue(Baseband::GLINK::IntentRequest, Local, bytes);
            if (r != Result::OK) return r;
            intentRequested = true;
            r = flush();
            if (r != Result::OK && r != Result::Busy) return r;
            return Result::Busy;
        }
        if (r != Result::OK) return r;
        Baseband::GLINK::header(txFrame, Baseband::GLINK::TXData, Local, id);
        write32(txFrame + 8, bytes); write32(txFrame + 12, 0);
        const auto *p = static_cast<const uint8_t *>(data);
        for (size_t i = 0; i < bytes; ++i) txFrame[16 + i] = p[i];
        r = io.send(txFrame, 16 + bytes);
        if (r == Result::Busy) remoteIntents.cancelUnpublished(epoch, id);
        else if (r != Result::OK) return fault(r); // ambiguous publication is never retried
        return r;
    }
};
}
