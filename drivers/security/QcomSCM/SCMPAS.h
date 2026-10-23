// SPDX-License-Identifier: GPL-2.0-only
// Qualcomm SIP/PAS ABI derived from Linux v6.12 qcom_scm{,-smc}.c.
#pragma once
#include <stdint.h>

namespace QcomSecure {
enum class Status { OK, Invalid, Unsupported, NoMemory, Busy, Denied, Ambiguous };
enum class Phase : uint32_t { Empty, Metadata, Region, Authenticated, Warm, Faulted };
struct Registers { uint64_t x[8]{}; };
struct Reply { Status status = Status::Ambiguous; uint64_t value = 0; int32_t monitor = INT32_MIN; };
constexpr uint32_t sip(uint32_t service, uint32_t command) {
    return 0x02000000u | (service << 8) | command;
}

// Backend implements smc(registers) and sleep(ms). Only register arguments are
// needed for PAS; no extended argument array or shared-memory bridge is used.
template<class Backend> class Transport {
    Backend &io;
    bool f64 = true, fReady = false;
    Reply invoke(uint32_t service, uint32_t command, uint32_t info,
                 uint64_t a = 0, uint64_t b = 0, uint64_t c = 0, bool fast = false) {
        const Registers initial{{sip(service, command) | (f64 ? 0x40000000u : 0) |
            (fast ? 0x80000000u : 0), info, a, b, c, 0, 0, 0}};
        for (unsigned busy = 0; busy <= 20; ++busy) {
            uint64_t function = initial.x[0], continuation = 0;
            Registers result{};
            for (unsigned interrupted = 0; ; ++interrupted) {
                result = initial;
                result.x[0] = function; result.x[6] = continuation;
                io.smc(result);
                const auto status = int32_t(result.x[0]);
                if (status != 1) break;
                // Qualcomm A6 quirk: resume with x0=1, original x1..5/x7,
                // and the secure monitor's returned x6 continuation cookie.
                if (interrupted == 1023) return {};
                function = 1; continuation = result.x[6];
            }
            switch (int32_t(result.x[0])) {
            case 0: return {Status::OK, result.x[1]};
            case -2: case -3: return {Status::Invalid, 0};
            case -4: return {Status::Unsupported, 0};
            case -5: return {Status::NoMemory, 0};
            case -12:
                if (busy == 20) return {Status::Busy, 0};
                io.sleep(30); break;
            // WAITQ_SLEEP is not the MSM8998 interrupted ABI. It cannot be
            // retried as a new transaction or treated as completed.
            default: return {Status::Ambiguous, 0, int32_t(result.x[0])};
            }
        }
        return {};
    }
public:
    explicit Transport(Backend &backend) : io(backend) {}
    Status probe() {
        if (fReady) return Status::OK;
        for (unsigned i = 0; i < 2; ++i) {
            f64 = i == 0;
            const auto r = invoke(6, 1, 1, sip(6, 1), 0, 0, true);
            if (r.status == Status::OK && r.value == 1) { fReady = true; return Status::OK; }
            // SMCCC returns -1 for an unknown 64-bit function. This probe
            // changes no peripheral state, so the documented SMC32 fallback
            // is safe; an interrupted/WAITQ transaction is not.
            if (r.status != Status::OK && r.status != Status::Unsupported && r.status != Status::Invalid &&
                !(r.status == Status::Ambiguous && r.monitor == -1))
                return r.status;
        }
        return Status::Unsupported; // Never guess a legacy convention.
    }
    bool is64() const { return f64; }
    Reply available(uint32_t service, uint32_t command) {
        return fReady ? invoke(6, 1, 1, sip(service, command)) : Reply{};
    }
    Reply supported() { return fReady ? invoke(2, 7, 1, 13) : Reply{}; }
    Reply pas(uint32_t command, uint64_t address = 0, uint64_t bytes = 0) {
        if (!fReady) return {};
        if (!f64 && (address > UINT32_MAX || bytes > UINT32_MAX)) return {Status::Invalid, 0};
        if (command == 1) return invoke(2, 1, 2 | (2 << 6), 13, address); // VAL, RW
        if (command == 2) return invoke(2, 2, 3, 13, address, bytes);
        if (command == 5 || command == 6) return invoke(2, command, 1, 13);
        return {Status::Invalid, 0};
    }
    Reply resume() { return fReady ? invoke(1, 10, 2, 0, 13) : Reply{}; }
};

template<class TransportType> class PAS {
    TransportType &transport;
    Phase fPhase = Phase::Empty;
    Status finish(Reply reply, Phase next) {
        if (reply.status == Status::Ambiguous) fPhase = Phase::Faulted;
        if (reply.status != Status::OK) return reply.status;
        if (reply.value) return Status::Denied;
        fPhase = next; return Status::OK;
    }
public:
    explicit PAS(TransportType &t) : transport(t) {}
    Phase phase() const { return fPhase; }
    void fault() { fPhase = Phase::Faulted; }
    Status resume() {
        if (fPhase != Phase::Empty && fPhase != Phase::Warm) return Status::Invalid;
        return finish(transport.resume(), Phase::Warm);
    }
    Status init(uint64_t address) {
        return fPhase == Phase::Empty ? finish(transport.pas(1, address), Phase::Metadata) : Status::Invalid;
    }
    Status setup(uint64_t address, uint64_t bytes, bool relocatable = true) {
        if (fPhase != Phase::Metadata || !address || !bytes) return Status::Invalid;
        // Fixed-address MDTs already encode their physical destination in the
        // signed program headers. Linux's PAS loader deliberately skips cmd 2.
        return finish(relocatable ? transport.pas(2, address, bytes) : Reply{Status::OK, 0}, Phase::Region);
    }
    Status authenticate() {
        return fPhase == Phase::Region ? finish(transport.pas(5), Phase::Authenticated) : Status::Invalid;
    }
    Status shutdown() {
        if (fPhase != Phase::Metadata && fPhase != Phase::Region && fPhase != Phase::Authenticated)
            return Status::Invalid; // A retained boot image belongs to its loader.
        return finish(transport.pas(6), Phase::Empty);
    }
    bool release() {
        if (fPhase != Phase::Empty && fPhase != Phase::Warm) return false;
        fPhase = Phase::Empty; return true;
    }
};
} // namespace QcomSecure
