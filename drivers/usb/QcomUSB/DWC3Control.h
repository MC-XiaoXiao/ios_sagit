// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "DWC3Transfer.h"
#include "DWC3Endpoint.h"

namespace DWC3 {
enum class ControlState : uint8_t {
    Uninitialized, Stopped, Setup, Dispatch, Data, WaitingStatus, Status, Recovering, Fault
};
enum class ControlNotificationKind : uint8_t { None, Setup, Data, Status, Aborted };
struct ControlNotification {
    ControlNotificationKind kind = ControlNotificationKind::None;
    USB::Result result = USB::Result::Success;
    uint64_t transaction = 0;
    USB::SetupPacket setup{};
    uint32_t bytes = 0;
};
struct ControlMemory {
    TRB *outTRBs, *inTRBs;
    uint64_t outDMA, inDMA;
    unsigned trbCount;
    volatile uint8_t *setupCPU;
    uint64_t setupDMA;
};

// EP0 transport only. The selected device personality supplies request policy,
// descriptors, configuration selection and function dispatch. The default
// adapter delegates to Apple IOUSBDeviceControlRequest; optional QcomUSBCDC
// has an exclusive CDC policy. Both call allowStatus only after processing.
// Notifications are returned by value; no client callback runs reentrantly
// inside an event-ring callback or while a TD still owns its DMA buffers.
template<class Controller, class Order> class Control {
    Controller &controller;
    Order &order;
    Transfer<Controller, Order> out, in;
    ControlMemory memory{};
    ControlState state = ControlState::Uninitialized;
    USB::SetupPacket setup{};
    unsigned packetSize = 0, dataEndpoint = 0, statusEndpoint = 1;
    uint32_t suppliedBytes = 0;
    uint64_t epoch = 0, transaction = 0, nextTransaction = 0, serial = 0;
    bool approved = false, statusReady = false, setupPending = false, stopping = false;
    USB::Result recoveryReason = USB::Result::Aborted;

    USB::Result fault(USB::Result result) { state = ControlState::Fault; return result; }
    Transfer<Controller, Order> &endpoint(unsigned ep) { return ep ? in : out; }
    static bool accepted(USB::Result result) {
        return result == USB::Result::Success || result == USB::Result::Pending;
    }
    void notify(ControlNotification &message, ControlNotificationKind kind,
                USB::Result result, uint32_t bytes = 0) const {
        message = {kind, result, transaction, setup, bytes};
    }
    USB::Result armSetup() {
        if (out.ownsBuffers() || in.ownsBuffers()) return fault(USB::Result::Busy);
        if (serial == UINT64_MAX) return fault(USB::Result::HardwareError);
        DMASegment segment{memory.setupDMA, 8};
        state = ControlState::Setup;
        approved = statusReady = setupPending = stopping = false;
        transaction = 0;
        auto result = out.submit(&segment, 1, 8, packetSize, TRBType::Setup, ++serial);
        return result == USB::Result::Success ? result : fault(result);
    }
    USB::Result startStatus() {
        if (state != ControlState::WaitingStatus || !approved || !statusReady || setupPending)
            return USB::Result::Pending;
        if (serial == UINT64_MAX) return fault(USB::Result::HardwareError);
        state = ControlState::Status;
        auto result = endpoint(statusEndpoint).submit(nullptr, 0, 0, packetSize,
            setup.length ? TRBType::Status3 : TRBType::Status2, ++serial);
        return result == USB::Result::Success ? result : fault(result);
    }
    USB::Result finishRecovery(ControlNotification &message) {
        if (out.ownsBuffers() || in.ownsBuffers()) return USB::Result::Pending;
        if (transaction) notify(message, ControlNotificationKind::Aborted, recoveryReason);
        if (stopping) {
            transaction = 0; state = ControlState::Stopped;
            return USB::Result::Success;
        }
        uint32_t reply = 0;
        // Control STALL is issued on physical EP0 OUT. Both TDs have already
        // stopped, so restarting SETUP cannot overwrite their live buffers.
        auto result = controller.command(0, Command::SetStall, 0, 0, 0, 0, reply);
        if (result != USB::Result::Success) return fault(result);
        return armSetup();
    }
    USB::Result recover(USB::Result reason, bool stopOnly, ControlNotification &message) {
        recoveryReason = reason; stopping = stopOnly; state = ControlState::Recovering;
        approved = statusReady = false;
        // EndTransfer is asynchronous even when its command register clears.
        // Neither endpoint is reused until both command completions arrive.
        for (unsigned ep = 0; ep < 2; ++ep) {
            auto &td = endpoint(ep);
            if (td.currentState() == TransferState::Fault) return fault(USB::Result::HardwareError);
            if (td.ownsBuffers()) {
                auto result = td.cancel(reason == USB::Result::Timeout ? reason : USB::Result::Aborted);
                if (!accepted(result)) return fault(result);
            }
        }
        return finishRecovery(message);
    }
public:
    Control(Controller &hardware, Order &barriers) :
        controller(hardware), order(barriers), out(hardware, barriers), in(hardware, barriers) {}
    ControlState currentState() const { return state; }
    uint64_t currentTransaction() const { return transaction; }
    const USB::SetupPacket &currentSetup() const { return setup; }
    bool ownsBuffers() const { return out.ownsBuffers() || in.ownsBuffers(); }

    USB::Result initialize(const ControlMemory &allocation, unsigned maximumPacket, uint64_t roleEpoch) {
        if (state != ControlState::Uninitialized) return USB::Result::Busy;
        if ((maximumPacket != 8 && maximumPacket != 16 && maximumPacket != 32 &&
             maximumPacket != 64 && maximumPacket != 512) || !allocation.setupCPU ||
            !allocation.outTRBs || !allocation.inTRBs || allocation.trbCount < 2 || allocation.trbCount > 64 ||
            !USB::dmaRange(allocation.setupDMA, 8, controller.dmaBits(), 8)) return USB::Result::Invalid;
        // DMA descriptor pools and SETUP storage must be disjoint in both
        // address spaces. An OUT packet must never overwrite live descriptors.
        uint64_t addresses[] = {allocation.outDMA, allocation.inDMA, allocation.setupDMA};
        uintptr_t cpu[] = {reinterpret_cast<uintptr_t>(allocation.outTRBs),
            reinterpret_cast<uintptr_t>(allocation.inTRBs), reinterpret_cast<uintptr_t>(allocation.setupCPU)};
        uint64_t sizes[] = {allocation.trbCount * sizeof(TRB), allocation.trbCount * sizeof(TRB), 8};
        for (unsigned i = 0; i < 3; ++i) {
            if (!USB::dmaRange(addresses[i], sizes[i], controller.dmaBits()) || cpu[i] > UINTPTR_MAX - sizes[i])
                return USB::Result::Invalid;
            for (unsigned j = 0; j < i; ++j)
                if ((addresses[i] < addresses[j] + sizes[j] && addresses[j] < addresses[i] + sizes[i]) ||
                    (cpu[i] < cpu[j] + sizes[j] && cpu[j] < cpu[i] + sizes[i])) return USB::Result::Invalid;
        }
        // Validate both pools before initializing either one, so a rejected
        // allocation leaves this object retryable rather than half initialized.
        if ((cpu[0] & 15) || (cpu[1] & 15) || (allocation.outDMA & 15) || (allocation.inDMA & 15) ||
            controller.endpointCount() < 2) return USB::Result::Invalid;
        auto result = out.initialize(0, allocation.outTRBs, allocation.outDMA, allocation.trbCount, roleEpoch);
        if (result != USB::Result::Success) return fault(result);
        result = in.initialize(1, allocation.inTRBs, allocation.inDMA, allocation.trbCount, roleEpoch);
        if (result != USB::Result::Success) return fault(result);
        memory = allocation; packetSize = maximumPacket; epoch = roleEpoch;
        state = ControlState::Stopped;
        return USB::Result::Success;
    }
    USB::Result start() {
        if (state != ControlState::Stopped) return USB::Result::NotReady;
        return armSetup();
    }
    USB::Result negotiate(USB::Speed speed, unsigned fullSpeedPacketSize = 64) {
        // A bus reset may be followed by CONNECT_DONE and a fresh SETUP in
        // one snapshot. Dispatch owns no DATA/STATUS descriptors, so applying
        // the negotiated EP0 configuration here preserves that SETUP token.
        if (state != ControlState::Setup && state != ControlState::Stopped &&
            (state != ControlState::Dispatch || ownsBuffers())) return USB::Result::NotReady;
        unsigned maximum = speed == USB::Speed::Super ? 512 :
            speed == USB::Speed::High ? 64 : fullSpeedPacketSize;
        EndpointConfiguration config{0, EndpointType::Control, speed, maximum, 0, 0};
        if (!config.valid()) return USB::Result::Invalid;
        // The real Core permits Modify while an explicitly typed SETUP is
        // active, and refuses it for DATA/STATUS or a stopping endpoint.
        auto result = controller.configureEndpoint(config, true);
        if (result != USB::Result::Success) return fault(result);
        config.physicalEndpoint = 1;
        result = controller.configureEndpoint(config, true);
        if (result != USB::Result::Success) return fault(result);
        packetSize = maximum;
        return USB::Result::Success;
    }
    USB::Result supplyData(uint64_t token, const DMASegment *segments, unsigned count, uint32_t bytes) {
        if (!token || token != transaction) return USB::Result::Invalid;
        if (state != ControlState::Dispatch || !setup.length || setupPending) return USB::Result::NotReady;
        if (bytes > setup.length || (!setup.in() && bytes != setup.length)) return USB::Result::Invalid;
        if (serial == UINT64_MAX) return fault(USB::Result::HardwareError);
        const bool zero = setup.in() && bytes && bytes < setup.length && bytes % packetSize == 0;
        auto &td = endpoint(dataEndpoint);
        auto result = td.submit(segments, count, bytes, packetSize, TRBType::ControlData, ++serial, zero);
        if (result == USB::Result::Success) { state = ControlState::Data; suppliedBytes = bytes; }
        else if (td.ownsBuffers()) return fault(result);
        return result;
    }
    USB::Result allowStatus(uint64_t token) {
        if (!token || token != transaction) return USB::Result::Invalid;
        if (setupPending || (state != ControlState::Dispatch && state != ControlState::Data &&
                            state != ControlState::WaitingStatus)) return USB::Result::NotReady;
        approved = true;
        if (!setup.length && state == ControlState::Dispatch) state = ControlState::WaitingStatus;
        return startStatus();
    }
    USB::Result reject(uint64_t token, ControlNotification &message) {
        message = {};
        if (!token || token != transaction) return USB::Result::Invalid;
        if (state == ControlState::Fault || state == ControlState::Recovering || state == ControlState::Stopped)
            return USB::Result::NotReady;
        return recover(USB::Result::Aborted, false, message);
    }
    USB::Result stop(ControlNotification &message, USB::Result reason = USB::Result::Aborted) {
        message = {};
        if (state == ControlState::Uninitialized || state == ControlState::Fault) return USB::Result::NotReady;
        if (reason != USB::Result::Aborted && reason != USB::Result::Timeout) return USB::Result::Invalid;
        if (state == ControlState::Stopped) return USB::Result::Success;
        if (state == ControlState::Recovering) { stopping = true; return USB::Result::Pending; }
        return recover(reason, true, message);
    }
    USB::Result handle(Event event, uint64_t roleEpoch, ControlNotification &message) {
        message = {};
        if (roleEpoch != epoch || event.device() || event.endpointNumber() > 1) return USB::Result::Invalid;
        if (state == ControlState::Uninitialized || state == ControlState::Stopped || state == ControlState::Fault)
            return USB::Result::NotReady;
        unsigned ep = event.endpointNumber();
        if (state == ControlState::Recovering) {
            TransferCompletion completion{};
            if (endpoint(ep).ownsBuffers()) {
                auto result = endpoint(ep).handle(event, epoch, completion);
                if (!accepted(result)) return fault(result);
            }
            return finishRecovery(message);
        }
        if (event.type() == 3) {
            if ((event.status() & 3) == 1) { // XferNotReady(DATA)
                if (!setup.length || ep != dataEndpoint)
                    return recover(USB::Result::HardwareError, false, message);
                return USB::Result::Pending; // DATA is submitted as soon as Family supplies it
            }
            if ((event.status() & 3) == 2) { // XferNotReady(STATUS)
                if (state != ControlState::WaitingStatus && !(state == ControlState::Dispatch && !setup.length))
                    return USB::Result::Pending;
                if (ep != statusEndpoint) return recover(USB::Result::HardwareError, false, message);
                if (setupPending) return recover(USB::Result::SetupPending, false, message);
                statusReady = true;
                return startStatus(); // latched even if Family has not approved yet
            }
            return USB::Result::Pending;
        }
        if (event.type() != 1 && event.type() != 7) return USB::Result::Pending;
        unsigned expected = state == ControlState::Setup ? 0 :
            state == ControlState::Data ? dataEndpoint : statusEndpoint;
        if ((state != ControlState::Setup && state != ControlState::Data && state != ControlState::Status) || ep != expected)
            return USB::Result::Invalid;
        TransferCompletion completion{};
        auto result = endpoint(ep).handle(event, epoch, completion);
        if (result == USB::Result::Pending) return result;
        if (result != USB::Result::Success) return fault(result);
        if (state == ControlState::Setup) {
            if (completion.result != USB::Result::Success || completion.bytes != 8)
                return fault(USB::Result::HardwareError);
            if (nextTransaction == UINT64_MAX) return fault(USB::Result::HardwareError);
            uint8_t bytes[8];
            order.dmaReadBarrier();
            for (unsigned i = 0; i < 8; ++i) bytes[i] = memory.setupCPU[i];
            setup = USB::SetupPacket::decode(bytes); transaction = ++nextTransaction;
            dataEndpoint = setup.in() ? 1 : 0;
            statusEndpoint = setup.length ? dataEndpoint ^ 1 : 1;
            state = ControlState::Dispatch;
            notify(message, ControlNotificationKind::Setup, USB::Result::Success);
            return USB::Result::Success;
        }
        if (state == ControlState::Data) {
            state = ControlState::WaitingStatus;
            setupPending = completion.result == USB::Result::SetupPending;
            if (setupPending) {
                notify(message, ControlNotificationKind::Aborted, USB::Result::SetupPending);
                transaction = 0; approved = false; // asynchronous Family replies are now stale
            } else if (setup.in() && completion.result == USB::Result::Success && completion.bytes != suppliedBytes) {
                return recover(USB::Result::HardwareError, false, message);
            } else if (completion.result != USB::Result::Success) {
                return recover(completion.result, false, message);
            } else notify(message, ControlNotificationKind::Data, completion.result, completion.bytes);
            return USB::Result::Success;
        }
        // Status completed; only now report success to Family and arm the
        // next SETUP. The same storage cannot be reused before this event.
        if (completion.result == USB::Result::Success)
            notify(message, ControlNotificationKind::Status, USB::Result::Success);
        else notify(message, ControlNotificationKind::Aborted, completion.result);
        if (completion.result != USB::Result::Success && completion.result != USB::Result::SetupPending)
            return recover(completion.result, false, message);
        return armSetup();
    }
    USB::Result resetAfterHalt(ControlNotification &message) {
        message = {};
        if (state == ControlState::Uninitialized || !controller.stopped()) return USB::Result::NotReady;
        for (unsigned ep = 0; ep < 2; ++ep) {
            if (endpoint(ep).ownsBuffers()) {
                TransferCompletion completion{};
                auto result = endpoint(ep).reclaimAfterHalt(completion);
                if (result != USB::Result::Success) return fault(result);
            }
        }
        if (transaction) notify(message, ControlNotificationKind::Aborted, USB::Result::Aborted);
        transaction = 0; approved = statusReady = setupPending = stopping = false;
        state = ControlState::Stopped;
        // Adapter must discard old ring events and reconfigure EP0 before
        // calling start(), e.g. after USB reset or controller-wide recovery.
        return USB::Result::Success;
    }
};
}
