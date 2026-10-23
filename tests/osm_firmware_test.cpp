// SPDX-License-Identifier: GPL-2.0-only
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>
#include "../boot/bridge/osm_firmware.h"
#include "../boot/bridge/osm_mmio.h"
#include "../boot/bridge/osm_peer.h"
using namespace BridgeOSMFirmware;

namespace {
uint32_t chipVersion = 0x20001, chipID = 292;
PlatformInfo platformInfo{30, 0x10000, 0, FALSE};
EFI_STATUS chipResult = EFI_SUCCESS, platformResult = EFI_SUCCESS;
unsigned protocolCalls = 0, methodCalls = 0;
bool missingChip = false, missingPlatform = false, exited = false;
EFI_STATUS EFIAPI getVersion(ChipProtocol *, UINT32 *out) {
    assert(!exited); ++methodCalls; *out = chipVersion; return chipResult;
}
EFI_STATUS EFIAPI getID(ChipProtocol *, UINT32 *out) {
    assert(!exited); ++methodCalls; *out = chipID; return chipResult;
}
EFI_STATUS EFIAPI getPlatform(PlatformProtocol *, PlatformInfo *out) {
    assert(!exited); ++methodCalls; *out = platformInfo; return platformResult;
}
ChipProtocol chip{0x10000, getVersion, nullptr, getID};
PlatformProtocol platform{0x20000, getPlatform};
EFI_STATUS EFIAPI locate(EFI_GUID *guid, void *, void **out) {
    assert(!exited); ++protocolCalls;
    const EFI_GUID chipGUID = {0xb0760469, 0x970c, 0x487a,
        {0xa4, 0xb5, 0x28, 0xdb, 0x7b, 0x45, 0xce, 0xf1}};
    const EFI_GUID platformGUID = {0x157a5c45, 0x21b2, 0x43c5,
        {0xba, 0x7c, 0x82, 0x2f, 0xee, 0x5f, 0xe5, 0x99}};
    if (!memcmp(guid, &chipGUID, sizeof(*guid))) {
        *out = missingChip ? nullptr : &chip;
        return missingChip ? EFI_NOT_FOUND : EFI_SUCCESS;
    }
    assert(!memcmp(guid, &platformGUID, sizeof(*guid)));
    *out = missingPlatform ? nullptr : &platform;
    return missingPlatform ? EFI_NOT_FOUND : EFI_SUCCESS;
}

struct MMIO {
    std::map<uint64_t, uint32_t> regs;
    struct Write { uint64_t address; uint32_t value; };
    std::vector<Write> writes;
    unsigned elapsed = 0;
    uint64_t stuckUpdate = 0, stuckEnable = 0, ignoredConfig = 0;
    uint64_t invalidRead = 0, losePLLOnRead = 0;
    bool stuckBranch = false;
    MMIO() {
        regs[GCCBase] = 0xc0100000; // firmware FSM vote active + lock
        regs[GCCBase + 0x0c] = 1;
        regs[GCCBase + 4] = 31;
        regs[GCCBase + 0x38] = 0x4000; // 600 MHz, not integer 595.2 MHz
        for (uint64_t cmd : {AuxCommand, OSMCommand, CPRCommand}) {
            regs[cmd] = 0x80000100; // initially disabled, preserve bit 8
            regs[cmd + 4] = 0x40000000; // preserve unrelated bit 30
        }
        regs[CPRBranch] = 0x80000010; // halted, preserve unrelated bit 4
    }
    static bool command(uint64_t address) {
        return address == AuxCommand || address == OSMCommand || address == CPRCommand;
    }
    static uint32_t read32(void *ctx, uint64_t address) {
        auto &self = *static_cast<MMIO *>(ctx);
        if (address == self.invalidRead) return UINT32_MAX;
        if (address == self.losePLLOnRead) self.regs[GCCBase] &= ~0x80000000u;
        assert(self.regs.count(address));
        return self.regs[address];
    }
    static void write32(void *ctx, uint64_t address, uint32_t value) {
        auto &self = *static_cast<MMIO *>(ctx);
        // This also detects accidental PLL/vote/security-register writes.
        assert(command(address) || command(address - 4) || address == CPRBranch);
        self.writes.push_back({address, value});
        if (address == self.ignoredConfig) return;
        if (command(address)) {
            if (address != self.stuckUpdate) value &= ~(1u | ConfigDirtyMask);
            if ((value & 2) && address != self.stuckEnable) value &= ~0x80000000u;
        } else if (address == CPRBranch && !self.stuckBranch) value &= ~0x80000000u;
        self.regs[address] = value;
    }
    static void delay(void *ctx, uint32_t us) {
        static_cast<MMIO *>(ctx)->elapsed += us;
    }
    IO io() { return {this, read32, write32, delay}; }
};

void identityTests() {
    EFI_BOOT_SERVICES services{}; services.LocateProtocol = locate;
    Identity result{};
    assert(identity(&services, result));
    assert(result.chipID == 292 && result.chipVersion == 0x20001 &&
           result.boardID == 30 && result.boardVersion == 0x10000);
    assert(result.failure == Failure::None && result.chipRevision == 0x10000 &&
           result.platformRevision == 0x20000 && protocolCalls == 2 && methodCalls == 3);
    chipVersion = 0x20000; chip.revision = 0x10002;
    assert(identity(&services, result));
    chip.revision = 0x10003;
    assert(!identity(&services, result) && result.failure == Failure::ChipRevision);
    chip.revision = 0x10000;
    for (uint32_t version : {0u, 0x10000u, 0x20002u, 0x30000u, UINT32_MAX}) {
        chipVersion = version;
        assert(!identity(&services, result) && result.failure == Failure::ChipVersion);
    }
    chipVersion = 0x20001; chipID = 291;
    assert(!identity(&services, result) && result.failure == Failure::ChipID);
    chipID = 292; chipResult = EFI_DEVICE_ERROR;
    assert(!identity(&services, result) && result.status == EFI_DEVICE_ERROR);
    chipResult = EFI_SUCCESS; missingChip = true;
    assert(!identity(&services, result) && result.failure == Failure::ChipProtocol);
    missingChip = false; missingPlatform = true;
    assert(!identity(&services, result) && result.failure == Failure::PlatformProtocol);
    missingPlatform = false; platform.revision = 0x10000;
    assert(!identity(&services, result) && result.failure == Failure::PlatformRevision);
    platform.revision = 0x20000; platformResult = EFI_DEVICE_ERROR;
    assert(!identity(&services, result) && result.failure == Failure::PlatformRead);
    platformResult = EFI_SUCCESS;
    platformInfo.platform = 8;
    assert(!identity(&services, result) && result.failure == Failure::BoardID);
    platformInfo.platform = 30; platformInfo.subtype = 1;
    assert(!identity(&services, result) && result.failure == Failure::BoardID);
    platformInfo.subtype = 0; platformInfo.fusion = TRUE;
    assert(!identity(&services, result) && result.failure == Failure::BoardID);
    platformInfo.fusion = FALSE;
    assert(identity(&services, result));
    assert(!identity(nullptr, result) && result.failure == Failure::Services &&
           result.chipID == 0 && result.boardID == 0);
    exited = true; // Every clock test below must be independent of EFI.
}

void acquireAndInspectTests() {
    MMIO mmio; Evidence result;
    assert(!clocksInspect(mmio.io(), result) && !result.ready && mmio.writes.empty());
    assert(clocksPrepare(mmio.io(), result));
    assert(result.ready && result.failure == Failure::None &&
           result.auxHz == 300000000 && result.osmHz == 200000000 &&
           result.xoHz == 19200000 && result.rbcprHz == 19200000);
    assert(mmio.regs[AuxCommand + 4] == (0x40000000 | AuxConfig));
    assert(mmio.regs[OSMCommand + 4] == (0x40000000 | OSMConfig));
    assert(mmio.regs[CPRCommand + 4] == (0x40000000 | CPRConfig));
    assert(mmio.regs[CPRBranch] == 0x11);
    for (uint64_t cmd : {AuxCommand, OSMCommand, CPRCommand})
        assert(mmio.regs[cmd] == 0x102);
    const size_t count = mmio.writes.size();
    assert(count == 10); // CFG/update/force for three roots, then one CPR vote.
    for (unsigned n = 0; n < 10; ++n) {
        assert(clocksPrepare(mmio.io(), result));
        assert(clocksInspect(mmio.io(), result));
        assert(mmio.writes.size() == count);
    }
    // Inspection is read-only even when write/delay callbacks are unavailable.
    IO readOnly{&mmio, MMIO::read32, nullptr, nullptr};
    assert(clocksInspect(readOnly, result));
    assert(!clocksPrepare(readOnly, result) && result.failure == Failure::IO &&
           !result.ready && !result.auxHz);
    assert(clocksInspect(readOnly, result));
    // Matching shadow CFG is not evidence of a committed mux configuration.
    mmio.regs[OSMCommand] |= 0x10;
    assert(!clocksInspect(readOnly, result) && result.failure == Failure::RootBusy);
    assert(clocksPrepare(mmio.io(), result) && !(mmio.regs[OSMCommand] & ConfigDirtyMask));
    const auto countAfterDirty = mmio.writes.size();
    assert(countAfterDirty == count + 2);
    mmio.regs[CPRBranch] |= 0x80000000;
    assert(!clocksInspect(readOnly, result) && result.failure == Failure::BranchEnable &&
           result.address == CPRBranch && !result.xoHz && !result.ready);
    mmio.regs[CPRBranch] &= ~0x80000000u;
    mmio.regs[OSMCommand + 4] ^= 1;
    assert(!clocksInspect(readOnly, result) && result.failure == Failure::RootConfiguration &&
           result.address == OSMCommand + 4 && result.expected == OSMConfig);
    mmio.regs[OSMCommand + 4] ^= 1;
    mmio.regs[AuxCommand] |= 1;
    assert(!clocksInspect(readOnly, result) && result.failure == Failure::RootBusy);
    assert(mmio.writes.size() == countAfterDirty);
}

void failureTests() {
    Evidence result;
    assert(!clocksInspect({}, result) && result.failure == Failure::IO);
    for (uint32_t mode : {0u, 0x80000000u, 0x80100000u, 0x40100000u, UINT32_MAX}) {
        MMIO m; m.regs[GCCBase] = mode;
        assert(!clocksPrepare(m.io(), result) && m.writes.empty() && !result.ready);
    }
    MMIO manual; manual.regs[GCCBase] = 0x80000001;
    assert(clocksPrepare(manual.io(), result));
    for (uint64_t address : {GCCBase + 4, GCCBase + 0x38, GCCBase + 0x0c}) {
        MMIO m; m.regs[address] = 0;
        assert(!clocksPrepare(m.io(), result) && m.writes.empty());
    }
    MMIO wrongRate; wrongRate.regs[GCCBase + 4] = 32;
    assert(!clocksPrepare(wrongRate.io(), result) && result.failure == Failure::PLLRate);
    MMIO invalid; invalid.invalidRead = AuxCommand;
    assert(!clocksPrepare(invalid.io(), result) && result.failure == Failure::InvalidRead &&
           result.address == AuxCommand && result.raw == UINT32_MAX && invalid.writes.empty());
    MMIO busy; busy.regs[AuxCommand] |= 1;
    assert(!clocksPrepare(busy.io(), result) && result.failure == Failure::RootBusy &&
           busy.writes.empty());
    for (uint64_t command : {AuxCommand, OSMCommand, CPRCommand}) {
        MMIO update; update.stuckUpdate = command;
        assert(!clocksPrepare(update.io(), result) && result.failure == Failure::RootUpdate &&
               result.address == command && update.elapsed == TimeoutUS && !result.ready);
        MMIO enable; enable.stuckEnable = command;
        assert(!clocksPrepare(enable.io(), result) && result.failure == Failure::RootEnable &&
               result.address == command && enable.elapsed == TimeoutUS);
        MMIO config; config.ignoredConfig = command + 4;
        assert(!clocksPrepare(config.io(), result) && result.failure == Failure::RootConfiguration &&
               result.address == command + 4);
    }
    MMIO branch; branch.stuckBranch = true;
    assert(!clocksPrepare(branch.io(), result) && result.failure == Failure::BranchEnable &&
           result.address == CPRBranch && branch.elapsed == TimeoutUS);
    // A stale prior successful result cannot survive parent loss or a new IO.
    MMIO changed; assert(clocksPrepare(changed.io(), result));
    changed.losePLLOnRead = OSMCommand;
    const auto count = changed.writes.size();
    assert(!clocksInspect(changed.io(), result) && result.failure == Failure::PLLInactive &&
           !result.ready && !result.auxHz && changed.writes.size() == count);
    MMIO recovery;
    assert(clocksPrepare(recovery.io(), result) && result.failure == Failure::None);
}

void mappingTests() {
    struct Mapping {
        unsigned calls = 0, failCall = 0;
        bool falseSuccess = false, unmapped = true, readOnly = false, stuck = false;
        uint64_t badPage = 0, badPAR = 1;
        static EFI_STATUS attributes(void *context, uint64_t address, uint64_t bytes, uint64_t attrs) {
            auto &m = *static_cast<Mapping *>(context);
            assert(address == 0x179d1000 && bytes == 0x3000);
            assert(attrs == BridgeOSMMMIO::Attributes ||
                   attrs == (BridgeOSMMMIO::Attributes | EFI_MEMORY_RO));
            if (++m.calls == m.failCall) return EFI_DEVICE_ERROR;
            if (m.stuck || (m.falseSuccess && m.calls == 1)) return EFI_SUCCESS;
            m.unmapped = false; m.readOnly = attrs & EFI_MEMORY_RO;
            return EFI_SUCCESS;
        }
        static uint64_t translate(void *context, uint64_t address, bool write) {
            const auto &m = *static_cast<Mapping *>(context);
            if (m.unmapped || (write && m.readOnly)) return 1;
            if (address == m.badPage) return m.badPAR;
            return address | (4ULL << 56) | 0x900;
        }
        BridgeOSMMMIO::IO io() { return {this, attributes, translate}; }
    };
    BridgeOSMMMIO::Evidence e;
    Mapping normal;
    assert(BridgeOSMMMIO::prepare(normal.io(), 0x179d1000, 0x3000, e));
    assert(e.calls == 1 && !e.retry && e.address == 0x179d3000);
    Mapping missing; missing.falseSuccess = true;
    assert(BridgeOSMMMIO::prepare(missing.io(), 0x179d1000, 0x3000, e));
    assert(e.calls == 3 && e.retry && !missing.unmapped && !missing.readOnly);
    Mapping stuck; stuck.stuck = true;
    assert(!BridgeOSMMMIO::prepare(stuck.io(), 0x179d1000, 0x3000, e));
    assert(e.calls == 3 && e.read == 1 && e.write == 1);
    for (unsigned call : {1u, 2u, 3u}) {
        Mapping failed; failed.falseSuccess = true; failed.failCall = call;
        assert(!BridgeOSMMMIO::prepare(failed.io(), 0x179d1000, 0x3000, e));
        assert(e.calls == call && e.status == EFI_DEVICE_ERROR);
    }
    // Check every page, read/write permissions, identity and Device attributes.
    for (uint64_t par : {1ULL, 0x179d9000ULL, 0x44000000179d2000ULL, 0xff000000179d2000ULL}) {
        Mapping invalid; invalid.badPage = 0x179d2000; invalid.badPAR = par;
        assert(!BridgeOSMMMIO::prepare(invalid.io(), 0x179d1000, 0x3000, e));
        assert(e.address == 0x179d2000 && e.calls == 3);
    }
    const BridgeOSMMMIO::IO readOnly{&normal, nullptr, Mapping::translate};
    const auto calls = normal.calls;
    assert(BridgeOSMMMIO::inspect(readOnly, 0x179d1000, 0x3000, e));
    normal.readOnly = true;
    assert(!BridgeOSMMMIO::inspect(readOnly, 0x179d1000, 0x3000, e) && e.write == 1);
    normal.readOnly = false; normal.unmapped = true;
    assert(!BridgeOSMMMIO::inspect(readOnly, 0x179d1000, 0x3000, e) && normal.calls == calls);
    assert(!BridgeOSMMMIO::prepare({}, 0x179d1000, 0x3000, e));
    for (uint64_t bytes : {0ULL, 1ULL, ~0ULL, 1ULL << 48}) {
        Mapping invalid;
        assert(!BridgeOSMMMIO::prepare(invalid.io(), 0x179d1000, bytes, e) && !invalid.calls);
    }
}

void peerTests() {
    struct Peer {
        int32_t version = 0x10000, initial = 1, startStatus = 0;
        unsigned elapsed = 0, starts = 0, stops = 0;
        uint64_t affinity = 0x80000100, state = 1;
        bool owned = false, stopping = false, offFails = false;
        int32_t psci(uint64_t function, uint64_t a, uint64_t b, uint64_t c) {
            if (function == 0x84000000) return version;
            assert(a == BridgeOSMPeer::Affinity);
            if (function == 0xc4000004) {
                assert(!b && !c);
                if (!owned) return initial;
                if (stopping && !offFails && elapsed >= 300) { owned = false; return 1; }
                return 0;
            }
            assert(function == 0xc4000003 && b == 0x90000100 && c == 0x90001000);
            ++starts;
            if (!startStatus) owned = true;
            return startStatus;
        }
        uint64_t read(unsigned offset) {
            assert(offset == 64 || offset == 128);
            return offset == 128 ? affinity : (stopping && offFails ? 2 : state);
        }
        void stop() { assert(owned); ++stops; stopping = true; }
        void delay(unsigned us) { assert(us == 100); elapsed += us; }
    };
    BridgeOSMPeer::Result r;
    Peer normal;
    assert(BridgeOSMPeer::start(normal, 0x90000100, 0x90001000, r) && r.owned && r.ready);
    assert(BridgeOSMPeer::stop(normal, r) && r.stopped && !r.owned && !r.ready);
    assert(normal.starts == 1 && normal.stops == 1 && normal.elapsed == 300);
    assert(BridgeOSMPeer::stop(normal, r) && normal.stops == 1);
    for (int32_t status : {-1, 0, 2}) {
        Peer foreign; foreign.initial = status;
        assert(!BridgeOSMPeer::start(foreign, 0x90000100, 0x90001000, r));
        assert(!foreign.starts && !r.owned && BridgeOSMPeer::stop(foreign, r) && !foreign.stops);
    }
    Peer unsupported; unsupported.version = 1;
    assert(!BridgeOSMPeer::start(unsupported, 0x90000100, 0x90001000, r) && !unsupported.starts);
    Peer refused; refused.startStatus = -3;
    assert(!BridgeOSMPeer::start(refused, 0x90000100, 0x90001000, r) && !r.owned);
    Peer noEntry; noEntry.state = 0;
    assert(!BridgeOSMPeer::start(noEntry, 0x90000100, 0x90001000, r) && r.owned && !r.ready);
    assert(noEntry.elapsed == 100000 && BridgeOSMPeer::stop(noEntry, r));
    Peer wrong; wrong.affinity = 0x80000000;
    assert(!BridgeOSMPeer::start(wrong, 0x90000100, 0x90001000, r) && r.owned);
    assert(BridgeOSMPeer::stop(wrong, r));
    Peer noOff; noOff.offFails = true;
    assert(BridgeOSMPeer::start(noOff, 0x90000100, 0x90001000, r));
    assert(!BridgeOSMPeer::stop(noOff, r) && r.owned && !r.stopped);
    Peer invalid;
    assert(!BridgeOSMPeer::start(invalid, 0x90000101, 0x90001000, r) && !invalid.starts);
    assert(!BridgeOSMPeer::start(invalid, 0x90000100, 0x90001004, r) && !invalid.starts);
}
}
int main() {
    identityTests();
    acquireAndInspectTests();
    failureTests();
    mappingTests();
    peerTests();
    puts("OSM firmware identity and clock preparation tests passed");
}
