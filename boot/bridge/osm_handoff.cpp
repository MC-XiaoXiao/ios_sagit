// SPDX-License-Identifier: GPL-2.0-only
// Sagit MSM8998 v2: firmware identity -> QFPROM -> CPRh -> OSM -> XNU.
#include <Uefi.h>
#include <Protocol/Cpu.h>
extern "C" {
#include "bridge.h"
void *memcpy(void *, const void *, size_t);
}
#include "osm_firmware.h"
#include "osm_mmio.h"
#include "osm_peer.h"
#include "../../drivers/clock/QcomOSM/CPRhInitialization.h"
#include "../../drivers/clock/QcomOSM/OSMInitialization.h"
#include "../../drivers/clock/QcomOSM/OSMHandoff.h"
#include "../../drivers/clock/QcomOSM/OSMSecureIO.h"
#include "../../drivers/clock/QcomOSM/OSMController.h"
#include "../../drivers/security/QcomSCM/SMCCC.h"

// PSCI supplies x0=context with MMU/data cache disabled. No stack, EFI call,
// GIC setup or shared cache line writes are needed by this temporary core.
extern "C" void bridge_osm_peer_entry(void);
__asm__(".text\n.balign 64\n.global bridge_osm_peer_entry\nbridge_osm_peer_entry:\n"
        "msr daifset,#15\nic iallu\ndsb sy\nisb\nmov x19,x0\nmrs x2,mpidr_el1\nstr x2,[x19,#128]\n"
        "dsb sy\nmov x2,#1\nstr x2,[x19,#64]\ndsb sy\n"
        "1: ldr x2,[x19]\ncmp x2,#1\nb.ne 1b\n"
        "mov x0,#2\nmovk x0,#0x8400,lsl #16\nmov x1,#0\nsmc #0\n"
        "str x0,[x19,#136]\nmov x2,#2\nstr x2,[x19,#64]\ndsb sy\n"
        "2: wfe\nb 2b\n");

namespace {
void *recordProperty = nullptr;
bool prepared = false;
bool timerFailure = false;
struct CPRAccess { uint32_t cluster, offset, value, write; };
CPRAccess cprTrace[32]{};
unsigned cprAccesses = 0;
void traceCPR(unsigned cluster, uint32_t offset, uint32_t value, bool write) {
    cprTrace[cprAccesses++ % 32] = {cluster, offset, value, write};
}
void logCPR() {
    const unsigned begin = cprAccesses > 32 ? cprAccesses - 32 : 0;
    for (unsigned i = begin; i < cprAccesses; ++i) {
        const auto &r = cprTrace[i % 32];
        bridge_puts("[OSM CPR] cluster="); bridge_hex(r.cluster);
        bridge_puts(" offset="); bridge_hex(r.offset);
        bridge_puts(" value="); bridge_hex(r.value);
        bridge_puts(" write="); bridge_hex(r.write); bridge_puts("\n");
    }
}
QcomCPRh::Identity identity{};
QcomCPRh::Plan plans[2]{};
uint64_t peerMailbox = 0;
constexpr struct { uint64_t address, bytes; } mmioWindows[] = {
    {0x00100000, 0x94000}, {0x00784000, 0x1000},
    {0x179c0000, 0xc000}, {0x179d1000, 0x1000},
    {0x17910000, 0x8000}, {0x17810000, 0x8000}
};
uint64_t translateMMIO(void *, uint64_t address, bool write) {
    uint64_t par;
    if (write) __asm__ volatile("at s1e1w,%1\n\tisb\n\tmrs %0,par_el1" : "=r"(par) : "r"(address) : "memory");
    else __asm__ volatile("at s1e1r,%1\n\tisb\n\tmrs %0,par_el1" : "=r"(par) : "r"(address) : "memory");
    return par;
}
EFI_STATUS attributesMMIO(void *context, uint64_t address, uint64_t bytes, uint64_t attributes) {
    auto *cpu = static_cast<EFI_CPU_ARCH_PROTOCOL *>(context);
    const auto status = cpu->SetMemoryAttributes(cpu, address, bytes, attributes);
    __asm__ volatile("dsb sy\n\tisb" ::: "memory");
    return status;
}
void logMMIO(const BridgeOSMMMIO::Evidence &e, bool ready, bool final) {
    bridge_puts("[OSM MMIO] final="); bridge_hex(final);
    bridge_puts(" ready="); bridge_hex(ready);
    bridge_puts(" address="); bridge_hex(e.address);
    bridge_puts(" read="); bridge_hex(e.read);
    bridge_puts(" write="); bridge_hex(e.write);
    bridge_puts(" status="); bridge_hex(e.status);
    bridge_puts(" calls="); bridge_hex(e.calls);
    bridge_puts(" retry="); bridge_hex(e.retry); bridge_puts("\n");
}
uint32_t read32(uint64_t address) {
    uint32_t value = *reinterpret_cast<volatile const uint32_t *>(address);
    __asm__ volatile("uxtw %x0,%w0\n\tdmb oshld" : "+r"(value) :: "memory");
    return value;
}
void write32(uint64_t address, uint32_t value) {
    __asm__ volatile("dmb oshst" ::: "memory");
    *reinterpret_cast<volatile uint32_t *>(address) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
uint64_t counter() { uint64_t v; __asm__ volatile("mrs %0,cntpct_el0" : "=r"(v)); return v; }
void waitUS(unsigned us) {
    if (timerFailure) return;
    const uint64_t start = counter(), ticks = (uint64_t(us) * 19200000 + 999999) / 1000000;
    uint64_t remaining = 2000000ULL + uint64_t(us) * 512;
    while (counter() - start < ticks) {
        if (!remaining--) { timerFailure = true; return; }
        __asm__ volatile("yield");
    }
}
struct PeerIO {
    int32_t psci(uint64_t function, uint64_t a, uint64_t b, uint64_t c) {
        QcomSecure::Registers r{}; r.x[0] = function; r.x[1] = a; r.x[2] = b; r.x[3] = c;
        QcomSecure::smc(r);
        return int32_t(r.x[0]);
    }
    uint64_t read(unsigned offset) {
        const auto address = peerMailbox + offset;
        __asm__ volatile("dc ivac,%0\n\tdsb sy" :: "r"(address) : "memory");
        const auto value = *reinterpret_cast<volatile const uint64_t *>(address);
        __asm__ volatile("dmb sy" ::: "memory");
        return value;
    }
    void stop() {
        *reinterpret_cast<volatile uint64_t *>(peerMailbox) = 1;
        __asm__ volatile("dc cvac,%0\n\tdsb sy\n\tsev" :: "r"(peerMailbox) : "memory");
    }
    void delay(unsigned us) { waitUS(us); }
};
void logPeer(const BridgeOSMPeer::Result &r) {
    bridge_puts("[OSM peer] status="); bridge_hex(uint64_t(int64_t(r.status)));
    bridge_puts(" affinity="); bridge_hex(r.observedAffinity);
    bridge_puts(" owned="); bridge_hex(r.owned);
    bridge_puts(" ready="); bridge_hex(r.ready);
    bridge_puts(" stopped="); bridge_hex(r.stopped); bridge_puts("\n");
}
void barrier(void *) { __asm__ volatile("dsb sy\n\tisb" ::: "memory"); }
void delay(void *, unsigned us) { waitUS(us); }
uint32_t firmwareRead(void *, uint64_t address) { return timerFailure ? UINT32_MAX : read32(address); }
void firmwareWrite(void *, uint64_t address, uint32_t value) { if (!timerFailure) write32(address, value); }
const BridgeOSMFirmware::IO clockIO{nullptr, firmwareRead, firmwareWrite, delay};
struct Monitor {
    void smc(QcomSecure::Registers &r) {
        if (timerFailure) { r.x[0] = uint64_t(-12); return; }
        QcomSecure::smc(r);
    }
    void sleep(unsigned ms) { waitUS(ms * 1000); }
};
using Secure = QcomOSMInit::SecureIO<Monitor>;
bool osmRead(void *, unsigned region, uint32_t offset, uint32_t &value) {
    if (timerFailure || region > 3 || (offset & 3) || offset >= (region < 2 ? 0x2000U : 0x800U)) return false;
    const uint64_t base = region < 2 ? QcomOSMInit::osmBase(region) : QcomOSMInit::acdBase(region - 2);
    value = read32(base + offset); return true;
}
bool osmWrite(void *, unsigned region, uint32_t offset, uint32_t value) {
    if (timerFailure || region > 3 || (offset & 3) || offset >= (region < 2 ? 0x2000U : 0x800U)) return false;
    const uint64_t base = region < 2 ? QcomOSMInit::osmBase(region) : QcomOSMInit::acdBase(region - 2);
    write32(base + offset, value); return true;
}
bool secureRead(void *context, uint32_t physical, uint32_t &value) {
    return !timerFailure && QcomOSMInit::permittedSecureRead(physical) && static_cast<Secure *>(context)->read(physical, value);
}
bool secureWrite(void *context, uint32_t physical, uint32_t value) {
    return !timerFailure && QcomOSMInit::permittedSecureWrite(physical) && static_cast<Secure *>(context)->write(physical, value);
}
bool cprRead(void *, unsigned cluster, uint32_t offset, uint32_t &value) {
    if (timerFailure || cluster > 1 || (offset & 3) || offset >= 0x4000) return false;
    value = read32((cluster ? 0x179c4000U : 0x179c8000U) + offset);
    traceCPR(cluster, offset, value, false); return true;
}
bool cprWrite(void *, unsigned cluster, uint32_t offset, uint32_t value) {
    if (timerFailure || cluster > 1 || (offset & 3) || offset >= 0x4000) return false;
    traceCPR(cluster, offset, value, true);
    write32((cluster ? 0x179c4000U : 0x179c8000U) + offset, value); return true;
}
uint32_t runtimeRead(void *, unsigned, uint32_t offset) { return read32(0x179c0000U + offset); }
void runtimeWrite(void *, unsigned, uint32_t offset, uint32_t value) { write32(0x179c0000U + offset, value); }
void logOSM(const QcomOSMInit::Result &r) {
    bridge_puts("[OSM init] stage="); bridge_hex(unsigned(r.stage));
    bridge_puts(" error="); bridge_hex(unsigned(r.error));
    bridge_puts(" cluster="); bridge_hex(r.cluster);
    bridge_puts(" offset="); bridge_hex(r.offset);
    bridge_puts(" expected="); bridge_hex(r.expected);
    bridge_puts(" observed="); bridge_hex(r.observed);
    bridge_puts(" writes="); bridge_hex(r.writes);
    bridge_puts(" enabled="); bridge_hex(r.enabledMask); bridge_puts("\n");
}
void logClocks(const BridgeOSMFirmware::Evidence &e) {
    bridge_puts("[OSM clocks] failure="); bridge_hex(unsigned(e.failure));
    bridge_puts(" address="); bridge_hex(e.address);
    bridge_puts(" observed="); bridge_hex(e.raw);
    bridge_puts(" expected="); bridge_hex(e.expected); bridge_puts("\n");
}
bool qualifyCPU(unsigned cluster) {
    QcomOSMClock::State state{};
    const bool ready = QcomOSMClock::awaitBootRate(cluster,
        [&](uint32_t off) { return read32(QcomOSMClock::base(cluster) + off); },
        [](unsigned us) { waitUS(us); }, state);
    bridge_puts("[OSM CPU clock] cluster="); bridge_hex(cluster);
    bridge_puts(" ready="); bridge_hex(ready && !timerFailure);
    bridge_puts(" configured-hz="); bridge_hex(state.localPLLHz());
    bridge_puts(" expected-hz="); bridge_hex(QcomOSMClock::BootHz[cluster]);
    bridge_puts(" stable="); bridge_hex(state.stable);
    for (unsigned i = 0; i < 7; ++i) { bridge_puts(" raw="); bridge_hex(state.words[i]); }
    bridge_puts("\n");
    return ready && !timerFailure;
}
void logCPUFailure() {
    for (unsigned c = 0; c < 2; ++c) {
        const uint64_t base = QcomOSMInit::osmBase(c);
        // Stock v2 debugfs enables bit 30 before reading watchdog state/PC.
        const uint32_t trace = read32(base + 0x1f38);
        write32(base + 0x1f38, trace | (1U << 30)); waitUS(100);
        constexpr uint32_t offsets[] = {0x1004,0x1020,0x1028,0x1034,0x1b04,
            0x1f00,0x1f04,0x1f10,0x1f14,0x1f20,0x1f2c,0x1f38,0x1c00,0x1c74};
        for (uint32_t off : offsets) {
            bridge_puts("[OSM CPU failure] cluster="); bridge_hex(c);
            bridge_puts(" offset="); bridge_hex(off);
            bridge_puts(" value="); bridge_hex(read32(base + off)); bridge_puts("\n");
        }
        write32(base + 0x1f38, trace);
        const uint64_t cpr = c ? 0x179c4000 : 0x179c8000;
        bridge_puts("[OSM CPU failure] CPR cluster="); bridge_hex(c);
        bridge_puts(" control="); bridge_hex(read32(cpr + 4));
        bridge_puts(" status="); bridge_hex(read32(cpr + 8)); bridge_puts("\n");
    }
}
}

extern "C" int bridge_osm_prepare(EFI_BOOT_SERVICES *bs, void *tree) {
    prepared = false; recordProperty = nullptr; timerFailure = false;
    uint32_t bytes = 0;
    void *property = dt_property(tree, "performance-controller@179c0000", "qcom,cpu-osm-handoff", &bytes);
    if (!property || bytes != sizeof(QcomOSMHandoff::Record)) return -1;
    uint32_t words[sizeof(QcomOSMHandoff::Record) / 4];
    memcpy(words, property, sizeof(words));
    for (unsigned i = 0; i < sizeof(words) / 4; ++i)
        if (words[i] != (i == 1 ? sizeof(words) : 0)) return -2;
    BridgeOSMFirmware::Identity firmware{};
    if (!BridgeOSMFirmware::identity(bs, firmware)) {
        bridge_puts("[OSM identity] failure="); bridge_hex(unsigned(firmware.failure));
        bridge_puts(" status="); bridge_hex(firmware.status);
        bridge_puts(" chip="); bridge_hex(firmware.chipID);
        bridge_puts(" version="); bridge_hex(firmware.chipVersion);
        bridge_puts(" board="); bridge_hex(firmware.boardID); bridge_puts("\n");
        return -3;
    }
    identity = {firmware.chipID, firmware.chipVersion, firmware.boardID};
    if (!QcomCPRh::supported(identity)) return -4;
    bridge_puts("[OSM identity] chip="); bridge_hex(identity.chipID);
    bridge_puts(" version="); bridge_hex(identity.chipVersion);
    bridge_puts(" board="); bridge_hex(identity.boardID); bridge_puts("\n");
    uint64_t frequency;
    __asm__ volatile("mrs %0,cntfrq_el0" : "=r"(frequency));
    if (frequency != 19200000) return -5;
    EFI_GUID guid = EFI_CPU_ARCH_PROTOCOL_GUID;
    EFI_CPU_ARCH_PROTOCOL *cpu = nullptr;
    uint64_t el; __asm__ volatile("mrs %0,CurrentEL" : "=r"(el));
    if (el != 4) return -6;
    if (EFI_ERROR(bs->LocateProtocol(&guid, nullptr, reinterpret_cast<void **>(&cpu))) ||
        !cpu || !cpu->SetMemoryAttributes) return -6;
    // Always-on apertures only. No PLL retune, PMIC/SPMI write, or CPU mux
    // change is performed during EFI preparation.
    const BridgeOSMMMIO::IO mapping{cpu, attributesMMIO, translateMMIO};
    for (const auto &window : mmioWindows) {
        BridgeOSMMMIO::Evidence evidence{};
        const bool ready = BridgeOSMMMIO::prepare(mapping, window.address, window.bytes, evidence);
        logMMIO(evidence, ready, false);
        if (!ready) return -7;
    }
    EFI_PHYSICAL_ADDRESS mailbox = 0;
    if (EFI_ERROR(bs->AllocatePages(AllocateAnyPages, EfiLoaderData, 1, &mailbox)) || !mailbox)
        return -12;
    peerMailbox = mailbox;
    const uint64_t entry = reinterpret_cast<uint64_t>(&bridge_osm_peer_entry);
    const uint64_t sharedPages[] = {mailbox, entry & ~4095ULL};
    for (uint64_t address : sharedPages) {
        const auto par = translateMMIO(nullptr, address, false);
        if ((par & 1) || (par & BridgeOSMMMIO::PhysicalMask) != address) return -13;
    }
    for (unsigned i = 0; i < 4096 / 8; ++i)
        reinterpret_cast<uint64_t *>(mailbox)[i] = 0;
    bridge_clean(mailbox, mailbox + 4096);
    bridge_clean(entry & ~63ULL, (entry + 256 + 63) & ~63ULL);
    uint64_t rows[8]{};
    for (unsigned i = 0; i < 8; ++i) {
        const uint64_t address = QcomCPRh::kFuseBase + QcomCPRh::kFuseRowOffsets[i];
        rows[i] = read32(address) | (uint64_t(read32(address + 4)) << 32);
        const uint64_t check = read32(address) | (uint64_t(read32(address + 4)) << 32);
        if (rows[i] != check) return -8;
    }
    QcomCPRh::FuseRows fuses{};
    static_assert(sizeof(fuses) == sizeof(rows), "QFPROM row list");
    memcpy(&fuses, rows, sizeof(fuses));
    for (unsigned c = 0; c < 2; ++c)
        if (!QcomCPRh::calibrate(identity, fuses, c, plans[c])) return -9 - int(c);
    for (unsigned c = 0; c < 2; ++c) {
        if (read32(QcomOSMInit::osmBase(c) + 0x1004) & 1) return -11;
        bridge_puts("[OSM calibration] cluster="); bridge_hex(c);
        bridge_puts(" speed-bin="); bridge_hex(plans[c].speedBin);
        bridge_puts(" CPR-revision="); bridge_hex(plans[c].cprRevision);
        bridge_puts(" active-corners="); bridge_hex(plans[c].activeCount); bridge_puts("\n");
    }
    recordProperty = property; prepared = true;
    return 0;
}

extern "C" int bridge_osm_finalize(void) {
    if (!prepared || !recordProperty) return -1;
    prepared = false;
    // Boot services have ended. Recheck translations without calling EFI or
    // touching registers, so a lost aperture cannot cause a synchronous abort.
    const BridgeOSMMMIO::IO mapping{nullptr, nullptr, translateMMIO};
    for (const auto &window : mmioWindows) {
        BridgeOSMMMIO::Evidence mapped{};
        const bool ready = BridgeOSMMMIO::inspect(mapping, window.address, window.bytes, mapped);
        logMMIO(mapped, ready, true);
        if (!ready) return -17;
    }
    BridgeOSMFirmware::Evidence evidence{};
    if (!BridgeOSMFirmware::clocksPrepare(clockIO, evidence)) { logClocks(evidence); return -2; }
    PeerIO peerIO; BridgeOSMPeer::Result peer{};
    if (!BridgeOSMPeer::start(peerIO, reinterpret_cast<uint64_t>(&bridge_osm_peer_entry), peerMailbox, peer)) {
        logPeer(peer); return -18;
    }
    logPeer(peer);
    if (!BridgeOSMFirmware::clocksInspect(clockIO, evidence)) { logClocks(evidence); return -19; }
    for (unsigned c=0;c<2;++c) {
        bridge_puts("[OSM inherited LLM] cluster="); bridge_hex(c);
        bridge_puts(" policy="); bridge_hex(read32(QcomOSMInit::osmBase(c)+0x1034));
        bridge_puts(" limit="); bridge_hex(read32(QcomOSMInit::osmBase(c)+0x1b04)); bridge_puts("\n");
    }
    Monitor monitor; Secure secure(monitor);
    if (!secure.probe()) return -3;
    QcomOSMInit::IO io{&secure, osmRead, osmWrite, secureRead, secureWrite, barrier, delay};
    // clocksPrepare/Inspect decode the live parent and divider configuration.
    const QcomOSMInit::Clocks clocks{evidence.auxHz, evidence.osmHz, evidence.xoHz, evidence.ready};
    QcomOSMInit::Engine engine;
    QcomOSMInit::Result result{};
    if (!engine.inspect(io, identity, clocks, plans, result)) { logOSM(result); return -4; }
    const QcomCPRh::RegisterIO cprIO{nullptr, cprRead, cprWrite, barrier, delay};
    // Only reached after EFI exit, live clock verification and the read-only
    // inspection of both disabled OSM banks and the stock v2 boot parameters.
    // This is HLOS ownership of CPRh/OSM setup. Original TZ/ClockDxe must have
    // completed their protected sequencer, CPU PLL and boot mux setup; the
    // readable parameters do not independently prove those operations.
    const QcomCPRh::Ownership ownership{true, true, true, true};
    for (unsigned c = 0; c < 2; ++c)
        if (!QcomCPRh::inspect(plans[c], cprIO, ownership)) { logCPR(); return -5 - int(c); }
    for (unsigned c = 0; c < 2; ++c)
        if (!QcomCPRh::apply(plans[c], cprIO, ownership)) { logCPR(); return -7 - int(c); }
    if (!engine.prepare(io, identity, clocks, plans, result)) { logOSM(result); return -9; }
    if (!BridgeOSMFirmware::clocksInspect(clockIO, evidence)) { logClocks(evidence); return -10; }
    if (!engine.enable()) { logOSM(result); return -11; }
    logOSM(result);
    QcomPower::OSMController runtime;
    if (runtime.initialize({nullptr, runtimeRead, runtimeWrite, delay, nullptr}) != QcomPower::OK ||
        !runtime.stateCount(0) || !runtime.stateCount(1) || !runtime.rate(0) || !runtime.rate(1))
        return -12;
    // Match Xiaomi's final boot-rate stage after OSM enable. Keep the temporary
    // performance core online until both clusters have physically left aux.
    for (unsigned c = 0; c < 2; ++c)
        if (runtime.setRate(c, QcomOSMClock::BootHz[c]) != QcomPower::OK) {
            logCPUFailure(); return -21;
        }
    bool bootClocks = true;
    for (unsigned c = 0; c < 2; ++c) if (!qualifyCPU(c)) bootClocks = false;
    if (!bootClocks) { logCPUFailure(); return -22; }
    if (!BridgeOSMPeer::stop(peerIO, peer)) { logPeer(peer); return -20; }
    logPeer(peer);
    if (!qualifyCPU(0)) { logCPUFailure(); return -23; }
    if (!BridgeOSMFirmware::clocksInspect(clockIO, evidence)) { logClocks(evidence); return -13; }
    for (unsigned c = 0; c < 2; ++c) {
        if (!QcomCPRh::verify(plans[c], cprIO)) return -15;
        for (unsigned row = 0; row < 40; ++row) {
            const auto &expected = plans[c].osm[row];
            const uint64_t base = QcomOSMInit::osmBase(c) + 0x1150 + row * 32;
            if (read32(base) != row || read32(base + 4) != expected.frequency ||
                read32(base + 8) != expected.voltage || read32(base + 12) != expected.overrideValue ||
                read32(base + 20) != expected.spare) return -16;
        }
    }
    QcomOSMHandoff::Record record{};
    record.bytes = sizeof(record);
    record.chipID = identity.chipID; record.chipVersion = identity.chipVersion; record.boardID = identity.boardID;
    record.flags = QcomOSMHandoff::Required;
    record.firmwareProfile = QcomOSMTZ::Profile;
    record.auxHz = clocks.auxiliaryHz; record.osmHz = clocks.osmHz; record.xoHz = clocks.xoHz;
    auto read = [](unsigned c, uint32_t offset) { return read32(QcomOSMInit::osmBase(c) + offset); };
    for (unsigned c = 0; c < 2; ++c) {
        record.firmwareChecks[c] = result.firmwareChecks[c];
        record.lutDigest[c] = QcomOSMHandoff::lutDigest(read, c);
        record.initialDesired[c] = read(c, 0x1f10);
        record.enable[c] = read(c, 0x1004);
    }
    record.version = QcomOSMHandoff::Version;
    if (QcomOSMHandoff::validate(record, read) != QcomOSMHandoff::Status::Accepted) return -14;
    // Publication is the last operation. A partial/failed sequence leaves the
    // embedded DT at version zero and cannot authorize XNU frequency requests.
    record.version = 0;
    memcpy(recordProperty, &record, sizeof(record)); barrier(nullptr);
    const uint32_t version = QcomOSMHandoff::Version;
    memcpy(recordProperty, &version, sizeof(version)); barrier(nullptr);
    bridge_puts("[OSM handoff] calibrated CPRh, OSM LUT/policy and ACD verified; XNU owns desired-state requests\n");
    return 0;
}
