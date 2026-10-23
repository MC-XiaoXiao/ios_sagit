#include <GyroHID.h>
#include <cassert>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <vector>
#include <utility>

struct Chip {
    uint8_t regs[128]{};
    unsigned operations = 0, failAt = 0, delays = 0, writes = 0;
    bool failWrites = false;
    std::vector<std::pair<uint8_t, uint8_t>> history;
    explicit Chip(Gyro::Model model) {
        regs[0x0f] = model == Gyro::Model::LSM6DSM ? 0x6a : 0xff;
        regs[0x75] = model == Gyro::Model::ICM20690 ? 0x20 : 0xff;
        regs[0x11] = 0x2c; regs[0x12] = 0x04;
        regs[0x13] = 0x40; regs[0x14] = 0x0c; regs[0x15] = 0x10;
        regs[0x16] = 0x80; regs[0x0a] = 0x26; regs[0x0d] = 0x02;
        regs[0x6b] = 0x41; regs[0x6c] = 0x3f; regs[0x6a] = 0x10;
    }
    bool read(uint8_t reg, uint8_t *out, size_t length) {
        if (++operations == failAt) return false;
        assert(reg + length <= sizeof(regs));
        memcpy(out, regs + reg, length); return true;
    }
    bool write(uint8_t reg, uint8_t value) {
        ++writes;
        // Simulate an ambiguous write which reached the chip before transport
        // reported failure. Rollback must include this very register.
        regs[reg] = value; history.emplace_back(reg, value);
        return ++operations != failAt && !failWrites;
    }
    void delayMS(unsigned value) { delays += value; }
    void sample(Gyro::Model model, int16_t x, int16_t y, int16_t z) {
        const int16_t values[]{x, y, z};
        bool st = model == Gyro::Model::LSM6DSM;
        regs[st ? 0x1e : 0x3a] = st ? 2 : 1;
        unsigned base = st ? 0x22 : 0x43;
        for (unsigned i = 0; i < 3; ++i) {
            uint16_t word = uint16_t(values[i]);
            regs[base+2*i] = st ? word : word >> 8;
            regs[base+2*i+1] = st ? word >> 8 : word;
        }
    }
};

void sensor(Gyro::Model model) {
    Chip chip(model); Gyro::Sensor<Chip> device(chip);
    uint8_t original[128]; memcpy(original, chip.regs, sizeof(original));
    assert(device.initialize(model) == Gyro::Result::Success && device.ready());
    assert(chip.delays >= 100);
    if (model == Gyro::Model::ICM20690) assert(chip.regs[0x1b] == 0x0c);
    Gyro::Sample result{{11, 22, 33}};
    assert(device.sample(result) == Gyro::Result::NoData && result.axis[0] == 11);
    chip.sample(model, 32767, -32768, 0);
    assert(device.sample(result) == Gyro::Result::Success && result.axis[2] == 0);
    for (unsigned i = 0; i < 2; ++i) {
        const double raw = i ? -32768 : 32767;
        const double dps = model == Gyro::Model::LSM6DSM ? raw * .07 : raw / 16.4;
        assert(std::abs(result.axis[i] - dps * std::acos(-1.) / 180. * 1000000.) < 0.6);
        assert(std::abs(result.axis[i]) <= 41000000);
    }
    Gyro::Mount mount{{-1, 2, -3}};
    Gyro::Sample mapped;
    assert(mount.apply(result, mapped) && mapped.axis[0] == -result.axis[0]);
    auto saved = result;
    chip.failAt = chip.operations + 6; // fail the actual burst after status
    assert(device.sample(result) == Gyro::Result::IO);
    assert(!memcmp(&saved, &result, sizeof(saved)));
    chip.failAt = 0;
    chip.regs[model == Gyro::Model::LSM6DSM ? 0x11 : 0x1b] = 0;
    assert(device.sample(result) == Gyro::Result::Configuration);
    assert(device.sleep() && !device.ready());
    assert(device.initialize(model) == Gyro::Result::Success);
    assert(device.restore() && !device.pendingRestore());
    // Only the data/status registers deliberately injected above may differ.
    for (auto [reg, value] : chip.history) { (void)value; assert(chip.regs[reg] == original[reg]); }
    assert(device.sample(result) == Gyro::Result::Configuration);

    Chip count(model); Gyro::Sensor<Chip> probe(count);
    assert(probe.initialize(model) == Gyro::Result::Success);
    for (unsigned fail = 1; fail <= count.operations; ++fail) {
        Chip failing(model); Gyro::Sensor<Chip> s(failing);
        uint8_t initial[128]; memcpy(initial, failing.regs, sizeof(initial));
        failing.failAt = fail;
        assert(s.initialize(model) != Gyro::Result::Success);
        assert(!s.ready());
        assert(s.restore());
        assert(!memcmp(initial, failing.regs, sizeof(initial)));
    }
    Chip rollback(model); Gyro::Sensor<Chip> sticky(rollback);
    assert(sticky.initialize(model) == Gyro::Result::Success);
    rollback.failWrites = true;
    assert(!sticky.restore() && sticky.pendingRestore());
    assert(sticky.initialize(model) == Gyro::Result::Restore);
    rollback.failWrites = false;
    assert(sticky.restore() && !sticky.pendingRestore());
    assert(!memcmp(original, rollback.regs, sizeof(original)));
    Chip wrong(model); Gyro::Sensor<Chip> identity(wrong);
    wrong.regs[model == Gyro::Model::LSM6DSM ? 0x0f : 0x75] = 0;
    assert(identity.initialize(model) == Gyro::Result::Identity && wrong.writes == 0);
}

void hid() {
    uint8_t bytes[GyroHID::InputBytes]{};
    Gyro::Sample sample{{123456, -654321, 0}};
    GyroHID::report(sample, bytes);
    assert(bytes[0] == 1 && GyroHID::get32(bytes+1) == 123456);
    assert(int32_t(GyroHID::get32(bytes+5)) == -654321);
    uint32_t interval = 77;
    for (uint32_t value : {5000U, 10000U, 1000000U}) {
        GyroHID::feature(value, bytes);
        assert(GyroHID::interval(bytes, 5, interval) && interval == value);
    }
    for (uint32_t value : {0U, 4999U, 1000001U, UINT32_MAX}) {
        GyroHID::feature(value, bytes); interval = 77;
        assert(!GyroHID::interval(bytes, 5, interval) && interval == 77);
    }
    assert(!GyroHID::interval(nullptr, 5, interval));
    assert(!GyroHID::interval(bytes, 4, interval));
    GyroHID::feature(10000, bytes); bytes[0] = 1;
    assert(!GyroHID::interval(bytes, 5, interval));
    // Parse actual HID items, independently checking the wire sizes, signed
    // range, SI units and native Sensor usage numbers.
    uint32_t size = 0, count = 0, id = 0, page = 0, exponent = 0, unit = 0;
    int32_t minimum = 0, maximum = 0, physicalMin = 0, physicalMax = 0;
    unsigned inputBits = 0, featureBits = 0, collections = 0;
    std::vector<uint32_t> usages;
    const auto &d = GyroHID::Descriptor;
    for (unsigned p = 0; p < sizeof(d);) {
        uint8_t key = d[p++];
        unsigned n = key & 3; if (n == 3) n = 4;
        assert(p + n <= sizeof(d));
        uint32_t value = 0;
        for (unsigned i = 0; i < n; ++i) value |= uint32_t(d[p++]) << (8*i);
        switch (key & 0xfc) {
        case 0x04: page = value; break;
        case 0x08: usages.push_back(value); break;
        case 0x14: minimum = int32_t(value); break;
        case 0x24: maximum = int32_t(value); break;
        case 0x34: physicalMin = int32_t(value); break;
        case 0x44: physicalMax = int32_t(value); break;
        case 0x54: exponent = value; break;
        case 0x64: unit = value; break;
        case 0x74: size = value; break;
        case 0x84: id = value; break;
        case 0x94: count = value; break;
        case 0xa0:
            assert(page == 32 && usages == std::vector<uint32_t>{118});
            ++collections; usages.clear(); break;
        case 0xc0: --collections; break;
        case 0x80:
            assert(id == 1 && page == 32 && exponent == 10 && unit == 0xf012);
            assert(minimum == -41000000 && maximum == 41000000);
            assert(physicalMin == minimum && physicalMax == maximum);
            assert((usages == std::vector<uint32_t>{0x457, 0x458, 0x459}));
            assert(value == 2); inputBits += size * count; usages.clear(); break;
        case 0xb0:
            assert(id == 2 && exponent == 10 && unit == 0x1001);
            assert(minimum == 5000 && maximum == 1000000);
            assert(physicalMin == minimum && physicalMax == maximum);
            assert(usages == std::vector<uint32_t>{0x30e});
            featureBits += size * count; usages.clear(); break;
        default: assert(false);
        }
    }
    assert(collections == 0 && inputBits / 8 + 1 == GyroHID::InputBytes);
    assert(featureBits / 8 + 1 == GyroHID::FeatureBytes);
    // Apple IOHIDElementPrivate::getScaledFixedValue(Exponent) uses physical
    // range + decimal exponent. Exercise its integer arithmetic, including 0.
    for (int64_t value : {-40033664LL, -1000000LL, 0LL, 1000000LL, 40033664LL}) {
        int64_t factor = (82000000LL * 65536) / 1000000;
        int64_t offset = (-41000000LL * 65536) / 1000000;
        int64_t native = (value + 41000000LL) * factor / 82000000LL + offset;
        assert(std::abs(native / 65536. - value / 1000000.) < 0.000031);
    }
}

int main() {
    sensor(Gyro::Model::LSM6DSM); sensor(Gyro::Model::ICM20690);
    for (auto a : {Gyro::Mount{{1, 1, 3}}, Gyro::Mount{{0, 2, 3}},
                   Gyro::Mount{{1, 2, -3}}, Gyro::Mount{{INT32_MIN, 2, 3}}}) assert(!a.valid());
    unsigned rotations = 0;
    for (int x = -3; x <= 3; ++x) for (int y = -3; y <= 3; ++y) for (int z = -3; z <= 3; ++z)
        if (Gyro::Mount{{x, y, z}}.valid()) ++rotations;
    assert(rotations == 24);
    hid();
    puts("Gyro: identities, register rollback, PM, endian/scaling, mount and HID reports passed");
}
