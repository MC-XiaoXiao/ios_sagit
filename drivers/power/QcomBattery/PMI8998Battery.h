// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdint.h>

// Register evidence: docs/battery/reference (Xiaomi sagit-n-oss).
// This observer never programs the charger, FG SRAM, IRQs or battery profile.
namespace PMICBattery {
enum class Result : uint32_t { Success, InvalidConfig, IOFailure, Unsupported,
                               NotReady, Incoherent, OutOfRange, Suspended };
struct Config {
    uint32_t version = 1, sid = 2, soc = 0x4000, info = 0x4100;
    uint32_t charger = 0x1000, misc = 0x1600, pollMS = 5000;
    bool valid() const {
        return version == 1 && sid <= 15 && soc == 0x4000 && info == 0x4100 &&
            charger == 0x1000 && misc == 0x1600 && pollMS >= 1000 && pollMS <= 60000;
    }
};
struct Sample {
    bool installed = false, external = false, charging = false, full = false;
    bool temperatureValid = false;
    uint32_t percent = 0, millivolts = 0;
    int32_t milliamps = 0, centiCelsius = 0;
    uint8_t revision = 0, rawSOC = 0, chargeState = 0, chargerFaults = 0;
    uint8_t powerPath = 0;
};
inline uint16_t word(const uint8_t *p, bool swapped) {
    return swapped ? (uint16_t(p[0]) << 8) | p[1] : (uint16_t(p[1]) << 8) | p[0];
}
inline bool equal(const uint8_t *a, const uint8_t *b, unsigned n) {
    for (unsigned i = 0; i < n; ++i) if (a[i] != b[i]) return false;
    return true;
}

// Bus exposes read(sid, address, bytes, count) only. Failure never commits a
// partial sample. All retries are bounded, including register shadow races.
template<class Bus> Result readSample(Bus &bus, const Config &c, Sample &output) {
    if (!c.valid()) return Result::InvalidConfig;
    auto read = [&](uint16_t address, uint8_t *p, unsigned n) {
        return bus.read(uint8_t(c.sid), address, p, n);
    };
    uint8_t id[3]{}, socType = 0, infoType = 0;
    if (!read(0x103, id, 3) || !read(c.soc + 5, &socType, 1) ||
        !read(c.info + 5, &infoType, 1)) return Result::IOFailure;
    if (id[1] != 0x51 || id[2] != 0x15 || id[0] < 1 || id[0] > 2 ||
        socType != 0x10 || infoType != 0x11) return Result::Unsupported;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        Sample s{};
        s.revision = id[0];
        uint8_t present = 0, presentAfter = 0, path = 0, pathAfter = 0;
        uint8_t charger[8]{}, chargerAfter[8]{};
        if (!read(c.info + 7, &present, 1) || !read(c.misc + 0x0b, &path, 1) ||
            !read(c.charger + 6, charger, 8)) return Result::IOFailure;
        s.installed = !(present & 7);
        s.powerPath = path;
        s.external = (path & 1) && (path & 0x18);
        s.chargeState = charger[0] & 7;
        s.chargerFaults = charger[1] & 0x73; // expiry, OV, missing, hot/cold
        if (s.installed) {
            uint8_t enabled = 0, ready = 0, restart = 0, cap[2]{};
            if (!read(c.soc + 0x46, &enabled, 1) || !read(c.soc + 0x10, &ready, 1) ||
                !read(c.soc + 0x48, &restart, 1)) return Result::IOFailure;
            // SOC-ready is interrupt line 1; require a running retained FG.
            if (!(enabled & 0x80) || !(ready & 2) || (restart & 1))
                return Result::NotReady;
            bool matched = false;
            for (unsigned n = 0; n < 5; ++n) {
                if (!read(c.soc + 9, cap, 2)) return Result::IOFailure;
                if (cap[0] == cap[1]) { matched = true; break; }
            }
            if (!matched) return Result::Incoherent;
            uint8_t vi[4]{}, shadow[4]{}, temp[2]{}, tempStatus = 0;
            if (!read(c.info + 0xa0, vi, 4) || !read(c.info + 0xa6, shadow, 4) ||
                !read(c.info + 6, &tempStatus, 1) ||
                !read(c.info + 0x50, temp, 2)) return Result::IOFailure;
            if (!equal(vi, shadow, 4)) continue;
            const uint16_t voltage = word(vi, s.revision == 1);
            const uint16_t current = word(vi + 2, s.revision == 1);
            const int32_t signedCurrent = current & 0x8000 ? int32_t(current) - 65536 : current;
            s.millivolts = uint64_t(voltage) * 122070 / 1000000;
            // Qualcomm positive = discharge; Apple positive = charge.
            s.milliamps = -(int64_t(signedCurrent) * 488281 / 1000000);
            if (s.millivolts < 1500 || s.millivolts > 5000) return Result::OutOfRange;
            s.rawSOC = cap[0];
            s.percent = (uint32_t(cap[0]) * 100 + 127) / 255;
            // Preserve quarter-Kelvin resolution; Apple Temperature uses 0.01 C.
            s.centiCelsius = int32_t((uint16_t(temp[1] & 7) << 8) | temp[0]) * 25 - 27315;
            s.temperatureValid = (tempStatus & 1) && s.centiCelsius >= -4000 && s.centiCelsius <= 10000;
            s.charging = s.external && !s.chargerFaults && s.chargeState <= 4 &&
                (charger[7] & 0xf0);
            // Inhibit is not evidence of full charge. Never force SOC to 100.
            s.full = s.external && !s.chargerFaults && s.chargeState == 5 && s.percent >= 99;
            uint8_t capAfter[2]{}, readyAfter = 0, enabledAfter = 0, restartAfter = 0;
            if (!read(c.soc + 9, capAfter, 2) || !read(c.soc + 0x10, &readyAfter, 1) ||
                !read(c.soc + 0x46, &enabledAfter, 1) ||
                !read(c.soc + 0x48, &restartAfter, 1)) return Result::IOFailure;
            if (!equal(cap, capAfter, 2) || !(readyAfter & 2) ||
                !(enabledAfter & 0x80) || (restartAfter & 1)) continue;
        }
        if (!read(c.info + 7, &presentAfter, 1) || !read(c.misc + 0x0b, &pathAfter, 1) ||
            !read(c.charger + 6, chargerAfter, 8)) return Result::IOFailure;
        if (present != presentAfter || path != pathAfter || charger[0] != chargerAfter[0] ||
            charger[1] != chargerAfter[1] || charger[7] != chargerAfter[7]) continue;
        output = s;
        return Result::Success;
    }
    return Result::Incoherent;
}
}
