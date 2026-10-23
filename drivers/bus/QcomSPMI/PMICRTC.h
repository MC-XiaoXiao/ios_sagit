// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <QcomPowerHardware.h>
namespace QcomPower {
// PM8998 uses the PM8941 RTC layout (Linux rtc-pm8xxx.c / pm8998.dtsi).
// Only observer reads are issued; never enable, stop, set or rearm the PMIC.
template <class Bus>
Status readPMICRTC(Bus &bus, uint32_t sid, uint32_t base, uint32_t &seconds)
{
    if (sid > 15 || base != 0x6000) return Invalid;
    uint8_t control = 0;
    Status s = bus.transfer(sid, base+0x46, &control, 1, false);
    if (s != OK) return s;
    if (!(control & 0x80)) return NotReady;
    // Re-read the low byte to detect a seconds rollover during the bulk read.
    for (unsigned retry = 0; retry < 3; ++retry) {
        uint8_t data[4] = {}, low = 0;
        s = bus.transfer(sid, base+0x48, data, 4, false);
        if (s != OK) return s;
        s = bus.transfer(sid, base+0x48, &low, 1, false);
        if (s != OK) return s;
        if (low < data[0]) continue;
        seconds = le32(data);
        return OK;
    }
    return Busy;
}

// The PMIC supplies the boot seconds. Continue with XNU uptime so calendar
// reads never issue blocking SPMI transactions or recurse into calendar time.
class RTCCalendar {
    uint64_t epoch = 0, anchor = 0;
    uint32_t fraction = 0;
public:
    bool set(uint64_t seconds, uint32_t nsecs, uint64_t uptime) {
        if (nsecs >= 1000000000U) return false;
        epoch = seconds; fraction = nsecs; anchor = uptime; return true;
    }
    bool get(uint64_t uptime, uint64_t &seconds, uint32_t &nsecs) const {
        if (uptime < anchor) return false;
        uint64_t delta = uptime-anchor, extra = delta/1000000000ULL;
        uint64_t ns = delta%1000000000ULL + fraction;
        extra += ns/1000000000ULL;
        if (extra > UINT64_MAX-epoch) return false;
        seconds = epoch+extra; nsecs = ns%1000000000ULL; return true;
    }
};
}
