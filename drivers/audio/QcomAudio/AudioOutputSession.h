// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <AudioOutputTypes.h>

namespace AudioOutput {
enum class Result { OK, Invalid, NotReady, Busy, IO, Stale, Timeout, Retained };
enum class State { Idle, Running, Faulted };

// The kernel adapter serializes these operations on a work loop. The backend
// must acknowledge quiescence; a failed stop never permits buffer reclamation.
class Session {
    State state_ = State::Idle;
    uint64_t generation_ = 0, frames_ = 0, host_ = 0, progress_ = 0;
    uint64_t timeout_ = 0, started_ = 0;
public:
    State state() const { return state_; }
    uint64_t generation() const { return generation_; }
    uint64_t frames() const { return frames_; }
    uint64_t hostTime() const { return host_; }
    bool quiescent() const { return state_ == State::Idle; }

    template<class Backend, class Clock>
    Result start(Backend &backend, Clock clock, uint64_t timeout) {
        if (state_ == State::Running) return Result::OK;
        if (state_ != State::Idle) return Result::Busy;
        const uint64_t before = clock();
        if (!before || !timeout || generation_ == UINT64_MAX) return Result::Invalid;
        ++generation_;
        state_ = State::Faulted; // start may fail after enabling DMA
        Result result = backend.startOutput(generation_);
        if (result != Result::OK) {
            if (backend.stopOutput() == Result::OK) state_ = State::Idle;
            return result;
        }
        const uint64_t now = clock();
        if (now < before || !now) {
            if (backend.stopOutput() == Result::OK) state_ = State::Idle;
            return Result::Invalid;
        }
        frames_ = host_ = 0;
        progress_ = started_ = now;
        timeout_ = timeout;
        state_ = State::Running;
        return Result::OK;
    }
    template<class Backend>
    Result stop(Backend &backend) {
        if (state_ == State::Idle) return Result::OK;
        state_ = State::Faulted;
        Result result = backend.stopOutput();
        if (result == Result::OK) state_ = State::Idle;
        return result;
    }
    template<class Backend, class Clock>
    Result poll(Backend &backend, Clock clock) {
        if (state_ != State::Running) return Result::NotReady;
        Position p{};
        Result result = backend.readPosition(p);
        // Take the upper timestamp bound AFTER the backend reads hardware.
        // Reading the clock before readPosition would reject fresh samples.
        const uint64_t now = clock();
        if (now < progress_) result = Result::Invalid;
        if (result == Result::OK) {
            if (p.generation != generation_) result = Result::Stale;
            else if (!p.hostTime || p.hostTime < started_ || p.hostTime > now || p.hostTime < host_ ||
                     p.frames < frames_ || (p.frames > frames_ && host_ && p.hostTime == host_))
                result = Result::Invalid;
            else {
                // Re-reading a timestamp without hardware progress must not
                // keep a dead DSP/stream alive indefinitely.
                if (p.frames > frames_) progress_ = now;
                frames_ = p.frames;
                host_ = p.hostTime;
            }
        }
        if (result == Result::NotReady) result = Result::OK;
        if (result == Result::OK && (now < progress_ || now - progress_ >= timeout_))
            result = Result::Timeout;
        if (result != Result::OK) {
            state_ = State::Faulted;
            // Keep the original error, but retain the unknown DMA ownership
            // if the synchronous drain itself fails.
            if (backend.stopOutput() == Result::OK) state_ = State::Idle;
        }
        return result;
    }
};
}
