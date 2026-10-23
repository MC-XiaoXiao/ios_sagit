// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "XHCIRing.h"
#include "XHCIPorts.h"
#include "XHCIFrames.h"

namespace XHCI {
struct alignas(64) SegmentTable {
    uint64_t address;
    uint32_t entries, reserved;
    uint8_t padding[48];
};
static_assert(sizeof(SegmentTable) == 64, "one aligned xHCI event segment table");
struct MemoryLayout {
    uint64_t command, dcbaa, segmentTable, events, scratchpadArray;
    SegmentTable *tableCPU;
    unsigned eventEntries, scratchpads, slots;
};
enum class State : uint8_t { Unprobed, Halted, Resetting, WaitingReady, Reset, Configured, Running, Fault };

// Operational/runtime MMIO for the xHCI window within DWC3. Reset completion
// is polled by the adapter's timer, so a slow reset never spins under a gate
// for hundreds of milliseconds. All memory is mapped in the device DMA domain.
template<class IO> class Core {
    IO &io;
    State state = State::Unprobed;
    uint32_t op = 0, runtime = 0, doorbells = 0;
    unsigned slots = 0, ports = 0, scratchpads = 0, bits = 0, stride = 0;
    uint64_t deadline = 0, lastTime = 0;
    unsigned eventEntries = 0;
    uint64_t eventBase = 0;
    bool registersValid = false;
    bool portPowerControl = false;
    bool contiguousFrameIDs = false;
    unsigned isochronousThreshold = 0;
    USB::Result fault(USB::Result result) { state = State::Fault; return result; }
    void write64(uint32_t reg, uint64_t value) {
        io.write(reg, uint32_t(value)); io.write(reg + 4, uint32_t(value >> 32));
    }
    USB::Result wait(uint32_t reg, uint32_t mask, uint32_t value, unsigned us) {
        for (unsigned n = 0; n <= us; ++n) {
            uint32_t actual = io.read(reg);
            if (actual == UINT32_MAX) return fault(USB::Result::HardwareError);
            if ((actual & mask) == value) return USB::Result::Success;
            if (n < us) io.delay(1);
        }
        return fault(USB::Result::Timeout);
    }
public:
    explicit Core(IO &access) : io(access) {}
    State currentState() const { return state; }
    unsigned maxSlots() const { return slots; }
    unsigned maxPorts() const { return ports; }
    unsigned scratchpadCount() const { return scratchpads; }
    unsigned dmaBits() const { return bits; }
    unsigned contextStride() const { return stride; }
    bool portPowerControlSupported() const { return portPowerControl; }
    bool contiguousFrameIDSupported() const { return contiguousFrameIDs; }
    unsigned isochronousSchedulingThreshold() const { return isochronousThreshold; }
    uint32_t frameCounterOffset() const { return registersValid ? runtime : 0; }
    USB::Result microFrameCount(uint32_t &raw) {
        if (state!=State::Running) return USB::Result::NotReady;
        const auto value=io.read(runtime);
        if (value==UINT32_MAX) return fault(USB::Result::HardwareError);
        raw=value&MicroFrameMask;
        return USB::Result::Success;
    }

    USB::Result probe(unsigned aperture, unsigned platformBits) {
        if (state != State::Unprobed) return USB::Result::Busy;
        if (aperture < 0xc100 || platformBits < 32 || platformBits > 40)
            return USB::Result::Invalid;
        uint32_t cap = io.read(0), hcs1 = io.read(4), hcs2 = io.read(8), hcc = io.read(16);
        if (cap == UINT32_MAX || hcs1 == UINT32_MAX || hcs2 == UINT32_MAX || hcc == UINT32_MAX)
            return fault(USB::Result::HardwareError);
        op = cap & 0xff; slots = hcs1 & 0xff; ports = hcs1 >> 24;
        unsigned version = cap >> 16, interrupters = (hcs1 >> 8) & 0x7ff;
        runtime = io.read(24) & ~31u; doorbells = io.read(20) & ~3u;
        scratchpads = ((hcs2 >> 27) & 31) | ((hcs2 >> 16) & 0x3e0);
        bits = hcc & 1 ? platformBits : 32; stride = hcc & 4 ? 64 : 32;
        portPowerControl = hcc & 8;
        contiguousFrameIDs = hcc & (1u<<11);
        isochronousThreshold = (hcs2&7)*((hcs2&8) ? 8 : 1);
        if (version < 0x100 || version > 0x110 || op < 0x20 || op > 0x80 || (op & 3) ||
            !slots || !ports || ports > 127 || !interrupters ||
            op + 0x400 + ports * 16 > 0xc100 ||
            runtime < op + 0x400 + ports * 16 || runtime > 0xc0c0 ||
            doorbells < op + 0x400 + ports * 16 || doorbells > 0xc100 - (slots + 1) * 4 ||
            (runtime < doorbells + (slots + 1) * 4 && runtime + 0x40 > doorbells))
            return fault(USB::Result::Unsupported);
        registersValid = true;
        // Firmware can leave a live controller. halt must precede reset or
        // replacing DCBAAP/CRCR; allocations cannot invalidate its old DMA.
        return halt();
    }

    USB::Result halt() {
        if (!registersValid) return USB::Result::NotReady;
        uint32_t cmd = io.read(op);
        if (cmd == UINT32_MAX) return fault(USB::Result::HardwareError);
        io.write(op, cmd & ~(5u|(1u<<10)));
        auto result = wait(op + 4, 1, 1, 16000);
        if (result == USB::Result::Success) state = State::Halted;
        return result;
    }

    USB::Result beginReset(uint64_t nowUS) {
        if (state != State::Halted) return USB::Result::NotReady;
        if (nowUS > UINT64_MAX - 250000) return USB::Result::Invalid;
        uint32_t cmd = io.read(op), status = io.read(op + 4);
        if (cmd == UINT32_MAX || status == UINT32_MAX) return fault(USB::Result::HardwareError);
        if ((cmd & 2) || (status & (1u << 11))) return USB::Result::Busy;
        io.write(op, 2); state = State::Resetting;
        lastTime = nowUS; deadline = nowUS + 250000;
        eventEntries = 0; eventBase = 0;
        return USB::Result::Success;
    }
    USB::Result advanceReset(uint64_t nowUS) {
        if (state != State::Resetting && state != State::WaitingReady) return USB::Result::NotReady;
        if (nowUS < lastTime) return USB::Result::Invalid;
        lastTime = nowUS;
        if (state == State::Resetting) {
            uint32_t cmd = io.read(op);
            if (cmd == UINT32_MAX) return fault(USB::Result::HardwareError);
            if (cmd & 2) return nowUS >= deadline ? fault(USB::Result::Timeout) : USB::Result::Busy;
            state = State::WaitingReady;
            // CNR shares the original reset deadline; polling late cannot
            // restart or extend the hardware's deadline indefinitely.
        }
        uint32_t status = io.read(op + 4);
        if (status == UINT32_MAX || (status & ((1u << 12) | 4))) return fault(USB::Result::HardwareError);
        if (status & (1u << 11)) return nowUS >= deadline ? fault(USB::Result::Timeout) : USB::Result::Busy;
        if (!(status & 1)) return fault(USB::Result::HardwareError);
        state = State::Reset;
        return USB::Result::Success;
    }

    USB::Result configure(const MemoryLayout &layout) {
        if (state != State::Reset) return USB::Result::NotReady;
        const uint32_t pageSizes = io.read(op + 8);
        if (pageSizes == UINT32_MAX) return fault(USB::Result::HardwareError);
        if (!layout.slots || layout.slots > slots || layout.scratchpads != scratchpads ||
            !layout.tableCPU || (reinterpret_cast<uintptr_t>(layout.tableCPU) & 63) ||
            !USB::dmaRange(layout.command, 64, bits, 64) ||
            !USB::dmaRange(layout.dcbaa, uint64_t(layout.slots + 1) * 8, bits, 64) ||
            !USB::dmaRange(layout.segmentTable, sizeof(SegmentTable), bits, 64) ||
            !segment(layout.events, layout.eventEntries, bits) ||
            (scratchpads && !USB::dmaRange(layout.scratchpadArray, uint64_t(scratchpads) * 8, bits, 64)) ||
            !(pageSizes & 1)) return USB::Result::Invalid;
        // Adapter has already initialized the coherent command/event rings,
        // DCBAA, scratchpad pointer array and each 4 KiB scratchpad page.
        *layout.tableCPU = {};
        layout.tableCPU->address = layout.events;
        layout.tableCPU->entries = layout.eventEntries;
        io.dmaWriteBarrier();
        io.write(op + 0x38, layout.slots);
        write64(op + 0x30, layout.dcbaa);
        write64(op + 0x18, layout.command | 1); // initial ring cycle is 1
        io.write(runtime + 0x20, 0); // interrupter disabled during setup
        io.write(runtime + 0x24, 4000); // 1 ms moderation, units of 250 ns
        io.write(runtime + 0x28, 1); // one event segment
        write64(runtime + 0x30, layout.segmentTable);
        write64(runtime + 0x38, layout.events);
        eventEntries = layout.eventEntries; eventBase = layout.events;
        state = State::Configured;
        return USB::Result::Success;
    }
    USB::Result run() {
        if (state != State::Configured) return USB::Result::NotReady;
        const auto cmd=io.read(op);
        if (cmd==UINT32_MAX) return fault(USB::Result::HardwareError);
        io.write(runtime + 0x20, 3); // clear old IP, set IE
        io.write(op, cmd | 5u | (1u<<10)); // EWE: MFINDEX Wrap Events
        state = State::Running;
        return wait(op + 4, 1, 0, 1000);
    }
    USB::Result doorbell(unsigned slot, unsigned endpoint, unsigned stream = 0) {
        if (state != State::Running) return USB::Result::NotReady;
        // USB streams require stream-context allocation, which is not enabled
        // by this controller initialization path.
        if (slot > slots || endpoint > 31 || stream || (!slot && endpoint) || (slot && !endpoint))
            return USB::Result::Invalid;
        io.dmaWriteBarrier();
        io.write(doorbells + slot * 4, endpoint);
        return USB::Result::Success;
    }
    USB::Result acknowledgeEvents(uint64_t dequeue) {
        if (state != State::Running) return USB::Result::NotReady;
        if ((dequeue & 15) || dequeue < eventBase || dequeue - eventBase >= uint64_t(eventEntries) * 16)
            return USB::Result::Invalid;
        // Clear the old interrupt indications BEFORE releasing EHB. A new
        // event after EHB is released must not be erased by a later IP/EINT
        // clear. EHB release is the final device-visible operation.
        io.dmaWriteBarrier();
        io.write(op + 4, 8); // USBSTS.EINT only, no unrelated W1C bits
        io.write(runtime + 0x20, 3); // IP: W1C, preserve IE
        write64(runtime + 0x38, dequeue | 8); // ERDP.EHB: W1C
        return USB::Result::Success;
    }
    USB::Result interruptStatus(uint32_t &status,bool &pending) {
        pending=false;
        if (state!=State::Running) return USB::Result::NotReady;
        status=io.read(op+4);const auto iman=io.read(runtime+0x20);
        if (status==UINT32_MAX || iman==UINT32_MAX || (status&((1u<<12)|4u|1u)))
            return fault(USB::Result::HardwareError);
        pending=(status&8u) || (iman&1u);
        return USB::Result::Success;
    }
    USB::Result maskInterrupts() {
        if (!registersValid) return USB::Result::NotReady;
        // IP is RW1C: writing zero preserves its evidence, while disabling IE.
        io.write(runtime+0x20,0);
        const auto iman=io.read(runtime+0x20); // flush posted disable
        return iman!=UINT32_MAX && !(iman&2) ? USB::Result::Success : fault(USB::Result::HardwareError);
    }
    USB::Result readPort(unsigned number, uint32_t &status) {
        if (state != State::Running) return USB::Result::NotReady;
        if (!number || number > ports) return USB::Result::Invalid;
        status = io.read(op + 0x400 + (number - 1) * 16);
        return status == UINT32_MAX ? fault(USB::Result::HardwareError) : USB::Result::Success;
    }
    USB::Result writePort(unsigned number,uint32_t mask,uint32_t value,uint32_t strobes) {
        if ((mask&PortSC::Power) && !portPowerControl) return USB::Result::Unsupported;
        if ((mask&~PortSC::Persistent) || (value&~mask) || (strobes&~PortSC::Strobes) ||
            ((mask&PortSC::LinkMask) && ((mask&PortSC::LinkMask)!=PortSC::LinkMask || !(strobes&PortSC::LinkWrite))) ||
            ((strobes&PortSC::LinkWrite) && (mask&PortSC::LinkMask)!=PortSC::LinkMask) ||
            ((strobes&PortSC::Reset) && (strobes&PortSC::WarmReset))) return USB::Result::Invalid;
        uint32_t raw=0;auto r=readPort(number,raw);if (r!=USB::Result::Success) return r;
        // Preserve only RWS fields. In particular, never echo PED (RW1C),
        // old change bits, reset strobes, or unrelated reserved registers.
        const auto offset=op+0x400+(number-1)*16;
        io.write(offset,((raw&PortSC::Persistent)&~mask)|value|strobes);
        return io.read(offset)!=UINT32_MAX ? USB::Result::Success : fault(USB::Result::HardwareError);
    }
};
}
