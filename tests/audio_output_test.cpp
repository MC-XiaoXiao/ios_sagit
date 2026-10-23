// SPDX-License-Identifier: BSD-2-Clause
#include "AudioOutputSession.h"
#include <assert.h>
#include <stdio.h>
#include <initializer_list>
using namespace AudioOutput;
struct Backend {
    Result startResult = Result::OK, stopResult = Result::OK, readResult = Result::OK;
    unsigned starts = 0, stops = 0;
    uint64_t *startClock = nullptr;
    uint64_t *readClock = nullptr;
    Position position{};
    Result startOutput(uint64_t generation) {
        if (startClock) *startClock += 500;
        ++starts; position = {generation, 0, 101}; return startResult;
    }
    Result stopOutput() { ++stops; return stopResult; }
    Result readPosition(Position &p) {
        if (readClock) { *readClock += 10; position.hostTime = *readClock; }
        p = position; return readResult;
    }
};
static Result start(Session &s, Backend &b, uint64_t now, uint64_t timeout) {
    return s.start(b, [now]() { return now; }, timeout);
}
static Result poll(Session &s, Backend &b, uint64_t now) {
    return s.poll(b, [now]() { return now; });
}
static void formatTests() {
    assert(validFormat(OutputFormat));
    Format f = OutputFormat;
    f.rate = 48000; assert(!validFormat(f)); // integer Hz != 32.32
    f = OutputFormat; f.flags = 0x0c; assert(validFormat(f));
    f.flags |= 0x20; assert(!validFormat(f)); // planar unsupported
    f = OutputFormat; f.bits = 24; assert(!validFormat(f));
    f = OutputFormat; f.channels = 0; assert(!validFormat(f));
    f = OutputFormat; f.bytesPerPacket = UINT32_MAX; assert(!validFormat(f));
    f = OutputFormat; f.reserved = 1; assert(!validFormat(f));
    Configuration c{1, 48000, 2, 16, 4096, 256, 32, 256};
    assert(validConfiguration(c));
    for (uint32_t frames : {0u, 1023u, 4095u, 65537u, UINT32_MAX}) {
        c.ringFrames = frames; assert(!validConfiguration(c));
    }
    c.ringFrames = 4096; c.periodFrames = 257; assert(!validConfiguration(c));
    c.periodFrames = 256; c.safetyFrames = 4096; assert(!validConfiguration(c));
}
static void runningTests() {
    Backend b; Session s;
    assert(poll(s, b, 100) == Result::NotReady);
    assert(start(s, b, 100, 1000) == Result::OK);
    assert(start(s, b, 100, 1000) == Result::OK && b.starts == 1);
    assert(poll(s, b, 110) == Result::OK && s.frames() == 0);
    b.position = {s.generation(), 4096, 200};
    assert(poll(s, b, 210) == Result::OK && s.frames() == 4096);
    b.position = {s.generation(), 8192, 300};
    assert(poll(s, b, 310) == Result::OK && s.frames() == 8192);
    assert(s.stop(b) == Result::OK && s.quiescent());
    assert(s.stop(b) == Result::OK && b.stops == 1);
    assert(start(s, b, 400, 1000) == Result::OK && s.generation() == 2);
    b.position = {1, 12288, 410};
    assert(poll(s, b, 420) == Result::Stale && s.quiescent());
}
static void failureTests() {
    Backend b; Session s;
    assert(start(s, b, 0, 100) == Result::Invalid && b.starts == 0);
    b.startResult = Result::IO;
    b.stopResult = Result::Timeout;
    assert(start(s, b, 100, 1000) == Result::IO);
    assert(!s.quiescent() && b.stops == 1);
    assert(start(s, b, 100, 1000) == Result::Busy && b.starts == 1);
    assert(s.stop(b) == Result::Timeout && !s.quiescent());
    b.stopResult = Result::OK;
    assert(s.stop(b) == Result::OK && s.quiescent());
    b.startResult = Result::OK;
    assert(start(s, b, 100, 1000) == Result::OK);
    b.position = {s.generation(), 512, 200};
    assert(poll(s, b, 210) == Result::OK);
    b.position.frames = 511; b.position.hostTime = 300;
    b.stopResult = Result::Timeout;
    assert(poll(s, b, 310) == Result::Invalid && !s.quiescent());
    assert(s.frames() == 512); // invalid report never updates published position
    b.stopResult = Result::OK;
    assert(s.stop(b) == Result::OK);
}
static void clockTests() {
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        Backend b; Session s;
        assert(start(s, b, 100, 1000) == Result::OK);
        b.position = {s.generation(), 256, 200};
        assert(poll(s, b, 210) == Result::OK);
        if (scenario == 0) b.position.hostTime = 0;
        if (scenario == 1) b.position.hostTime = 199;
        if (scenario == 2) b.position.hostTime = 500; // future timestamp
        if (scenario == 3) b.position.frames = 512; // same tick with new frames
        if (scenario == 4) b.readResult = Result::IO;
        assert(poll(s, b, 310) == (scenario == 4 ? Result::IO : Result::Invalid));
        assert(s.quiescent());
    }
    Backend b; Session s;
    assert(start(s, b, 100, 1000) == Result::OK);
    b.readResult = Result::NotReady;
    assert(poll(s, b, 1099) == Result::OK && s.frames() == 0 && s.hostTime() == 0);
    assert(poll(s, b, 1100) == Result::Timeout && s.quiescent());
    b.readResult = Result::OK;
    assert(start(s, b, 1200, 1000) == Result::OK);
    b.position = {s.generation(), 0, 1300};
    assert(poll(s, b, 1300) == Result::OK);
    b.position.hostTime = 2200; // changing host clock alone is not playback
    assert(poll(s, b, 2200) == Result::Timeout);
    assert(start(s, b, 3000, 1000) == Result::OK);
    b.position = {s.generation(), 1, 2999};
    assert(poll(s, b, 3010) == Result::Invalid); // timestamp from before this start
    assert(start(s, b, 4000, 1000) == Result::OK);
    b.position = {s.generation(), 1, 4005};
    assert(poll(s, b, 4010) == Result::OK);
    b.position = {s.generation(), 2, 4006};
    assert(poll(s, b, 4009) == Result::Invalid); // clock rollback despite progress
    assert(start(s, b, 5000, 1000) == Result::OK);
    uint64_t clock = 5000;
    b.position.frames = 1; b.readClock = &clock;
    assert(s.poll(b, [&clock]() { return clock; }) == Result::OK);
    assert(s.hostTime() == 5010); // clock sampled after readPosition
    Backend delayed; Session fresh; uint64_t slowClock = 100;
    delayed.startClock = &slowClock;
    assert(fresh.start(delayed, [&slowClock]() { return slowClock; }, 1000) == Result::OK);
    delayed.readResult = Result::NotReady;
    assert(poll(fresh, delayed, 1100) == Result::OK); // timeout starts after RUN acknowledgment
    assert(poll(fresh, delayed, 1600) == Result::Timeout);
}
int main() {
    formatTests(); runningTests(); failureTests(); clockTests();
    puts("audio output: format, lifecycle, DMA-drain failures, generations and hardware timestamps passed");
}
