// SPDX-License-Identifier: GPL-2.0-only
// Qualcomm SIP memory assignment, Linux v6.12 qcom_scm.c / qcom_scm-smc.c.
#pragma once
#include <BasebandProtocol.h>

namespace Baseband { namespace Secure {
constexpr uint32_t HLOS = 3, MSS = 15;
constexpr size_t ScratchBytes = 256;
struct Registers { uint64_t x[8]{}; };
constexpr uint32_t function(uint32_t service, uint32_t command) {
    return 0x02000000 | (service << 8) | command;
}
struct Reply { Result result; uint64_t value; int32_t monitor; };

// Scratch is retained physically contiguous memory, outside every region being
// reassigned. It is never a stack/kernel/userspace virtual pointer in an SMC.
// Backend.publish() cleans the scratch cache and performs the required barrier.
// The caller encloses probe/change and every interrupted continuation/retry in
// one SecureMonitorRunAction, shared with the other SCM clients.
template<class Backend> class Assignment {
    Backend &io;
    uint8_t *scratch;
    uint64_t physical;
    bool use64 = true, ready = false, pinned = false;
    Reply invoke(uint32_t service, uint32_t command, uint32_t info,
                 uint64_t a, uint64_t b, uint64_t c, uint64_t d, bool fast = false) {
        const Registers original{{function(service, command) | (use64 ? 0x40000000 : 0) |
            (fast ? 0x80000000 : 0), info, a, b, c, d, 0, 0}};
        for (unsigned busy = 0; busy <= 20; ++busy) {
            uint64_t fn = original.x[0], cookie = 0;
            Registers r{};
            for (unsigned interrupted = 0; ; ++interrupted) {
                r = original; r.x[0] = fn; r.x[6] = cookie; io.smc(r);
                if (int32_t(r.x[0]) != 1) break;
                if (interrupted == 1023) { pinned = true; return {Result::Quarantined, 0, 1}; }
                fn = 1; cookie = r.x[6];
            }
            const auto monitor = int32_t(r.x[0]);
            switch (monitor) {
            case 0: return {Result::OK, r.x[1], 0};
            case -1: case -4: return {Result::Unsupported, 0, monitor};
            case -2: case -3: return {Result::Invalid, 0, monitor};
            case -5: return {Result::NoMemory, 0, monitor};
            case -12:
                if (busy == 20) return {Result::Busy, 0, monitor};
                io.delayMS(30); break;
            default:
                // WAITQ_SLEEP/unknown continuation is never a completed call.
                pinned = true; return {Result::Quarantined, 0, monitor};
            }
        }
        pinned = true; return {Result::Quarantined, 0, INT32_MIN};
    }
public:
    Assignment(Backend &backend, void *buffer, uint64_t address)
        : io(backend), scratch(static_cast<uint8_t *>(buffer)), physical(address) {}
    bool quarantined() const { return pinned; }
    bool is64() const { return use64; }
    Result probe() {
        if (pinned) return Result::Quarantined;
        if (!scratch || !physical || (physical & 63) ||
            !range(physical, ScratchBytes, uint64_t(1) << 32)) return Result::Invalid;
        if (ready) return Result::OK;
        // is_call_available is read-only. SMC32 fallback is limited to an
        // explicit unsupported convention; no retry after an ambiguous reply.
        for (unsigned i = 0; i < 2; ++i) {
            use64 = !i;
            const auto r = invoke(6, 1, 1, function(6, 1), 0, 0, 0, true);
            if (r.result == Result::OK && r.value == 1) {
                const auto available = invoke(6, 1, 1, function(12, 22), 0, 0, 0);
                if (available.result != Result::OK) return available.result;
                if (available.value != 1) return Result::Unsupported;
                ready = true; return Result::OK;
            }
            if (r.result != Result::OK && r.result != Result::Unsupported && r.result != Result::Invalid)
                return r.result;
        }
        return Result::Unsupported;
    }
    Result change(const Region &region, Ownership current, Ownership next, bool confirm = false,
                  uint32_t hostPermissions = 7) {
        if (pinned) return Result::Quarantined;
        if (!ready) return Result::NotReady;
        if (!region.valid() || current == Ownership::Unknown || next == Ownership::Unknown ||
            (hostPermissions != 6 && hostPermissions != 7) ||
            overlaps(region.address, region.bytes, physical, ScratchBytes)) return Result::Invalid;
        if (current == next && !confirm) return Result::OK;
        // A reserved address does not prove initial HLOS access. The Board
        // requests a real MP_ASSIGN completion before touching its payload.
        if (confirm && (current != Ownership::Host || next != Ownership::Host)) return Result::Invalid;
        for (size_t i = 0; i < ScratchBytes; ++i) scratch[i] = 0;
        unsigned sources = 0, destinations = 0;
        if (current != Ownership::Modem) write32(scratch + 4 * sources++, HLOS);
        if (current != Ownership::Host) write32(scratch + 4 * sources++, MSS);
        write64(scratch + 64, region.address); write64(scratch + 72, region.bytes);
        auto destination = [&](uint32_t vmid, uint32_t permissions) {
            write32(scratch + 128 + 24 * destinations, vmid);
            write32(scratch + 132 + 24 * destinations, permissions); ++destinations;
        };
        if (next != Ownership::Modem) destination(HLOS, hostPermissions);
        if (next != Ownership::Host) destination(MSS, 6); // R/W; MBA handles auth.
        // Seven possible extended slots begin at argument 3, padded with zero.
        // mem_map pointer/size and source pointer occupy x2/x3/x4; x5 is physical.
        const uint64_t args[4] = {sources * 4u, physical + 128, destinations * 24u, 0};
        for (unsigned i = 0; i < 4; ++i) {
            if (use64) write64(scratch + 192 + i * 8, args[i]);
            else write32(scratch + 192 + i * 4, uint32_t(args[i]));
        }
        if (!io.publish(scratch, physical, ScratchBytes)) return Result::IO;
        const auto r = invoke(12, 22, 0x1117, physical + 64, 16, physical, physical + 192);
        if (r.result != Result::OK) return r.result;
        return r.value ? Result::Authentication : Result::OK;
    }
};
}} // namespace Baseband::Secure
