/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef _UFS_PROTOCOL_H
#define _UFS_PROTOCOL_H
#include <stdint.h>
#include <stddef.h>

namespace UFS {
constexpr unsigned MaxSlots = 32, MaxLUNs = 8, MaxPRDs = 256;
constexpr uint32_t MaxPRDBytes = 256 * 1024, MaxTransfer = 1024 * 1024;
constexpr unsigned MaxPending = 128;
struct PlatformContract {
    uint32_t version, flags, coreHz, uniproHz, addressBits, lanes;
    bool valid() const {
        return (version == 1 || version == 2) && !(flags & ~0x1fu) && (flags & 15) == 15 &&
            coreHz == 200000000 && uniproHz == 150000000 &&
            addressBits >= 32 && addressBits <= 40 && lanes >= 1 && lanes <= 2;
    }
};
enum Register : uint32_t {
    CAP = 0x00, VER = 0x08, AHIT = 0x18, IS = 0x20, IE = 0x24,
    HCS = 0x30, HCE = 0x34, UECPA = 0x38, UECDL = 0x3c,
    UECN = 0x40, UECT = 0x44, UECDME = 0x48, IACR = 0x4c,
    UTRLBA = 0x50, UTRLBAU = 0x54, UTRLDBR = 0x58,
    UTRLCLR = 0x5c, UTRLRSR = 0x60, UTMRLBA = 0x70,
    UTMRLBAU = 0x74, UTMRLDBR = 0x78, UTMRLRSR = 0x80,
    UICCMD = 0x90, UICARG1 = 0x94, UICARG2 = 0x98, UICARG3 = 0x9c,
    SYS1CLK = 0xc0, CFG1 = 0xdc, HWVER = 0xe4
};
constexpr uint32_t TransferDone = 1, UICError = 4, PowerDone = 0x10,
    HibernateExit = 0x20, HibernateEnter = 0x40, UICDone = 0x400;
constexpr uint32_t Fatal = 0x800 | 0x10000 | 0x20000 | 0x40000 | 0x80;
constexpr uint32_t Interrupts = TransferDone | UICError | Fatal;
constexpr uint8_t Command = 0x01, NOPOut = 0, Query = 0x16,
    Response = 0x21, NOPIn = 0x20, QueryResponse = 0x36;
constexpr uint16_t TXGear = 0x1568, RXGear = 0x1583,
    TXLanes = 0x1560, RXLanes = 0x1580, ConnectedTX = 0x1561,
    ConnectedRX = 0x1581, MaxRXHS = 0x1587, TXTermination = 0x1569,
    RXTermination = 0x1584, HSSeries = 0x156a, PowerMode = 0x1571;

inline void zero(void *p, size_t n) { auto b = static_cast<uint8_t *>(p); while (n--) *b++ = 0; }
inline uint16_t be16(const uint8_t *p) { return uint16_t(p[0]) << 8 | p[1]; }
inline uint32_t be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline uint64_t be64(const uint8_t *p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }
inline void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
inline void put32(uint8_t *p, uint32_t v) { put16(p, v >> 16); put16(p + 2, v); }
inline void put64(uint8_t *p, uint64_t v) { put32(p, v >> 32); put32(p + 4, v); }
// SDM835 and the target Apple kernel are little endian. UPIUs and SCSI use
// big endian fields; UFSHCI descriptors and registers use little endian.
struct PRD { uint64_t address; uint32_t reserved, byteCount; };
struct Transfer {
    uint32_t header, reserved1, ocs, reserved3;
    uint64_t commandAddress;
    uint16_t responseLength, responseOffset, prdtLength, prdtOffset;
};
struct alignas(128) CommandDescriptor {
    uint8_t request[512];
    uint8_t response[512];
    PRD prdt[MaxPRDs];
};
static_assert(sizeof(PRD) == 16 && sizeof(Transfer) == 32, "UFSHCI descriptor ABI");
static_assert(offsetof(CommandDescriptor, response) == 512 &&
    offsetof(CommandDescriptor, prdt) == 1024 && sizeof(CommandDescriptor) == 5120, "UCD ABI");

inline bool addressFits(uint64_t address, uint64_t size, unsigned bits) {
    if (!size || bits < 32 || bits > 64 || size - 1 > UINT64_MAX - address) return false;
    return bits == 64 || (address + size - 1) >> bits == 0;
}
inline bool appendPRD(PRD *prds, unsigned &count, uint64_t address, uint64_t size, unsigned bits) {
    if ((address | size) & 3 || !addressFits(address, size, bits)) return false;
    while (size) {
        if (count == MaxPRDs) return false;
        uint32_t part = size > MaxPRDBytes ? MaxPRDBytes : uint32_t(size);
        prds[count++] = {address, 0, part - 1};
        address += part; size -= part;
    }
    return true;
}
inline void initTransfer(Transfer &t, uint64_t address, unsigned prds, unsigned direction) {
    zero(&t, sizeof(t));
    // Command type UFS storage (1), interrupt on completion, DD=1 write/2 read.
    t.header = (1u << 28) | (1u << 24) | (direction << 25);
    t.ocs = 0xf; t.commandAddress = address;
    t.responseLength = 512 / 4; t.responseOffset = 512 / 4;
    t.prdtOffset = 1024 / 4; t.prdtLength = prds;
}
inline void scsi(uint8_t *p, uint8_t lun, uint8_t tag, const uint8_t *cdb,
    uint32_t bytes, bool write, bool ordered = false) {
    zero(p, 512); p[0] = Command; p[1] = bytes ? (write ? 0x20 : 0x40) : 0;
    p[1] |= ordered ? 1 : 0; p[2] = lun; p[3] = tag;
    put32(p + 12, bytes);
    for (unsigned i = 0; i < 16; ++i) p[16 + i] = cdb[i];
}
inline bool readWriteCDB(uint8_t *cdb, uint64_t block, uint64_t blocks,
    uint32_t blockSize, uint64_t capacity, bool write, bool fua, uint32_t &bytes) {
    if (!blocks || blocks > UINT32_MAX || block >= capacity || blocks > capacity - block ||
        blockSize < 512 || blockSize > 65536 || (blockSize & (blockSize - 1)) ||
        blocks > MaxTransfer / blockSize) return false;
    bytes = uint32_t(blocks * blockSize);
    if (block <= UINT32_MAX && blocks <= UINT16_MAX && blocks - 1 <= UINT32_MAX - block) {
        zero(cdb, 16); cdb[0] = write ? 0x2a : 0x28; cdb[1] = fua ? 8 : 0;
        put32(cdb + 2, uint32_t(block)); put16(cdb + 7, uint16_t(blocks)); return true;
    }
    zero(cdb, 16); cdb[0] = write ? 0x8a : 0x88; cdb[1] = fua ? 8 : 0;
    put64(cdb + 2, block); put32(cdb + 10, uint32_t(blocks)); return true;
}
inline void query(uint8_t *p, uint8_t tag, uint8_t opcode, uint8_t idn,
    uint8_t index, uint16_t length) {
    zero(p, 512); p[0] = Query; p[3] = tag;
    p[5] = opcode == 1 || opcode == 3 || opcode == 5 ? 1 : 0x81;
    p[12] = opcode; p[13] = idn; p[14] = index; put16(p + 18, length);
}
enum class Result { Good, Controller, Protocol, QueryRejected, CheckCondition, Busy, NoDevice, ShortTransfer };
// UniPro already retransmits NAC/replay events. PA_INIT and upper-layer
// errors require software recovery; this driver fails them without replaying
// writes. Keep unknown codes conservative. See ufshcd_update_uic_error.
inline bool uicRequiresRecovery(uint32_t pa,uint32_t dl,uint32_t n,uint32_t t,uint32_t dme) {
    constexpr uint32_t valid=1u<<31;
    return ((pa&valid) && (pa&~(valid|0x10u))) ||
        ((dl&valid) && (dl&~(valid|1u))) ||
        ((n&valid) && (n&~valid)) || ((t&valid) && (t&~valid)) ||
        ((dme&valid) && (dme&~valid));
}
struct Completion {
    Result result; uint32_t bytes; uint8_t status, senseKey, asc, ascq;
};
inline Completion response(const Transfer &t, const uint8_t *p, uint8_t transaction,
    uint8_t tag, uint8_t lun, uint32_t expected) {
    Completion r = {Result::Protocol, 0, 0, 0, 0, 0};
    if ((t.ocs & 0xf) != 0) { r.result = Result::Controller; return r; }
    if (p[0] != transaction || p[3] != tag || p[2] != lun || p[4] || p[8] ||
        (p[9] & ~1u) || (transaction != Response && p[9])) return r;
    if (transaction == NOPIn) { if (!p[6] && !p[7] && !be16(p + 10)) r.result = Result::Good; return r; }
    if (transaction == QueryResponse) {
        if (p[7] || be16(p + 10) > 255) return r;
        r.result = p[6] ? Result::QueryRejected : Result::Good;
        return r;
    }
    r.status = p[7];
    // Samsung reports TARGET FAILURE (1) together with CHECK CONDITION.
    // The SCSI sense payload still carries the actionable reset/not-ready
    // condition; rejecting it as a malformed UPIU prevents LU discovery.
    if (p[6] && (p[6] != 1 || r.status != 2)) return r;
    if (r.status == 8 || r.status == 0x28) { r.result = Result::Busy; return r; }
    if (r.status == 2) {
        uint16_t data = be16(p + 10), sense = be16(p + 32);
        if (data < 2 || data > 480 || sense > data - 2 || sense > 478) return r;
        r.result = Result::CheckCondition;
        if (sense >= 14 && ((p[34] & 0x7f) == 0x70 || (p[34] & 0x7f) == 0x71)) {
            r.senseKey = p[36] & 15; r.asc = p[46]; r.ascq = p[47];
        } else if (sense >= 4 && ((p[34] & 0x7f) == 0x72 || (p[34] & 0x7f) == 0x73)) {
            r.senseKey = p[35] & 15; r.asc = p[36]; r.ascq = p[37];
        }
        return r;
    }
    if (r.status || be16(p + 10) || (p[1] & 0x40)) return r;
    uint32_t residual = be32(p + 12);
    if (residual > expected || (!(p[1] & 0x20) && residual)) return r;
    r.bytes = expected - residual;
    r.result = residual ? Result::ShortTransfer : Result::Good;
    return r;
}
inline uint32_t completedSlots(uint32_t active, uint32_t doorbell) { return active & ~doorbell; }
inline uint32_t slotMask(unsigned count) { return count == 32 ? UINT32_MAX : (1u << count) - 1; }

// Select from the actual pending FIFO. An ordered request is a controller
// barrier (stronger than a per-LU SCSI ordering requirement). Admission and
// this selection are always protected by the Apple IOCommandGate.
template<class Request, class LUN> bool selectPending(Request *&head, Request *&tail,
    unsigned &pendingCount, Request *const *active, unsigned slots, const LUN *luns,
    uint32_t activeMask, unsigned &tag, Request *&request) {
    if (!head) return false;
    for (unsigned i = 0; i < slots; ++i) if (active[i] && active[i]->ordered) return false;
    request = head;
    if (request->ordered && activeMask) return false;
    tag = 0; while (tag < slots && active[tag]) ++tag;
    if (tag == slots) return false;
    if (request->responseType == Response && luns[request->lun].enabled) {
        unsigned count = 0;
        for (unsigned i = 0; i < slots; ++i) if (active[i] && active[i]->lun == request->lun) ++count;
        if (count >= luns[request->lun].depth) return false;
    }
    head = request->next; if (!head) tail = nullptr;
    request->next = nullptr; --pendingCount; return true;
}

// Shared production/model register sequencing. Register accessors implement
// device barriers; delays are bounded and only used before live I/O or after
// admission has been closed. There is no polling on the read/write fast path.
template<class IO> class Host {
    IO &io;
public:
    explicit Host(IO &registers) : io(registers) {}
    bool wait(uint32_t reg, uint32_t mask, uint32_t value, unsigned usec) {
        for (unsigned i = 0; i < usec / 10 + 1; ++i) {
            if ((io.read(reg) & mask) == value) return true;
            io.delay(10);
        }
        return false;
    }
    bool stop() {
        io.write(IE, 0); io.write(UTRLRSR, 0); io.write(UTMRLRSR, 0);
        io.write(HCE, 0);
        // A doorbell clear is NOT DMA quiescence. HCE disable is the fence
        // required before completion, unmapping or tag reuse on failure.
        return wait(HCE, 1, 0, 100000);
    }
    bool enable() {
        if (!stop()) return false;
        io.write(IS, UINT32_MAX); io.write(HCE, 1);
        return wait(HCE, 1, 1, 100000) && wait(HCS, 8, 8, 100000);
    }
    bool uic(uint8_t opcode, uint32_t arg1, uint32_t arg3, uint32_t *value = nullptr,
        uint32_t event = 0) {
        if (!wait(HCS, 8, 8, 500000)) return false;
        // Old Qcom revisions require separation between DME operations.
        io.delay(1000); io.write(IS, UICDone | event);
        io.write(UICARG1, arg1); io.write(UICARG2, 0); io.write(UICARG3, arg3);
        io.write(UICCMD, opcode);
        if (!wait(IS, UICDone, UICDone, 500000)) return false;
        bool ok = !(io.read(UICARG2) & 0xff);
        if (value) *value = io.read(UICARG3);
        io.write(IS, UICDone);
        if (!ok || !event) return ok;
        if (!wait(IS, event, event, 500000)) return false;
        io.write(IS, event);
        return ((io.read(HCS) >> 8) & 7) == 1; // UPMCRS=PWR_LOCAL
    }
    bool get(uint16_t mib, uint32_t &value, bool peer = false) {
        return uic(peer ? 3 : 1, uint32_t(mib) << 16, 0, &value);
    }
    bool set(uint16_t mib, uint32_t value, uint32_t event = 0, bool peer = false) {
        return uic(peer ? 4 : 2, uint32_t(mib) << 16, value, nullptr, event);
    }
    bool configureTimeouts() {
        constexpr uint32_t values[] = {8191, 65535, 32767};
        for (unsigned i = 0; i < 6; ++i)
            if (!set(0x15b0 + i, values[i % 3])) return false;
        for (unsigned i = 0; i < 3; ++i)
            if (!set(0xd041 + i, values[i])) return false;
        return true;
    }
    bool tuneActivation() {
        uint32_t localGran, peerGran, localTime, peerTime;
        constexpr uint32_t units[] = {1, 4, 8, 16, 32, 100};
        if (!get(0x15aa, localGran) || !get(0x15aa, peerGran, true) ||
            localGran < 1 || localGran > 6 || peerGran < 1 || peerGran > 6 ||
            !get(0x15a8, localTime) || !get(0x15a8, peerTime, true) ||
            localTime > 255 || peerTime > 255) return false;
        uint32_t localUS = localTime * units[localGran - 1];
        uint32_t peerUnit = units[peerGran - 1];
        if (localUS < peerTime * peerUnit) return true;
        uint32_t newPeer = localUS / peerUnit + 1;
        return newPeer <= 255 && set(0x15a8, newPeer, 0, true);
    }
};
} // namespace UFS
#endif
