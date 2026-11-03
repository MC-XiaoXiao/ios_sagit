// SPDX-License-Identifier: GPL-2.0-only
#include <KernelShim.h>
#include "../QcomBattery.cpp"
#include <cstdio>
static uint64_t number(IOService *s, const char *key) {
    auto *n = dynamic_cast<OSNumber *>(s->getProperty(key)); assert(n); return n->value;
}
static void cell(IOService *s, const char *key, std::initializer_list<uint32_t> values) {
    auto *data = new OSData;
    for (uint32_t v : values) for (unsigned i = 0; i < 4; ++i) data->bytes.push_back(v >> (8 * i));
    s->setProperty(key, data); data->release();
}
static IOReturn request(IOService *s, const char *key, OSObject *v) {
    auto *d = new OSDictionary; d->setObject(key, v);
    auto r = s->setProperties(d); d->release(); return r;
}
int main() {
    const int baseline = liveObjects;
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        auto *dt = new IOService;
        cell(dt, "qcom,battery-observer", {1, 2, 0x4000, 0x4100, 0x1000, 0x1600, 5000});
        if (scenario != 1) cell(dt, "qcom,battery-charge-control", {1});
        auto *spmi = new QcomSPMI; spmi->attach(dt);
        std::array<uint8_t, 65536> bytes{};
        bytes[0x103] = 2; bytes[0x104] = 0x51; bytes[0x105] = 0x15;
        bytes[0x4005] = 0x10; bytes[0x4105] = 0x11;
        bytes[0x4046] = 0x80; bytes[0x4010] = 2;
        bytes[0x4009] = bytes[0x400a] = 128;
        bytes[0x41a0] = bytes[0x41a6] = 0; bytes[0x41a1] = bytes[0x41a7] = 125;
        bytes[0x4106] = 1; bytes[0x4150] = 0xa8; bytes[0x4151] = 4;
        bytes[0x160b] = 0x11; bytes[0x1006] = 2; bytes[0x100d] = 0x20;
        bytes[0x1042] = 0x81;
        if (scenario == 2) bytes[0x1051] = 0xc5;
        unsigned reads = 0, writes = 0; bool offline = true, failWrite = false, failInput = false;
        spmi->transport = [&](UInt8 sid, UInt16 address, UInt8 *p, unsigned n, bool write) {
            assert(sid == 2);
            if (write) { ++writes; assert((address == 0x1042 || address == 0x1051) && n == 1); }
            else ++reads;
            if (offline || (write && failWrite) || (!write && failInput && address == 0x1365)) return -99;
            for (unsigned i = 0; i < n; ++i) {
                if (write) bytes[address + i] = p[i]; else p[i] = bytes[address + i];
            }
            return kIOReturnSuccess;
        };
        auto *driver = new QcomBattery; driver->attach(spmi);
        eventAdds = 0; failEventAdd = scenario == 3 || scenario == 4 ? scenario - 2 : 0;
        failPM = scenario == 5;
        if (scenario >= 3) assert(!driver->start(spmi));
        else {
            assert(driver->start(spmi));
            assert(driver->children.empty() && !writes);
            offline = false; driver->getWorkLoop()->fire();
            auto *source = dynamic_cast<QcomBatteryPowerSource *>(driver->children.front());
            assert(source && number(source, "CurrentCapacity") == 50);
            assert(source->getProperty("IsCharging") == kOSBooleanTrue);
            assert(!writes); // merely starting/observing never enables the charger
            const unsigned notices = source->notifications;
            driver->getWorkLoop()->fire(); assert(source->notifications == notices);
            administrator = false;
            assert(request(source, "ChargingEnabled", kOSBooleanFalse) == kIOReturnNotPrivileged);
            assert(!writes); administrator = true;
            auto *invalid = OSNumber::withNumber(1, 32);
            assert(request(source, "ChargingEnabled", invalid) == kIOReturnBadArgument);
            assert(request(source, "ChargeLimit", invalid) == kIOReturnBadArgument);
            invalid->release(); assert(!writes);
            auto *mixed = new OSDictionary;
            mixed->setObject("ChargingEnabled", kOSBooleanFalse);
            mixed->setObject("RawRegister", kOSBooleanTrue);
            assert(source->setProperties(mixed) == kIOReturnUnsupported);
            mixed->release(); assert(!writes);
            const auto r = request(source, "ChargingEnabled", kOSBooleanFalse);
            if (scenario == 1) assert(r == kIOReturnUnsupported && !writes);
            else {
                assert(r == kIOReturnSuccess && !(bytes[0x1042] & 1));
                assert(!(bytes[0x1051] & 0xc0));
                assert(source->getProperty("ChargeEnablePinControlled") == kOSBooleanFalse);
                assert(source->getProperty("IsCharging") == kOSBooleanFalse);
                assert(number(source, "ChargeInhibitReasons") & PMICBattery::Manual);
                assert(request(source, "IsCharging", kOSBooleanFalse) == kIOReturnSuccess);
                assert(request(source, "ChargingEnabled", kOSBooleanTrue) == kIOReturnSuccess);
                assert(!(bytes[0x1042] & 1)); // manual enable preserves system inhibit
                assert(request(source, "IsCharging", kOSBooleanTrue) == kIOReturnSuccess);
                assert(bytes[0x1042] & 1);
                failInput = true; driver->getWorkLoop()->fire();
                assert(source->getProperty("BatteryTelemetryValid") == kOSBooleanTrue);
                assert(source->getProperty("USBInputStatusValid") == kOSBooleanFalse);
                assert(!source->getProperty("USBInputSettledLimitMA"));
                assert(source->getProperty("ChargeControlFault") == kOSBooleanFalse && (bytes[0x1042] & 1));
                failInput = false; driver->getWorkLoop()->fire();
                assert(source->getProperty("USBInputStatusValid") == kOSBooleanTrue);
                assert(request(source, "PredictiveChargingInhibit", kOSBooleanTrue) == kIOReturnSuccess);
                assert(!(bytes[0x1042] & 1));
                assert(request(source, "PredictiveChargingInhibit", kOSBooleanFalse) == kIOReturnSuccess);
                assert(bytes[0x1042] & 1);
                auto *limit = OSNumber::withNumber(80, 32);
                assert(request(source, "ChargeLimit", limit) == kIOReturnSuccess); limit->release();
                bytes[0x4009] = bytes[0x400a] = 204; // exactly 80%
                driver->getWorkLoop()->fire(); assert(!(bytes[0x1042] & 1));
                bytes[0x4009] = bytes[0x400a] = 199; // 78%
                driver->getWorkLoop()->fire(); assert(bytes[0x1042] & 1);
                offline = true; driver->getWorkLoop()->fire();
                assert(source->getProperty("BatteryTelemetryValid") == kOSBooleanFalse);
                assert(source->getProperty("ChargeControlFault") == kOSBooleanTrue);
                assert(!source->getProperty("IsCharging") && !source->getProperty("BatteryNetCharging"));
                offline = false; driver->getWorkLoop()->fire();
                assert(!(bytes[0x1042] & 1));
                assert(request(source, "IsCharging", kOSBooleanTrue) == kIOReturnIOError);
                assert(!(bytes[0x1042] & 1)); // native vote cannot clear fault
                failWrite = true;
                // Disable is already verified: recovery then attempts enable and fails.
                assert(request(source, "ChargingEnabled", kOSBooleanTrue) == kIOReturnIOError);
                failWrite = false;
                assert(request(source, "ChargingEnabled", kOSBooleanTrue) == kIOReturnSuccess);
                assert(bytes[0x1042] & 1);
            }
            driver->setPowerState(0, spmi);
            const unsigned paused = reads;
            driver->getWorkLoop()->fire(); assert(reads == paused);
            assert(request(source, "ChargingEnabled", kOSBooleanTrue) == kIOReturnNotReady);
            if (scenario != 1) assert(!(bytes[0x1042] & 1));
            driver->setPowerState(1, spmi);
            if (scenario != 1) assert(bytes[0x1042] & 1);
            driver->stop(spmi);
            if (scenario != 1) assert(!(bytes[0x1042] & 1));
            assert(driver->children.empty() && !driver->getWorkLoop());
        }
        driver->detach(spmi); driver->release(); spmi->detach(dt); spmi->release(); dt->release();
        assert(liveObjects == baseline);
    }
    puts("Production IOKit battery adapter: privileges, votes, notifications, PM, faults and rollback passed");
}
