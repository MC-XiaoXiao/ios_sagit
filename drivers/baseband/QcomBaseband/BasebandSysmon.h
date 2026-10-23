// SPDX-License-Identifier: GPL-2.0-only
// MSM8998 modem sysmon client, Linux v6.12 qcom_sysmon.c / qcom_q6v5_mss.c.
#pragma once
#include "BasebandQRTR.h"

namespace Baseband { namespace Sysmon {
constexpr uint32_t Service = 43, ModemInstance = 0x12;
constexpr uint16_t Shutdown = 0x21, SubsystemEvent = 0x23;
constexpr uint64_t DiscoveryUS = 500000, TimeoutUS = 5000000;
constexpr const char *ChannelName = "sys_mon";
enum class Event : uint32_t { BeforePowerUp, AfterPowerUp, BeforeShutdown, AfterShutdown };
enum class Driven : uint32_t { Forced, Graceful };
enum class Mode : uint32_t { None, Legacy, SSCTLv1, SSCTLv2 };
enum class Phase : uint32_t { Idle, Selecting, Sending, Waiting, Complete, Failed };
enum class Operation : uint32_t { Stop, Notify };
constexpr const char *EventNames[] = {"before_powerup", "after_powerup", "before_shutdown", "after_shutdown"};

// One serialized transaction. QRTR responses use its fresh client port and
// QMI TID; legacy ACKs have no TID, so a timed-out legacy channel is tainted
// until it has a fresh GLINK identity. Nothing synthesizes a remote ACK.
template<class Link> class Client {
    Link &link;
    QRTR::Router<Link> &router;
    uint64_t epoch = 0, lastTime = 0, deadline = 0, discoveryEnd = 0;
    Phase phase = Phase::Idle;
    Mode transport = Mode::None;
    Operation operation = Operation::Stop;
    Result result = Result::NotReady;
    uint16_t error = 0, channel = 0, poisonedLegacy = 0;
    uint32_t port = 0;
    bool active = false, lookedUp = false, indication = false;
    uint8_t legacy[64]{}, payload[40]{};
    size_t legacyBytes = 0, payloadBytes = 0;
    Result finish(Result value) {
        result = value; phase = value == Result::OK ? Phase::Complete : Phase::Failed;
        if (value != Result::OK && transport == Mode::Legacy && channel) poisonedLegacy = channel;
        return value == Result::Quarantined ? value : Result::OK;
    }
    bool pending() const { return phase == Phase::Selecting || phase == Phase::Sending || phase == Phase::Waiting; }
    Result clock(uint64_t now) {
        if (now < lastTime) return finish(Result::Stale);
        lastTime = now;
        return pending() && now >= deadline ? finish(Result::Timeout) : Result::OK;
    }
    Result begin(uint64_t generation, uint64_t now, uint64_t timeout, Operation op) {
        if (!active || generation != epoch) return Result::Stale;
        if (!timeout || timeout > UINT64_MAX - now || now < lastTime) return Result::Invalid;
        if (pending()) return Result::Busy;
        if (port) {
            const auto r = router.close(epoch, port, now); if (r != Result::OK) return r;
            port = 0;
        }
        lastTime = now; deadline = now + timeout;
        discoveryEnd = now + (timeout < DiscoveryUS ? timeout : DiscoveryUS);
        operation = op; phase = Phase::Selecting; transport = Mode::None;
        lookedUp = indication = false; result = Result::Busy; error = channel = 0;
        return Result::OK;
    }
    Result select(uint64_t now) {
        if (!lookedUp) {
            const auto r = router.lookup(epoch, Service, 0, 0, now);
            if (r == Result::OK) lookedUp = true;
            else if (r != Result::Busy && r != Result::NotReady) return finish(r);
        }
        auto r = router.open(epoch, Service, 2, ModemInstance, port);
        if (r == Result::OK) transport = Mode::SSCTLv2;
        else if (r == Result::NotReady && operation == Operation::Stop) {
            r = router.open(epoch, Service, 1, 0, port);
            if (r == Result::OK) transport = Mode::SSCTLv1;
        }
        if (r == Result::OK) {
            channel = link.channelIdentity();
            if (!channel) return finish(Result::Stale);
            phase = Phase::Sending; return Result::OK;
        }
        if (r != Result::NotReady) return finish(r); // An ambiguous match is not a version-1 fallback.
        if (now < discoveryEnd) return Result::NotReady;
        if (!link.connected(ChannelName)) return finish(Result::NotReady);
        channel = link.channelIdentity(ChannelName);
        if (!channel || channel == poisonedLegacy) return finish(Result::Stale);
        transport = Mode::Legacy; phase = Phase::Sending; return Result::OK;
    }
public:
    Client(Link &endpoint, QRTR::Router<Link> &qrtr) : link(endpoint), router(qrtr) {}
    Result start(uint64_t generation, uint64_t now) {
        if (active || !generation || generation <= epoch) return Result::Invalid;
        epoch = generation; lastTime = now; active = true; phase = Phase::Idle;
        transport = Mode::None; port = 0; channel = poisonedLegacy = error = 0;
        indication = false; result = Result::NotReady; return Result::OK;
    }
    Result shutdown(uint64_t generation, uint64_t now, uint64_t timeout = TimeoutUS) {
        const auto r = begin(generation, now, timeout, Operation::Stop);
        if (r != Result::OK) return r;
        const uint8_t request[] = "ssr:shutdown";
        for (size_t i = 0; i < sizeof(request); ++i) legacy[i] = request[i];
        legacyBytes = sizeof(request); payloadBytes = 0; return Result::OK;
    }
    Result notify(uint64_t generation, const char *subject, Event event, Driven driven,
                  uint64_t now, uint64_t timeout = TimeoutUS) {
        const auto eventID = uint32_t(event), drivenID = uint32_t(driven);
        if (!subject || eventID > 3 || drivenID > 1) return Result::Invalid;
        size_t length = 0;
        for (; length < 16 && subject[length]; ++length) {
            const auto c = subject[length];
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return Result::Invalid;
        }
        if (!length || length > 15) return Result::Invalid;
        const char own[] = "modem"; bool self = length == sizeof(own) - 1;
        for (size_t i = 0; self && i < length; ++i) self &= subject[i] == own[i];
        if (self) return Result::Invalid; // Events describe another processor's actual lifecycle.
        const auto r = begin(generation, now, timeout, Operation::Notify); if (r != Result::OK) return r;
        legacyBytes = 0;
        const char prefix[] = "ssr:";
        for (unsigned i = 0; i < 4; ++i) legacy[legacyBytes++] = uint8_t(prefix[i]);
        for (size_t i = 0; i < length; ++i) legacy[legacyBytes++] = uint8_t(subject[i]);
        legacy[legacyBytes++] = ':';
        for (size_t i = 0; EventNames[eventID][i]; ++i) legacy[legacyBytes++] = uint8_t(EventNames[eventID][i]);
        // Event text has no trailing NUL; the shutdown request above does.
        payload[0] = 1; write16(payload + 1, uint16_t(length + 1)); payload[3] = uint8_t(length);
        for (size_t i = 0; i < length; ++i) payload[4 + i] = uint8_t(subject[i]);
        size_t at = 4 + length; payload[at] = 2; write16(payload + at + 1, 4); write32(payload + at + 3, eventID);
        at += 7; payload[at] = 0x10; write16(payload + at + 1, 4); write32(payload + at + 3, drivenID);
        payloadBytes = at + 7; return Result::OK;
    }
    Result progress(uint64_t generation, uint64_t now) {
        if (!active || generation != epoch) return Result::Stale;
        auto r = clock(now); if (r != Result::OK) return r;
        const uint8_t *data = nullptr; size_t bytes = 0;
        r = link.datagramOn(epoch, ChannelName, data, bytes);
        if (r == Result::OK) {
            if (transport == Mode::Legacy && phase == Phase::Waiting) {
                const uint8_t ack[] = "ssr:ack";
                bool valid = bytes >= sizeof(ack) && link.channelIdentity(ChannelName) == channel;
                for (size_t i = 0; valid && i < sizeof(ack); ++i) valid &= data[i] == ack[i];
                finish(valid ? Result::OK : Result::Invalid);
            }
            r = link.acceptOn(epoch, ChannelName);
            if (r != Result::OK) return r;
        } else if (r != Result::NotReady) return r;
        if (!pending()) return Result::NotReady;
        if (phase == Phase::Selecting) {
            r = select(now); if (r != Result::OK || !pending()) return r;
        }
        const bool legacyMode = transport == Mode::Legacy;
        if (link.channelIdentity(legacyMode ? ChannelName : "IPCRTR") != channel ||
            (!legacyMode && !router.clientReady(epoch, port))) return finish(Result::Stale);
        if (phase != Phase::Sending) return Result::NotReady;
        r = legacyMode ? link.sendOn(epoch, ChannelName, legacy, legacyBytes, now) :
            router.request(epoch, port, operation == Operation::Stop ? Shutdown : SubsystemEvent,
                           payloadBytes ? payload : nullptr, payloadBytes, now, deadline - now);
        if (r == Result::OK) phase = Phase::Waiting;
        else if (r != Result::Busy && r != Result::NotReady) return finish(r);
        return r;
    }
    void onEvent(uint64_t generation, const QRTR::Event &event) {
        if (!active || generation != epoch || phase != Phase::Waiting || transport == Mode::Legacy ||
            event.client != port || event.service != Service) return;
        if (event.type == QRTR::EventType::Response) {
            if (event.message.id != (operation == Operation::Stop ? Shutdown : SubsystemEvent)) { finish(Result::Invalid); return; }
            error = event.qmiError; finish(event.result);
        }
        else if (event.type == QRTR::EventType::Timeout) finish(Result::Timeout);
        else if (event.type == QRTR::EventType::Indication && operation == Operation::Stop &&
            event.message.id == Shutdown && !event.message.bytes) indication = true;
    }
    Result outcome() const { return pending() ? Result::Busy : result; }
    Phase state() const { return phase; }
    Mode mode() const { return transport; }
    uint16_t qmiError() const { return error; }
    bool shutdownIndicated() const { return indication; }
    // The reset/resource owner retires the router/link before calling this.
    void stopped() { active = false; port = 0; phase = Phase::Idle; result = Result::NotReady; }
};
}} // namespace Baseband::Sysmon
