// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "BluetoothIBS.h"

namespace Bluetooth {
// Success and Pending retain resource ownership. A definite refusal must leave
// the resource unowned; Fault/Timeout retain it until shutdown proves otherwise.
// UART release must join callbacks and drain/abort all queued bytes; BootROM
// release powers the controller down using its baud-specific pulse. Firmware
// acquisition means verified ROM, patch/NVM and baud setup, with IBS disabled.
// All calls run on one controller workloop. An asynchronous hardware backend
// must split these stages into continuations, never wait on its own workloop.
template <class Backend> class Controller {
    Backend& backend;
    InBandSleep<Backend> ibs;
    H4Decoder decoder;
    ResetTransaction reset;
    State state = State::Detached;
    Result last = Result::Success;
    uint32_t held = 0;
    uint64_t generation = 0, receiveTime = 0;
    unsigned nextResource = 0;
    bool detaching = false;
    static constexpr Resource order[ResourceCount] = { Rails, ReferenceClock, Pins, UART, BootROM, Firmware };
    Result fault(Result result)
    {
        state = State::Fault;
        return last = result;
    }
    Result unwind()
    {
        for (unsigned i = ResourceCount; i; --i) {
            const Resource resource = order[i - 1];
            if (!(held & resource))
                continue;
            Result result = backend.release(resource, generation);
            if (result == Result::Pending) {
                state = State::Stopping;
                return last = result;
            }
            if (result != Result::Success)
                return fault(result);
            if (resource == UART) {
                result = ibs.quiesce(true);
                if (result != Result::Success)
                    return fault(result);
            }
            held &= ~uint32_t(resource);
        }
        decoder.reset();
        reset.reset(0);
        state = detaching ? State::Detached : State::Off;
        return Result::Success;
    }
    // H4Decoder callback interface: only the decoder may call these helpers.
    struct Sink {
        Controller& owner;
        Result signal(uint8_t value)
        {
            return owner.ibs.signal(owner.generation, value);
        }
        Result packet(const uint8_t* p, size_t bytes)
        {
            return owner.packet(p, bytes);
        }
    };
    Result packet(const uint8_t* p, size_t bytes)
    {
        if (p[0] == Event && p[1] == 0x10)
            return Result::Fault; // HCI Hardware Error
        if (state == State::Starting)
            return backend.bootstrapPacket(p, bytes, generation, receiveTime);
        if (state == State::WaitingForReset) {
            if (p[0] != Event)
                return Result::Success;
            const Result result = reset.receive(generation, receiveTime, p, bytes);
            if (result == Result::Success) {
                if (ibs.start(generation) != Result::Success)
                    return Result::Fault;
                state = State::Ready;
                last = Result::Success;
                decoder.enableIBS();
            } else if (result != Result::Ignored && result != Result::Pending)
                return result;
            return Result::Success;
        }
        // The backend must copy before this callback returns. Native system
        // registration and HCI credits/data queues are a separate adapter.
        return backend.packet(p, bytes, generation);
    }

public:
    explicit Controller(Backend& operations)
        : backend(operations)
        , ibs(operations)
    {
    }
    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;
    State currentState() const
    {
        return state;
    }
    Result lastResult() const
    {
        return last;
    }
    uint32_t resources() const
    {
        return held;
    }
    uint64_t epoch() const
    {
        return generation;
    }
    void resetBootstrapFraming()
    {
        if (state == State::Starting)
            decoder.reset();
    }
    void transportFault(Result result)
    {
        fault(result);
    }
    void attach()
    {
        if (state == State::Detached)
            state = State::Off;
    }
    Result start(uint64_t now)
    {
        if (state == State::Ready)
            return Result::Success;
        if (state != State::Off || held)
            return Result::Busy;
        if (generation == UINT64_MAX)
            return fault(Result::Fault);
        uint64_t deadline;
        if (!deadlineAfter(now, 2000, deadline))
            return last = Result::Invalid;
        ++generation;
        state = State::Starting;
        nextResource = 0;
        detaching = false;
        decoder.reset();
        reset.reset(generation);
        return advanceStart(now);
    }
    Result advanceStart(uint64_t now)
    {
        if (state != State::Starting)
            return Result::NotReady;
        while (nextResource < ResourceCount) {
            const Resource resource = order[nextResource];
            const Result result = backend.acquire(resource, generation);
            if (result == Result::Pending) {
                held |= resource;
                return last = result;
            }
            if (result != Result::Success) {
                // Ambiguous hardware completion is owned until the backend
                // proves shutdown, even if acquisition never returned success.
                if (result == Result::Fault || result == Result::Timeout)
                    held |= resource;
                const Result cleanup = unwind();
                return last = cleanup == Result::Success || cleanup == Result::Pending ? result : cleanup;
            }
            held |= resource;
            ++nextResource;
        }
        Result result = reset.begin(generation, now, 2000);
        if (result == Result::Success)
            result = backend.write(ResetCommand, sizeof(ResetCommand), generation);
        if (result != Result::Success) {
            const Result cleanup = unwind();
            return last = cleanup == Result::Success || cleanup == Result::Pending ? result : cleanup;
        }
        state = State::WaitingForReset;
        return last = Result::Pending;
    }
    Result receive(uint64_t epoch, uint64_t now, const uint8_t* data, size_t bytes)
    {
        if (!epoch || epoch != generation
            || (state != State::WaitingForReset && state != State::Ready
                && !(state == State::Starting && (held & Firmware))))
            return Result::Ignored;
        receiveTime = now;
        Sink sink { *this };
        const Result result = decoder.feed(data, bytes, sink);
        return result == Result::Success ? result : fault(result);
    }
    Result tick(uint64_t epoch, uint64_t now)
    {
        if (!epoch || epoch != generation)
            return Result::Ignored;
        if (state == State::Starting)
            return advanceStart(now);
        if (state == State::Stopping)
            return last = unwind();
        Result result = Result::Ignored;
        if (state == State::WaitingForReset)
            result = reset.poll(epoch, now);
        else if (state == State::Ready)
            result = ibs.tick(epoch, now);
        if (result != Result::Success && result != Result::Ignored && result != Result::Pending)
            return fault(result);
        return result;
    }
    Result stop()
    {
        if (state == State::Detached)
            return Result::NotReady;
        state = State::Stopping;
        return last = unwind();
    }
    Result detach()
    {
        if (state == State::Detached)
            return Result::Success;
        detaching = true;
        const Result result = stop();
        if (result == Result::Success)
            state = State::Detached;
        return result;
    }
    Result transmit(uint64_t epoch, uint64_t now, const uint8_t* p, size_t bytes)
    {
        if (state != State::Ready || epoch != generation)
            return Result::NotReady;
        if (!validPacket(p, bytes, true))
            return Result::Invalid;
        const auto awake = ibs.requestTransmit(epoch, now);
        if (awake != Result::Success)
            return awake == Result::Pending ? awake : fault(awake);
        const auto result = backend.write(p, bytes, epoch);
        return result == Result::Success || result == Result::Busy ? result : fault(result);
    }
    Result transmitDrained(uint64_t now)
    {
        return state == State::Ready ? ibs.transmitDrained(generation, now) : Result::NotReady;
    }
};
}
