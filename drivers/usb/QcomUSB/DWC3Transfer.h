// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "DWC3Registers.h"
#include "USBProtocol.h"

namespace DWC3 {
struct DMASegment { uint64_t address; uint32_t length; };
struct TransferCompletion {
    USB::Result result;
    uint64_t cookie, epoch;
    uint32_t bytes;
};
enum class TransferState : uint8_t { Uninitialized, Idle, Active, Stopping, Fault };

// One active TD per physical endpoint. The IOKit adapter queues requests and
// supplies segments from IODMACommand, coherent TRBs, and an OUT bounce buffer
// when requested length is not a multiple of maximumPacketSize. No DMA map or
// Apple completion is released inside this class. Completion returns that
// responsibility to the adapter only after a hardware event proves it idle.
template<class Controller, class Order> class Transfer {
    Controller &controller;
    Order &order;
    volatile TRB *trbs = nullptr;
    uint64_t dma = 0, epoch = 0, cookie = 0;
    unsigned number = 0, capacity = 0, used = 0, resource = 0;
    uint32_t requested = 0;
    TRBType activeType = TRBType::Normal;
    uint32_t programmed[64]{};
    TransferState state = TransferState::Uninitialized;
    USB::Result abortReason = USB::Result::Aborted;

    USB::Result fail(USB::Result result) {
        state = TransferState::Fault;
        return result;
    }
    void finish(USB::Result result, uint32_t bytes, TransferCompletion &completion) {
        completion = {result, cookie, epoch, bytes};
        resource = used = requested = 0;
        cookie = 0; state = TransferState::Idle;
    }
public:
    Transfer(Controller &hardware, Order &barriers) : controller(hardware), order(barriers) {}
    TransferState currentState() const { return state; }
    bool ownsBuffers() const {
        return state == TransferState::Active || state == TransferState::Stopping ||
            state == TransferState::Fault;
    }
    unsigned physicalEndpoint() const { return number; }
    unsigned transferResource() const { return resource; }

    USB::Result initialize(unsigned ep, TRB *memory, uint64_t address,
                           unsigned count, uint64_t roleEpoch) {
        if (state != TransferState::Uninitialized) return USB::Result::Busy;
        if (ep >= controller.endpointCount() || !memory ||
            (reinterpret_cast<uintptr_t>(memory) & 15) || count < 1 || count > 64 ||
            reinterpret_cast<uintptr_t>(memory) > UINTPTR_MAX - count * sizeof(TRB) ||
            !USB::dmaRange(address, count * sizeof(TRB), controller.dmaBits(), 16))
            return USB::Result::Invalid;
        trbs = memory; dma = address; capacity = count; number = ep; epoch = roleEpoch;
        for (unsigned i = 0; i < capacity; ++i) {
            trbs[i].addressLo = trbs[i].addressHi = trbs[i].size = trbs[i].control = 0;
        }
        order.dmaWriteBarrier(); state = TransferState::Idle;
        return USB::Result::Success;
    }

    USB::Result submit(const DMASegment *segments, unsigned count, uint32_t length,
        unsigned packetSize, TRBType type, uint64_t requestCookie, bool appendZero = false) {
        if (state != TransferState::Idle) return USB::Result::NotReady;
        if (!packetSize || packetSize > 1024 || !requestCookie ||
            (count && !segments) || count > capacity ||
            (type != TRBType::Normal && type != TRBType::Setup &&
             type != TRBType::ControlData && type != TRBType::Status2 && type != TRBType::Status3))
            return USB::Result::Invalid;
        const bool in = number & 1;
        if (type != TRBType::Normal && number > 1) return USB::Result::Invalid;
        if ((type == TRBType::Setup && (number || length != 8 || count != 1)) ||
            ((type == TRBType::Status2 || type == TRBType::Status3) && (length || count)) ||
            (appendZero && (!in || (type != TRBType::Normal && type != TRBType::ControlData) ||
                !length || length % packetSize)))
            return USB::Result::Invalid;
        uint64_t total = 0;
        unsigned needed = count ? count : 1;
        if (appendZero) ++needed;
        if (needed > capacity) return USB::Result::Busy;
        const uintptr_t source = reinterpret_cast<uintptr_t>(segments);
        const uintptr_t destination = reinterpret_cast<uintptr_t>(trbs);
        if (count && (source > UINTPTR_MAX - count * sizeof(DMASegment) ||
            (source < destination + capacity * sizeof(TRB) &&
             destination < source + count * sizeof(DMASegment)))) return USB::Result::Invalid;
        // TRB lengths are 24 bits. Bound each segment and its entire DMA
        // range before touching descriptors or handing ownership to hardware.
        for (unsigned i = 0; i < count; ++i) {
            if (!segments[i].length || segments[i].length > TRBLengthMask ||
                !USB::dmaRange(segments[i].address, segments[i].length, controller.dmaBits()))
                return USB::Result::Invalid;
            if (segments[i].address < dma + capacity * sizeof(TRB) &&
                dma < segments[i].address + segments[i].length) return USB::Result::Invalid;
            total += segments[i].length;
            if (total > UINT32_MAX) return USB::Result::Invalid;
        }
        if ((!count && length) || total < length || (in && total != length))
            return USB::Result::Invalid;
        if (!in && type != TRBType::Setup && type != TRBType::Status2 && type != TRBType::Status3) {
            // OUT hardware can write a complete packet. Every SG boundary
            // preceding the end must also be packet-aligned. Extra capacity
            // belongs to a mapped bounce buffer, never adjacent user memory.
            uint64_t rounded = (uint64_t(length) + packetSize - 1) / packetSize * packetSize;
            if (!length) rounded = packetSize;
            if (total != rounded) return USB::Result::Invalid;
            for (unsigned i = 0; i + 1 < count; ++i)
                if (segments[i].length % packetSize) return USB::Result::Invalid;
        } else if (total != length) return USB::Result::Invalid;

        for (unsigned i = 0; i < needed; ++i) {
            uint64_t address = i < count ? segments[i].address : 0;
            uint32_t bytes = i < count ? segments[i].length : 0;
            programmed[i] = bytes;
            trbs[i].addressLo = uint32_t(address);
            trbs[i].addressHi = uint32_t(address >> 32);
            trbs[i].size = bytes;
            uint32_t control = uint32_t(type) << 4 | InterruptShort;
            if (i + 1 == needed) control |= Last | InterruptComplete;
            else control |= Chain;
            // HWO on the first descriptor is published last. STARTTRANSFER
            // follows the second barrier; hardware cannot see a partial TD.
            trbs[i].control = control | (i ? Owned : 0);
        }
        order.dmaWriteBarrier(); trbs[0].control |= Owned; order.dmaWriteBarrier();
        used = needed; requested = length; cookie = requestCookie;
        activeType = type;
        state = TransferState::Active;
        uint32_t reply = 0;
        const auto result = controller.startTransfer(number, dma, type, reply);
        if (result != USB::Result::Success) return fail(result);
        resource = (reply >> 16) & 127;
        // EP0 OUT can legally use transfer resource 0, including DATA and
        // STATUS stages. Other endpoints require a nonzero resource index.
        if (!resource && number != 0) return fail(USB::Result::HardwareError);
        return USB::Result::Success;
    }

    USB::Result cancel(USB::Result reason = USB::Result::Aborted) {
        if (reason != USB::Result::Aborted && reason != USB::Result::Timeout)
            return USB::Result::Invalid;
        if (state == TransferState::Stopping) return USB::Result::Pending;
        if (state != TransferState::Active) return USB::Result::NotReady;
        abortReason = reason; state = TransferState::Stopping;
        uint32_t reply = 0;
        auto result = controller.command(number, Command::End, 0, 0, 0,
            CommandForce | CommandInterrupt | resource << 16, reply);
        if (result != USB::Result::Pending)
            return fail(result == USB::Result::Success ? USB::Result::HardwareError : result);
        return result;
    }

    USB::Result handle(Event event, uint64_t roleEpoch, TransferCompletion &completion) {
        if (roleEpoch != epoch || event.device() || event.endpointNumber() != number)
            return USB::Result::Invalid;
        if (!ownsBuffers() || state == TransferState::Fault) return USB::Result::NotReady;
        if (event.type() == 7 && ((event.parameters() >> 8) & 15) == unsigned(Command::End)) {
            if (state != TransferState::Stopping) return USB::Result::Invalid;
            if (event.status()) return fail(USB::Result::HardwareError);
            order.dmaReadBarrier();
            finish(abortReason, 0, completion);
            return USB::Result::Success;
        }
        if (event.type() != 1) return USB::Result::Pending;
        // An XferComplete can race EndTransfer. Keep its TD and all data
        // mapped until that command's completion also arrives.
        if (state == TransferState::Stopping) return USB::Result::Pending;
        order.dmaReadBarrier();
        uint32_t actual = 0;
        bool shortPacket = false;
        USB::Result result = event.status() & 1 ? USB::Result::HardwareError : USB::Result::Success;
        bool setupPending = false;
        for (unsigned i = 0; i < used && !shortPacket; ++i) {
            const uint32_t control = trbs[i].control, status = trbs[i].size;
            // After a short packet DWC3 may leave the unused suffix HWO set.
            // The first descriptor still owned without a preceding short is
            // contradictory to completion; retain it until a proven halt.
            if (control & Owned) return fail(USB::Result::HardwareError);
            uint32_t remaining = status & TRBLengthMask;
            if (remaining > programmed[i]) return fail(USB::Result::HardwareError);
            actual += programmed[i] - remaining;
            unsigned trbStatus = status >> 28;
            if (trbStatus == 2 && number < 2 && (activeType == TRBType::ControlData ||
                activeType == TRBType::Status2 || activeType == TRBType::Status3)) {
                setupPending = true;
                break; // a new SETUP displaced this control request
            }
            if (trbStatus) result = USB::Result::HardwareError;
            shortPacket = remaining != 0;
        }
        if (actual > requested) result = USB::Result::Overflow;
        if (setupPending && !(event.status() & 1)) { result = USB::Result::SetupPending; actual = 0; }
        finish(result, actual > requested ? requested : actual, completion);
        return USB::Result::Success;
    }

    // Only controller-wide halt/isolation permits reclaiming a faulted TD.
    // Caller drains/discards that incarnation's event ring before reuse, so
    // stale completions cannot be associated with the next request.
    USB::Result reclaimAfterHalt(TransferCompletion &completion) {
        if (!ownsBuffers() || !controller.stopped()) return USB::Result::NotReady;
        order.dmaReadBarrier();
        finish(state == TransferState::Stopping ? abortReason : USB::Result::HardwareError, 0, completion);
        return USB::Result::Success;
    }
};
}
