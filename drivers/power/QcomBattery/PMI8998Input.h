// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "PMI8998Battery.h"

// Read-only SMB2 input diagnostics. The USB connector retains register ownership.
// Units and SDP decoding follow Xiaomi qpnp-smb2.c and smb-lib.c.
namespace PMICBattery {
struct InputStatus {
    uint32_t settledMA = 0, highCurrentMA = 0, sdpMA = 0;
    uint8_t apsdResult = 0, typeCSource = 0;
    bool apsdComplete = false, sdp = false, highCurrent = false;
    bool highCurrentValid = false, suspended = false;
    bool aiclComplete = false, aiclFailed = false, softLimited = false;
};
template<class Bus> Result readInputStatus(Bus &bus, const Config &c, InputStatus &output) {
    if (!c.valid()) return Result::InvalidConfig;
    uint8_t partner[5]{}, partnerAfter[5]{}, options[2]{}, optionsAfter[2]{};
    uint8_t input[4]{}, limit = 0, limitAfter = 0, suspend = 0, suspendAfter = 0;
    auto read = [&](uint16_t address, uint8_t *p, unsigned n) {
        return bus.read(uint8_t(c.sid), address, p, n);
    };
    if (!read(0x1307, partner, 5) || !read(0x1365, options, 2) ||
        !read(0x1370, &limit, 1) || !read(0x1340, &suspend, 1) ||
        !read(c.misc + 7, input, 4) || !read(0x1340, &suspendAfter, 1) ||
        !read(0x1370, &limitAfter, 1) || !read(0x1365, optionsAfter, 2) ||
        !read(0x1307, partnerAfter, 5)) return Result::IOFailure;
    // A route/configuration transition must not produce a mixed budget. QC
    // pulse counters and voltage transitions are not configuration identity.
    if (partner[0] != partnerAfter[0] || partner[1] != partnerAfter[1] ||
        partner[4] != partnerAfter[4] || !equal(options, optionsAfter, 2) ||
        limit != limitAfter || suspend != suspendAfter) return Result::Incoherent;
    if (input[0] > 192) return Result::OutOfRange; // ICL status: 0..4800 mA, 25 mA steps
    InputStatus s;
    s.settledMA = uint32_t(input[0]) * 25;
    s.highCurrentValid = limit <= 120; // Configured HC ceiling: 0..3000 mA
    s.highCurrentMA = uint32_t(limit) * 25;
    s.apsdComplete = partner[0] & 1;
    s.apsdResult = partner[1] & 0x7f;
    s.typeCSource = partner[4] & 0xe0;
    s.sdp = s.apsdComplete && s.apsdResult == 1 && s.typeCSource == 0x80;
    s.highCurrent = options[1] & 1;
    const bool usb3 = options[1] & 4;
    s.sdpMA = options[1] & 2 ? (usb3 ? 900 : 500) : (usb3 ? 150 : 100);
    s.suspended = suspend & 1;
    s.aiclComplete = input[3] & 1;
    s.aiclFailed = input[3] & 2;
    s.softLimited = input[3] & 0x40;
    output = s;
    return Result::Success;
}
}
