// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <BluetoothProtocol.h>

namespace Bluetooth {
// WCN3990 pulse sequencing; call advance() from an IOTimerEventSource/UART
// completion. No sleeps or busy waits. The owner retains rails/clock/pins/UART
// until quiesced, including on failure. Pulses bypass H4 and IBS decoders.
// configure(baud, flowControl), flush(), pulse(byte), drained(), reopen(baud)
// are checked backend operations. drained() may return Pending; pulse success
// only admits the byte, it never proves the byte left the UART.
template<class Backend> class BootSequence {
public:
    enum class Phase : uint8_t { Idle, DrainOff, WaitOff, DrainOn, WaitOn, Complete, Fault };
private:
    Backend &backend;
    Phase phase = Phase::Idle;
    uint64_t deadline = 0;
    Result fail(Result result) { phase = Phase::Fault; return result; }
    Result send(bool on, uint64_t now) {
        if (!deadlineAfter(now,1000,deadline)) return fail(Result::Invalid);
        Result result = backend.configure(on ? InitialBaud : 2400,false);
        if (result == Result::Success) result = backend.flush();
        if (result == Result::Success) result = backend.pulse(on ? 0xfc : 0xc0);
        if (result != Result::Success) return fail(result);
        phase = on ? Phase::DrainOn : Phase::DrainOff;
        return Result::Pending;
    }
public:
    explicit BootSequence(Backend &operations) : backend(operations) {}
    Phase currentPhase() const { return phase; }
    void resetAfterQuiesce() {phase=Phase::Idle;deadline=0;}
    Result begin(uint64_t now) { return phase == Phase::Idle ? send(false,now) : Result::Busy; }
    Result advance(uint64_t now) {
        if (phase == Phase::Idle) return Result::NotReady;
        if (phase == Phase::Fault) return Result::Fault;
        if (phase == Phase::Complete) return Result::Success;
        if (phase == Phase::DrainOff || phase == Phase::DrainOn) {
            if (now >= deadline) return fail(Result::Timeout);
            const Result result = backend.drained();
            if (result == Result::Pending) return result;
            if (result != Result::Success) return fail(result);
            const bool on = phase == Phase::DrainOn;
            if (!deadlineAfter(now,on ? 100 : 10,deadline)) return fail(Result::Invalid);
            phase = on ? Phase::WaitOn : Phase::WaitOff;
            return Result::Pending;
        }
        if (now < deadline) return Result::Pending;
        if (phase == Phase::WaitOff) return send(true,now);
        const Result result = backend.reopen(InitialBaud);
        if (result != Result::Success) return fail(result);
        phase = Phase::Complete; return Result::Success;
    }
    // Called only after the owning UART backend has stopped and joined its
    // callbacks. A new owner creates a fresh sequence for the next generation.
};
}
