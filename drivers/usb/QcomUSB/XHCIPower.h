// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "USBProtocol.h"

namespace XHCI {
constexpr uint64_t SuspendMinimumUS=10000;
enum class DeviceActivity : uint8_t { Running,Quiescing,Suspended,Resuming };

// Device-wide admission/deadline fence. Endpoint queues independently prove
// each Stop(SP); this latch does not claim that any DMA has stopped.
class DevicePause {
    DeviceActivity phase=DeviceActivity::Running;
    uint64_t started=0,resumeAfter=0;
public:
    bool running() const { return phase==DeviceActivity::Running; }
    bool suspended() const { return phase==DeviceActivity::Suspended; }
    bool transitioning() const { return phase==DeviceActivity::Quiescing || phase==DeviceActivity::Resuming; }
    USB::Result beginSuspend(uint64_t now) {
        if (!running()) return USB::Result::Busy;
        if (now>UINT64_MAX-SuspendMinimumUS) return USB::Result::Invalid;
        started=now;resumeAfter=now+SuspendMinimumUS;phase=DeviceActivity::Quiescing;return USB::Result::Success;
    }
    USB::Result stopped(uint64_t now) {
        if (phase!=DeviceActivity::Quiescing || now<resumeAfter-SuspendMinimumUS || now>UINT64_MAX-SuspendMinimumUS)
            return USB::Result::Invalid;
        if (now+SuspendMinimumUS>resumeAfter) resumeAfter=now+SuspendMinimumUS;
        return USB::Result::Success;
    }
    bool finishSuspend() {
        if (phase!=DeviceActivity::Quiescing) return false;
        phase=DeviceActivity::Suspended;return true;
    }
    bool beginResume() {
        if (phase!=DeviceActivity::Quiescing && phase!=DeviceActivity::Suspended) return false;
        phase=DeviceActivity::Resuming;return true;
    }
    uint64_t remaining(uint64_t now) const { return now<resumeAfter ? resumeAfter-now : 0; }
    bool elapsed(uint64_t now,uint64_t &duration) const {
        if (running() || now<started) return false;
        duration=now-started;return true;
    }
    bool finishResume(uint64_t now) {
        if (phase!=DeviceActivity::Resuming || remaining(now)) return false;
        phase=DeviceActivity::Running;started=resumeAfter=0;return true;
    }
};
}
