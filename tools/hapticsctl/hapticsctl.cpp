// SPDX-License-Identifier: GPL-2.0-only
#include <IOKitUser.h>
#include <HapticsProtocol.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static bool decimal(const char *s, uint64_t min, uint64_t max, uint64_t &out) {
    if (!s || !*s) return false;
    uint64_t v = 0;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9' || v > max / 10) return false;
        v = v * 10 + unsigned(*s - '0');
        if (v > max) return false;
    }
    out = v; return v >= min;
}
int main(int argc, char **argv) {
    const bool schema = argc == 2 && !strcmp(argv[1], "--schema");
    if (schema) {
        puts("PMI8998Haptics diagnostic protocol=1; info; status; stop; pulse <5..15000 ms> <1..1000 strength>; administrator required; close cancels output");
        return 0;
    }
    uint64_t input[2] = {};
    const bool info = argc == 2 && !strcmp(argv[1], "info");
    const bool status = argc == 2 && !strcmp(argv[1], "status");
    const bool stop = argc == 2 && !strcmp(argv[1], "stop");
    const bool pulse = argc == 4 && !strcmp(argv[1], "pulse") &&
        decimal(argv[2], 5, 15000, input[0]) && decimal(argv[3], 1, 1000, input[1]);
    if (!info && !status && !stop && !pulse) {
        fputs("usage: hapticsctl info | status | stop | pulse <5..15000 ms> <1..1000 strength> | --schema\n", stderr);
        return 2;
    }
    auto matching = IOServiceMatching("PMI8998Haptics");
    if (!matching) return 1;
    mach_port_t iterator = MACH_PORT_NULL, service = MACH_PORT_NULL;
    auto result = IOServiceGetMatchingServices(MACH_PORT_NULL, matching, &iterator);
    if (result) { fprintf(stderr, "matching failed: 0x%x\n", result); return 1; }
    unsigned found = 0;
    for (mach_port_t next; (next = IOIteratorNext(iterator));) {
        ++found;
        if (!service) service = next; else IOObjectRelease(next);
    }
    IOObjectRelease(iterator);
    if (found != 1) {
        if (service) IOObjectRelease(service);
        fprintf(stderr, "expected one PMI8998Haptics service, found %u (board ownership is disabled by default)\n", found);
        return 1;
    }
    mach_port_t connection = MACH_PORT_NULL;
    result = IOServiceOpen(service, mach_task_self(), Haptics::DiagnosticClient, &connection);
    IOObjectRelease(service);
    if (result) { fprintf(stderr, "open failed (administrator required): 0x%x\n", result); return 1; }
    uint64_t output[Haptics::StatusScalars] = {};
    uint32_t count = Haptics::InfoScalars;
    result = IOConnectCallScalarMethod(connection, Haptics::GetInfo, nullptr, 0, output, &count);
    if (!result && (count != Haptics::InfoScalars || output[0] != Haptics::Version)) result = KERN_INVALID_ARGUMENT;
    if (!result && info)
        printf("protocol=%llu period-us=%llu max-mV=%llu limit-mA=%llu max-ms=%llu min-ms=%llu max-strength=%llu native-AOP-bridge=%llu\n",
            output[0], output[1], output[2], output[3], output[4], output[5], output[6], output[7]);
    if (!result && (pulse || stop)) {
        count = 0;
        result = IOConnectCallScalarMethod(connection, pulse ? Haptics::Play : Haptics::Stop,
            pulse ? input : nullptr, pulse ? 2 : 0, nullptr, &count);
        if (!result && count) result = KERN_INVALID_ARGUMENT;
        // Keep the owning connection alive through pulse + the five-cycle
        // brake interval. The kernel enforces the deadline independently.
        if (!result) usleep(unsigned((pulse ? input[0] + 35 : 35) * 1000));
    }
    if (!result && !info) {
        count = Haptics::StatusScalars;
        result = IOConnectCallScalarMethod(connection, Haptics::GetStatus, nullptr, 0, output, &count);
        if (!result && (count != Haptics::StatusScalars || output[0] != Haptics::Version ||
            output[1] > uint32_t(Haptics::State::Fault))) result = KERN_INVALID_ARGUMENT;
        if (!result) {
            printf("state=%llu initialized=%llu output-known-off=%llu strength=%llu applied-mV=%llu remaining-ms=%llu result=%llu io-error=0x%llx generation=%llu awake=%llu owned=%llu\n",
                output[1], output[2], output[3], output[4], output[5], output[6], output[7], output[8], output[9], output[10], output[11]);
            if (output[1] == uint32_t(Haptics::State::Fault)) result = KERN_FAILURE;
        }
    }
    IOServiceClose(connection);
    if (result) fprintf(stderr, "request failed: 0x%x\n", result);
    return result ? 1 : 0;
}
