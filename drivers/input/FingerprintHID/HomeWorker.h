// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdint.h>

namespace QcomHomeProtocol {
// All methods run under the HID gate. Secure calls execute after dropping it.
// A power transition invalidates in-flight input without racing the TA buffer.
class WorkerQueue {
public:
    enum class Kind { None, Configure, Sample };
    struct Work { Kind kind=Kind::None; uint64_t generation=0; bool enabled=false; };
private:
    uint64_t generation=0;
    bool enabled=false, stopping=false, configure=false, sample=false;
public:
    bool isEnabled() const { return enabled && !stopping; }
    bool samplePending() const { return isEnabled() && sample; }
    bool setEnabled(bool value) {
        if (stopping) return false;
        ++generation; enabled=value; configure=true; sample=false; return true;
    }
    bool queueSample() {
        if (!isEnabled()) return false;
        sample=true; return true;
    }
    Work next() {
        Work work;
        if (stopping) return work;
        work.generation=generation;work.enabled=enabled;
        if (configure) { configure=false;work.kind=Kind::Configure; }
        else if (enabled && sample) { sample=false;work.kind=Kind::Sample; }
        return work;
    }
    bool current(const Work &work) const { return !stopping && work.generation==generation; }
    void cancel() { ++generation;enabled=configure=sample=false; }
    void stop() { ++generation;stopping=true;enabled=configure=sample=false; }
};
}
