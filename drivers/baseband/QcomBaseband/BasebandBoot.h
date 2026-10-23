// SPDX-License-Identifier: GPL-2.0-only
// MSM8998 PBL -> MBA -> MPSS lifecycle, Linux v6.12 qcom_q6v5_mss.c.
#pragma once
#include "BasebandFirmware.h"
#include "BasebandQ6.h"

namespace Baseband {
struct BootLayout {
    Region modem{0x8d000000, 0x7000000}, mba{0x94500000, 0x200000}, metadata{};
    bool valid() const {
        const Region regions[] = {modem, mba, metadata};
        for (unsigned i = 0; i < 3; ++i) {
            if (!regions[i].valid()) return false;
            for (unsigned j = 0; j < i; ++j)
                if (overlaps(regions[i].address, regions[i].bytes, regions[j].address, regions[j].bytes))
                    return false;
        }
        // Upstream mdata is dynamically reserved within 0xa0000000..0xc0000000.
        // 0x94400000 is inside sagit's Venus reservation, not free metadata RAM.
        return modem.address == 0x8d000000 && modem.bytes == 0x7000000 &&
            mba.address == 0x94500000 && mba.bytes == 0x200000 &&
            metadata.address >= 0xa0000000 && metadata.address + metadata.bytes <= 0xc0000000 &&
            metadata.bytes >= 0x4000;
    }
};

// Board is the resource/memory boundary, not a mock success adapter:
// admit() proves EFI/XNU reservations and a stopped remote without writes.
// acquire() retains CX/MX and all proxy/active clocks with reset held, confirms
// initial HLOS ownership with SCM before mapping/copying target RAM;
// failed acquire must unwind its partial changes or report Quarantined.
// assign() runs under the global SCM transaction lock and uses real VMID
// transitions. stageMetadata/copy/zero/publish perform bounded writes/cache
// maintenance. quiesceAndReset() drains all AXI ports and asserts MSS reset.
// Resources and allocations must remain pinned after any ambiguous result.
// This owner and the Board live on the heap and are called on one work loop.
template<class Board, class RegisterBackend> class Boot {
    Board &board;
    Q6::Core<RegisterBackend> core;
    BootLayout layout{};
    Ownership owners[3]{Ownership::Host, Ownership::Host, Ownership::Host};
    uint64_t epoch = 0;
    State phase = State::Offline;
    bool resources = false, pinned = false;
    Result last = Result::OK;

    const Region &region(unsigned i) const {
        return i == 0 ? layout.modem : i == 1 ? layout.mba : layout.metadata;
    }
    Result transfer(unsigned i, Ownership next) {
        if (owners[i] == next) return Result::OK;
        if (owners[i] == Ownership::Unknown) return Result::Quarantined;
        const auto r = board.assign(region(i), owners[i], next);
        if (r == Result::OK) owners[i] = next;
        else if (r == Result::Quarantined) { owners[i] = Ownership::Unknown; pinned = true; }
        return r;
    }
    Result cleanup() {
        if (pinned) return Result::Quarantined;
        // A write/timeout failure does not prove that Q6 stopped issuing DMA.
        // Do not expose or free any remote memory until reset/drain succeeds.
        if (resources && board.quiesceAndReset() != Result::OK) {
            pinned = true; return Result::Quarantined;
        }
        for (int i = 2; i >= 0; --i) {
            if (transfer(unsigned(i), Ownership::Host) != Result::OK) {
                pinned = true; return Result::Quarantined;
            }
        }
        if (resources) {
            if (board.release() != Result::OK) { pinned = true; return Result::Quarantined; }
            resources = false;
        }
        return Result::OK;
    }
    Result fault(Result reason) {
        phase = State::Faulted; last = reason;
        if (reason == Result::Quarantined) pinned = true;
        return cleanup() == Result::OK ? reason : (last = Result::Quarantined);
    }
public:
    Boot(Board &backend, RegisterBackend &registers) : board(backend), core(registers) {}
    State state() const { return phase; }
    bool quarantined() const { return pinned; }
    bool retainsResources() const { return resources; }
    Result lastResult() const { return last; }
    uint64_t generation() const { return epoch; }
    Ownership owner(unsigned i) const { return i < 3 ? owners[i] : Ownership::Unknown; }
    Result start(uint64_t generation, const FirmwareImage &mba, const FirmwareImage &modem,
                 const BootLayout &memory) {
        if (pinned) return Result::Quarantined;
        if (resources || (phase != State::Offline && phase != State::Faulted)) return Result::Busy;
        if (!generation || generation <= epoch || !memory.valid() || !mba.valid() || !modem.valid() ||
            mba.byteCount() > memory.mba.bytes || modem.metadataBytes() > memory.metadata.bytes ||
            modem.validateRegion(memory.modem) != Result::OK) return Result::Invalid;
        auto r = board.admit(memory);
        if (r != Result::OK) return r; // Preflight must not modify hardware.
        layout = memory; epoch = generation; phase = State::Prepared; last = Result::OK;
        r = board.acquire();
        if (r != Result::OK) return fault(r);
        resources = true;
        // PBL consumes the complete signed MBA file, not its ELF PT_LOADs.
        if (!board.copy(layout.mba.address, mba.bytes(), mba.byteCount()) ||
            !board.zero(layout.mba.address + mba.byteCount(), layout.mba.bytes - mba.byteCount()) ||
            !board.publish(layout.mba.address, layout.mba.bytes)) return fault(Result::IO);
        // MBA may wipe MPSS itself. Give it sole MPSS access before boot.
        if ((r = transfer(0, Ownership::Modem)) != Result::OK ||
            (r = transfer(1, Ownership::Modem)) != Result::OK) return fault(r);
        phase = State::MBAStarting;
        if ((r = board.deassertReset()) != Result::OK ||
            (r = core.start(layout.mba.address)) != Result::OK ||
            (r = core.waitMBA()) != Result::OK) return fault(r);
        phase = State::MBAReady;
        if ((r = board.stageMetadata(modem, layout.metadata)) != Result::OK ||
            (r = transfer(2, Ownership::Modem)) != Result::OK) return fault(r);
        phase = State::Metadata;
        r = core.authenticateMetadata(layout.metadata.address);
        if (r != Result::OK) return fault(r); // Reset/drain before revoking a still-active metadata reader.
        const auto reclaimed = transfer(2, Ownership::Host);
        if (reclaimed != Result::OK) return fault(reclaimed);
        // Secure restart reclamation is permitted only after MBA is alive.
        if ((r = transfer(0, Ownership::Host)) != Result::OK ||
            (r = transfer(0, Ownership::Shared)) != Result::OK) return fault(r);
        phase = State::Loading;
        uint32_t loaded = 0;
        for (unsigned i = 0; i < modem.segmentCount(); ++i) {
            const auto *segment = modem.segment(i);
            uint64_t address = 0;
            if (modem.destination(layout.modem, i, address) != Result::OK) return fault(Result::Invalid);
            if ((segment->fileBytes && !board.copy(address, modem.bytes() + segment->fileOffset, segment->fileBytes)) ||
                (segment->memoryBytes > segment->fileBytes &&
                 !board.zero(address + segment->fileBytes, segment->memoryBytes - segment->fileBytes)) ||
                !board.publish(address, segment->memoryBytes)) return fault(Result::IO);
            if (!loaded) {
                const uint64_t start = modem.relocatable() ? layout.modem.address : modem.lowestAddress();
                if ((r = core.beginLoad(start)) != Result::OK) return fault(r);
            }
            loaded += segment->memoryBytes;
            if ((r = core.loaded(loaded)) != Result::OK) return fault(r);
        }
        if ((r = transfer(0, Ownership::Modem)) != Result::OK) return fault(r);
        phase = State::Authenticating;
        if ((r = core.waitAuthentication()) != Result::OK) return fault(r);
        phase = State::WaitingReady;
        if ((r = board.waitReady(generation, 5000000)) != Result::OK ||
            (r = transfer(1, Ownership::Host)) != Result::OK) return fault(r);
        phase = State::Running; return Result::OK;
    }
    Result stop(uint64_t generation) {
        if (generation != epoch || !epoch) return Result::Stale;
        if (pinned) return Result::Quarantined;
        if (phase == State::Offline) return Result::OK;
        phase = State::Stopping;
        // Graceful STOP_ACK is desirable; drain/reset is still mandatory even
        // if the modem ignores it. It is the proof used for memory reclamation.
        if (resources) board.requestStop(generation, 5000000);
        const auto r = cleanup();
        last = r; phase = r == Result::OK ? State::Offline : State::Faulted;
        return r;
    }
};
} // namespace Baseband
