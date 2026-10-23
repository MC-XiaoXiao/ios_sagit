// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace Gyro {
enum class Model : uint32_t { LSM6DSM = 1, ICM20690 = 2 };
enum class Result { Success, NoData, Invalid, Identity, IO, Configuration, Restore };
struct Sample { int32_t axis[3]{}; }; // microradians/second, uncalibrated
struct Mount {
    int32_t axis[3]{1, 2, 3}; // signed, one-based source axes
    bool valid() const {
        unsigned index[3], mask = 0;
        int sign = 1;
        for (unsigned i = 0; i < 3; ++i) {
            if (!axis[i] || axis[i] < -3 || axis[i] > 3) return false;
            index[i] = unsigned(axis[i] < 0 ? -axis[i] : axis[i]) - 1;
            if (mask & (1U << index[i])) return false;
            mask |= 1U << index[i];
            if (axis[i] < 0) sign = -sign;
        }
        for (unsigned i = 0; i < 3; ++i)
            for (unsigned j = i + 1; j < 3; ++j)
                if (index[i] > index[j]) sign = -sign;
        // Angular velocity is an axial vector: only proper rotations accepted.
        return mask == 7 && sign == 1;
    }
    bool apply(const Sample &in, Sample &out) const {
        if (!valid()) return false;
        Sample mapped;
        for (unsigned i = 0; i < 3; ++i) {
            const int32_t a = axis[i];
            const int64_t value = int64_t(in.axis[(a < 0 ? -a : a) - 1]) * (a < 0 ? -1 : 1);
            if (value < INT32_MIN || value > INT32_MAX) return false;
            mapped.axis[i] = int32_t(value);
        }
        out = mapped;
        return true;
    }
};
inline int32_t angularRate(Model model, int16_t raw) {
    // ST: 70 mdps/LSB at 2000 dps. ICM20690: 16.4 LSB/dps at 2000 dps.
    // Integer arithmetic only; conversion is symmetric and avoids -32768
    // overflow when a mounted axis is inverted. pi rounded to 9 decimals.
    const int64_t numerator = model == Model::LSM6DSM
        ? int64_t(raw) * 70 * 3141592654LL
        : int64_t(raw) * 10 * 3141592654LL;
    const int64_t denominator = model == Model::LSM6DSM ? 180000000LL : 29520000LL;
    return int32_t((numerator + (numerator >= 0 ? denominator / 2 : -denominator / 2)) / denominator);
}

// Transport reads/writes chip registers over SPI and provides delayMS().
// Exclusive ownership covers the entire IMU (gyro shares control with accel).
// No chip reset, firmware loading, FIFO, fusion, or fabricated calibration.
template<class Transport> class Sensor {
    Transport &bus;
    Model model = Model::LSM6DSM;
    struct Saved { uint8_t reg, value; } saved[12]{};
    unsigned count = 0;
    bool active = false;
    bool read(uint8_t reg, uint8_t &value) { return bus.read(reg, &value, 1); }
    bool write(uint8_t reg, uint8_t value) {
        uint8_t observed = 0;
        return bus.write(reg, value) && read(reg, observed) && observed == value;
    }
    bool save(const uint8_t *regs, unsigned n) {
        if (count || n > 12) return false;
        // No mutation until every original register has been read.
        for (unsigned i = 0; i < n; ++i) {
            saved[i].reg = regs[i];
            if (!read(regs[i], saved[i].value)) return false;
        }
        count = n;
        return true;
    }
    uint8_t original(uint8_t reg) const {
        for (unsigned i = 0; i < count; ++i) if (saved[i].reg == reg) return saved[i].value;
        return 0;
    }
    Result failed() {
        active = false;
        return restore() ? Result::IO : Result::Restore;
    }
public:
    explicit Sensor(Transport &transport) : bus(transport) {}
    bool ready() const { return active; }
    bool pendingRestore() const { return count != 0; }
    bool restore() {
        active = false;
        if (!count) return true;
        // Stop sampling before restoring endian/scaling registers. On a failed
        // standby keep the originals and let the next cleanup attempt retry.
        if (!sleep()) return false;
        bool ok = true;
        // Power/ODR register is saved first, so it is restored last.
        for (unsigned i = count; i--;)
            if (!write(saved[i].reg, saved[i].value)) ok = false;
        if (ok) count = 0; // Never replace the originals after a failed rollback.
        return ok;
    }
    Result initialize(Model requested) {
        active = false;
        if (requested != Model::LSM6DSM && requested != Model::ICM20690) return Result::Invalid;
        if (count && !restore()) return Result::Restore;
        model = requested;
        uint8_t identity = 0;
        if (!read(model == Model::LSM6DSM ? 0x0f : 0x75, identity)) return Result::IO;
        if (identity != (model == Model::LSM6DSM ? 0x6a : 0x20)) return Result::Identity;
        if (model == Model::LSM6DSM) {
            uint8_t bank = 0;
            if (!read(0x01, bank)) return Result::IO;
            if (bank & 0xe0) return Result::Configuration;
            const uint8_t regs[] = {0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x0a, 0x0d, 0x0e};
            if (!save(regs, sizeof(regs))) return Result::IO;
            // Reject a reset/boot in progress or 3-wire interface.
            if (original(0x12) & 0x89) { count = 0; return Result::Configuration; }
            // Fixed 208 Hz / 2000 dps; BDU + auto increment, little endian.
            if (!write(0x11, 0x0c) ||
                !write(0x12, (original(0x12) & 0x30) | 0x44) ||
                !write(0x13, 0x04) || // SPI only; gyro awake; LPF1 disabled
                !write(0x14, original(0x14) & 0x03) || // no gyro self-test/rounding
                !write(0x15, original(0x15) & 0x1c) || // no DEN gating
                !write(0x16, 0) || // high-performance, high-pass off
                !write(0x0a, 0) || !write(0x0d, 0) || !write(0x0e, 0) ||
                !write(0x11, 0x5c)) return failed();
        } else {
            const uint8_t regs[] = {0x6b, 0x6c, 0x6a, 0x23, 0x37, 0x38, 0x1a, 0x1b, 0x19};
            if (!save(regs, sizeof(regs))) return Result::IO;
            if (original(0x6b) & 0x80 || original(0x6a) & 0x0f) {
                count = 0; return Result::Configuration;
            }
            // ICM20690's FSR field is bits 3:2, NOT the MPU6500 bits 4:3.
            // Gyro 2000 dps, DLPF 41 Hz, 1 kHz / (4+1) = 200 Hz.
            if (!write(0x6b, 0x01)) return failed();
            bus.delayMS(100);
            if (!write(0x6c, 0x38) || !write(0x6a, 0x10) ||
                !write(0x23, 0) || !write(0x37, 0) || !write(0x38, 1) ||
                !write(0x1a, 3) || !write(0x1b, 0x0c) ||
                !write(0x19, 4)) return failed();
        }
        bus.delayMS(100); // covers both manufacturers' gyro startup time
        // Drain a possible pre-configuration sample before publishing.
        uint8_t discarded[6];
        if (!bus.read(model == Model::LSM6DSM ? 0x22 : 0x43, discarded, sizeof(discarded))) return failed();
        active = true;
        return Result::Success;
    }
    bool sleep() {
        active = false;
        if (!count) return true;
        return model == Model::LSM6DSM ? write(0x11, 0x0c) : write(0x6c, 0x3f);
    }
    Result sample(Sample &out) {
        if (!active) return Result::Configuration;
        uint8_t status = 0, config = 0, identity = 0, interface = 0, power = 0;
        const bool st = model == Model::LSM6DSM;
        if (!read(st ? 0x0f : 0x75, identity) || !read(st ? 0x11 : 0x1b, config) ||
            !read(st ? 0x12 : 0x6c, interface) || !read(st ? 0x13 : 0x6b, power)) return Result::IO;
        if (identity != (st ? 0x6a : 0x20) || config != (st ? 0x5c : 0x0c) ||
            (st ? ((interface & 0xcf) != 0x44) : interface != 0x38) ||
            power != (st ? 4 : 1))
            return Result::Configuration;
        if (!read(st ? 0x1e : 0x3a, status)) return Result::IO;
        if (!(status & (st ? 2 : 1))) return Result::NoData;
        uint8_t bytes[6];
        if (!bus.read(st ? 0x22 : 0x43, bytes, sizeof(bytes))) return Result::IO;
        Sample next;
        for (unsigned i = 0; i < 3; ++i) {
            const uint16_t word = st ? (uint16_t(bytes[2*i+1]) << 8) | bytes[2*i]
                                     : (uint16_t(bytes[2*i]) << 8) | bytes[2*i+1];
            const int32_t signedValue = word & 0x8000 ? int32_t(word) - 65536 : word;
            next.axis[i] = angularRate(model, int16_t(signedValue));
        }
        out = next;
        return Result::Success;
    }
};
}
