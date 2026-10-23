// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "BluetoothH4.h"

namespace Bluetooth {
// Backend::vote(bool, epoch) owns a UART clock reference; control(byte, epoch)
// queues one byte on the SAME ordered UART writer as H4. Failure can mean a
// byte reached the wire, so clock ownership is retained until real quiescence.
template<class Backend> class InBandSleep {
public:
    enum class TX : uint8_t { Asleep, Waking, Awake };
    static constexpr uint64_t WakeRetryMS = 100, IdleMS = 2000;
    static constexpr unsigned MaxWakeAttempts = 3;
private:
    Backend &backend;
    uint64_t epoch = 0, wakeDeadline = 0, idleDeadline = 0;
    TX tx = TX::Asleep;
    bool enabled = false, rxAwake = false, clockHeld = false, failed = false;
    unsigned attempts = 0;
    Result fail(Result result) { failed = true; return result; }
    Result clockOn() {
        if (clockHeld) return Result::Success;
        const Result result = backend.vote(true,epoch);
        if (result == Result::Success) clockHeld = true;
        return result;
    }
    Result clockOffIfIdle() {
        if (!clockHeld || rxAwake || tx != TX::Asleep) return Result::Success;
        const Result result = backend.vote(false,epoch);
        if (result == Result::Success) clockHeld = false;
        else return fail(result);
        return Result::Success;
    }
public:
    explicit InBandSleep(Backend &operations) : backend(operations) {}
    TX transmitState() const { return tx; }
    bool receiveAwake() const { return rxAwake; }
    bool ownsClock() const { return clockHeld; }
    bool faulted() const { return failed; }
    Result start(uint64_t generation) {
        if (!generation) return Result::Invalid;
        if (enabled || clockHeld) return Result::Busy;
        epoch = generation; enabled = true; failed = rxAwake = false;
        tx = TX::Asleep; attempts = 0; idleDeadline = wakeDeadline = 0;
        return Result::Success;
    }
    Result requestTransmit(uint64_t generation, uint64_t now) {
        if (!enabled || generation != epoch) return Result::NotReady;
        if (failed) return Result::Fault;
        idleDeadline = 0;
        if (tx == TX::Awake) return Result::Success;
        if (tx == TX::Waking) return Result::Pending;
        uint64_t deadline;
        if (!deadlineAfter(now,WakeRetryMS,deadline)) return Result::Invalid;
        Result result = clockOn();
        if (result != Result::Success) return fail(result);
        result = backend.control(WakeIndication,epoch);
        if (result != Result::Success) return fail(result);
        tx = TX::Waking; wakeDeadline = deadline; attempts = 1;
        return Result::Pending;
    }
    Result signal(uint64_t generation, uint8_t byte) {
        if (!enabled || generation != epoch) return Result::NotReady;
        if (failed) return Result::Fault;
        if (byte == WakeIndication) {
            Result result = clockOn();
            if (result != Result::Success) return fail(result);
            rxAwake = true;
            result = backend.control(WakeAcknowledgement,epoch);
            return result == Result::Success ? result : fail(result);
        }
        if (byte == WakeAcknowledgement) {
            if (tx == TX::Waking) { tx = TX::Awake; wakeDeadline = 0; }
            return Result::Success;
        }
        if (byte == SleepIndication) { rxAwake = false; return clockOffIfIdle(); }
        return Result::Invalid;
    }
    // Called only after the UART has drained the complete H4 packet and no TX
    // remains queued. Queue admission or a software timeout is not a drain.
    Result transmitDrained(uint64_t generation, uint64_t now) {
        if (!enabled || generation != epoch || tx != TX::Awake) return Result::NotReady;
        if (failed) return Result::Fault;
        return deadlineAfter(now,IdleMS,idleDeadline) ? Result::Success : Result::Invalid;
    }
    Result tick(uint64_t generation, uint64_t now) {
        if (!enabled || generation != epoch) return Result::Ignored;
        if (failed) return Result::Fault;
        if (tx == TX::Waking && now >= wakeDeadline) {
            if (attempts == MaxWakeAttempts) return fail(Result::Timeout);
            uint64_t deadline;
            if (!deadlineAfter(now,WakeRetryMS,deadline)) return fail(Result::Invalid);
            const Result result = backend.control(WakeIndication,epoch);
            if (result != Result::Success) return fail(result);
            ++attempts; wakeDeadline = deadline;
        }
        if (tx == TX::Awake && idleDeadline && now >= idleDeadline) {
            const Result result = backend.control(SleepIndication,epoch);
            if (result != Result::Success) return fail(result);
            tx = TX::Asleep; idleDeadline = 0;
            return clockOffIfIdle();
        }
        return Result::Success;
    }
    Result quiesce(bool uartStopped) {
        if (!uartStopped) return Result::Busy;
        if (clockHeld) {
            const Result result = backend.vote(false,epoch);
            if (result != Result::Success) return fail(result);
        }
        enabled = clockHeld = failed = rxAwake = false;
        tx = TX::Asleep; epoch = wakeDeadline = idleDeadline = 0;
        return Result::Success;
    }
};
}
