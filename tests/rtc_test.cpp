// SPDX-License-Identifier: GPL-2.0-only
#include <PMICRTC.h>
#include <cassert>
#include <cstdio>
#include <vector>
using namespace QcomPower;
struct Bus {
    bool enabled = true;
    Status error = OK;
    unsigned calls = 0, reads = 0;
    std::vector<uint32_t> full{0x123456ff, 0x12345700};
    std::vector<uint8_t> low{0, 0};
    Status transfer(uint8_t sid, uint16_t address, uint8_t *data, unsigned count, bool write) {
        assert(sid == 0 && !write);
        ++calls;
        if (error != OK) return error;
        if (address == 0x6046) { assert(count == 1); *data = enabled ? 0x80 : 0; }
        else {
            assert(address == 0x6048);
            if (count == 4) { assert(reads < full.size()); put32(data, full[reads]); }
            else { assert(count == 1 && reads < low.size()); *data = low[reads++]; }
        }
        return OK;
    }
};
int main() {
    uint32_t seconds = 42;
    Bus rollover;
    assert(readPMICRTC(rollover, 0, 0x6000, seconds) == OK && seconds == 0x12345700);
    assert(rollover.calls == 5);
    Bus disabled; disabled.enabled = false;
    assert(readPMICRTC(disabled, 0, 0x6000, seconds) == NotReady && disabled.calls == 1);
    for (Status error : {Denied, TimedOut, Stale, Unsupported}) {
        Bus bad; bad.error = error; seconds = 42;
        assert(readPMICRTC(bad, 0, 0x6000, seconds) == error && seconds == 42);
    }
    Bus invalid;
    assert(readPMICRTC(invalid, 16, 0x6000, seconds) == Invalid && invalid.calls == 0);
    assert(readPMICRTC(invalid, 0, 0x6100, seconds) == Invalid && invalid.calls == 0);
    Bus unstable; unstable.full = {255, 255, 255}; unstable.low = {0, 0, 0};
    assert(readPMICRTC(unstable, 0, 0x6000, seconds) == Busy);
    RTCCalendar calendar;
    assert(calendar.set(123, 900000000, 1000));
    uint64_t utc = 0; uint32_t ns = 0;
    assert(calendar.get(200001000, utc, ns) && utc == 124 && ns == 100000000);
    assert(calendar.get(3000001000, utc, ns) && utc == 126 && ns == 900000000);
    assert(!calendar.get(999, utc, ns));
    assert(!calendar.set(100, 1000000000, 1000));
    assert(calendar.get(1000, utc, ns) && utc == 123 && ns == 900000000);
    assert(calendar.set(UINT64_MAX, 0, 0));
    assert(!calendar.get(1000000000, utc, ns));
    assert(calendar.set(100, 123, 1000));
    assert(calendar.get(1001, utc, ns) && utc == 100 && ns == 124);
    puts("RTC: read-only PMIC access, disabled/error rejection, rollover retries and volatile calendar corrections passed");
}
