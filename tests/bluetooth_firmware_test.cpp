// SPDX-License-Identifier: BSD-2-Clause
#include "BluetoothBoot.h"
#include "BluetoothFirmware.h"
#include <assert.h>
#include <stdio.h>
#include <vector>
#include <array>
#include <random>
using namespace Bluetooth;

struct PulseBackend {
    std::vector<unsigned> calls;
    unsigned failAt = 0;
    bool drainedNow = true;
    Result record(unsigned call) { calls.push_back(call); return calls.size() == failAt ? Result::Fault : Result::Success; }
    Result configure(unsigned baud, bool flow) { assert(!flow); return record(baud); }
    Result flush() { return record(1); }
    Result pulse(uint8_t value) { return record(value); }
    Result drained() { return drainedNow ? record(2) : Result::Pending; }
    Result reopen(unsigned baud) { assert(baud == InitialBaud); return record(3); }
};
struct Discard {
    Result packet(const uint8_t *, size_t) { return Result::Success; }
    Result signal(uint8_t) { return Result::Success; }
};
static void boot() {
    PulseBackend uart; BootSequence<PulseBackend> boot(uart);
    assert(boot.begin(0) == Result::Pending);
    assert(uart.calls == std::vector<unsigned>({2400,1,0xc0}));
    assert(boot.begin(0) == Result::Busy);
    uart.drainedNow = false;
    assert(boot.advance(1) == Result::Pending && uart.calls.size() == 3);
    uart.drainedNow = true;
    assert(boot.advance(2) == Result::Pending && uart.calls.back() == 2);
    assert(boot.advance(11) == Result::Pending && uart.calls.size() == 4);
    assert(boot.advance(12) == Result::Pending && uart.calls.back() == 0xfc);
    assert(boot.advance(13) == Result::Pending);
    assert(boot.advance(112) == Result::Pending);
    assert(boot.advance(113) == Result::Success);
    assert(uart.calls == std::vector<unsigned>({2400,1,0xc0,2,115200,1,0xfc,2,3}));
    for (unsigned fail = 1; fail <= 9; ++fail) {
        PulseBackend failing; failing.failAt = fail; BootSequence<PulseBackend> sequence(failing);
        auto result = sequence.begin(0);
        for (uint64_t now : {uint64_t(1),uint64_t(11),uint64_t(12),uint64_t(112)})
            if (result == Result::Pending) result = sequence.advance(now);
        assert(result == Result::Fault && sequence.currentPhase() == BootSequence<PulseBackend>::Phase::Fault);
        const size_t calls = failing.calls.size(); assert(sequence.advance(10000) == Result::Fault && calls == failing.calls.size());
    }
    PulseBackend timeout; BootSequence<PulseBackend> t(timeout); assert(t.begin(0) == Result::Pending);
    assert(t.advance(1000) == Result::Timeout); // a late drain is not accepted
    PulseBackend overflow; BootSequence<PulseBackend> o(overflow);
    assert(o.begin(UINT64_MAX) == Result::Invalid && overflow.calls.empty());
}

static void firmware() {
    std::vector<uint8_t> patch(600,0x5a);
    write32(patch.data(),uint32_t((patch.size() - 4) << 8) | 1);
    write32(patch.data() + 4,uint32_t(patch.size() - 4));
    write32(patch.data() + 8,100);
    FirmwareView view;
    for (uint8_t mode = 0; mode < 4; ++mode) {
        patch[14] = mode;
        assert(parseFirmware(patch.data(),patch.size(),FirmwareType::Patch,view) == Result::Success);
        FirmwareSegment segment;
        for (size_t offset = 0; offset < patch.size(); offset += FirmwareSegmentBytes) {
            assert(nextFirmwareSegment(view,offset,segment) == Result::Success);
            assert(segment.vendorEvent == (segment.last || !(mode & 1)));
            assert(segment.commandComplete == (segment.last || !(mode & 2)));
            std::array<uint8_t,249> output{}; size_t written = 100;
            assert(encodeFirmwareSegment(view,segment,output.data(),output.size(),written) == Result::Success);
            assert(written == segment.bytes + 6 && output[4] == 0x1e && output[5] == segment.bytes);
            assert(validPacket(output.data(),written,true) && read16(output.data() + 1) == DownloadOpcode);
            for (size_t i = 0; i < segment.bytes; ++i) assert(output[6 + i] == patch[offset + i]);
            assert(encodeFirmwareSegment(view,segment,output.data(),written - 1,written) == Result::Invalid && written == 0);
        }
        assert(nextFirmwareSegment(view,patch.size(),segment) == Result::Invalid);
        assert(nextFirmwareSegment(view,1,segment) == Result::Invalid);
    }
    std::vector<uint8_t> exact(2 * FirmwareSegmentBytes,0);
    write32(exact.data(),uint32_t((exact.size() - 4) << 8) | 1); exact[14] = 3;
    assert(parseFirmware(exact.data(),exact.size(),FirmwareType::Patch,view) == Result::Success);
    FirmwareSegment last;
    assert(nextFirmwareSegment(view,FirmwareSegmentBytes,last) == Result::Success);
    assert(last.bytes == FirmwareSegmentBytes && last.last && last.vendorEvent && last.commandComplete);
    patch[14] = 4;
    assert(parseFirmware(patch.data(),patch.size(),FirmwareType::Patch,view) == Result::Unsupported && !view.data);
    patch[14] = 0;
    assert(parseFirmware(patch.data(),patch.size() - 1,FirmwareType::Patch,view) == Result::Invalid);
    write32(patch.data() + 8,UINT32_MAX);
    assert(parseFirmware(patch.data(),patch.size(),FirmwareType::Patch,view) == Result::Invalid);
    std::vector<uint8_t> nvm(4 + 12 + 6 + 12,0);
    write32(nvm.data(),uint32_t((nvm.size() - 4) << 8) | 2);
    write16(nvm.data() + 4,2); write16(nvm.data() + 6,6);
    write16(nvm.data() + 22,17); // a zero-length tag still consumes its header
    assert(parseFirmware(nvm.data(),nvm.size(),FirmwareType::NVM,view) == Result::Success);
    write16(nvm.data() + 24,1);
    assert(parseFirmware(nvm.data(),nvm.size(),FirmwareType::NVM,view) == Result::Invalid);
    assert(parseFirmware(nullptr,0,FirmwareType::NVM,view) == Result::Invalid && !view.data);
    const uint8_t good[] = {Event,0xff,3,0,4,0}, failed[] = {Event,0xff,3,0,4,1};
    assert(validateDownloadEvent(good,sizeof(good)) == Result::Success);
    assert(validateDownloadEvent(failed,sizeof(failed)) == Result::Fault);
    assert(validateDownloadEvent(good,sizeof(good) - 1) == Result::Invalid);
    uint8_t version[] = {Event,0xff,14,0,2,1,0,0,0,2,0,3,0,4,0,0,0}; SoCVersion soc;
    assert(parseVersionEvent(version,sizeof(version),soc) == Result::Success);
    assert(soc.product == 1 && soc.patch == 2 && soc.rom == 3 && soc.soc == 4);
    version[11] = 0;
    assert(parseVersionEvent(version,sizeof(version),soc) == Result::Invalid);
    // Structural tests do not authenticate or authorize these synthetic assets.
}

static void malformedInputs() {
    std::mt19937 random(0x8353990); Discard sink;
    for (unsigned i = 0; i < 10000; ++i) {
        std::array<uint8_t,1024> bytes{};
        const size_t size = random() % bytes.size();
        for (size_t j = 0; j < size; ++j) bytes[j] = uint8_t(random());
        FirmwareView view; SoCVersion version;
        parseFirmware(bytes.data(),size,FirmwareType::Patch,view);
        parseFirmware(bytes.data(),size,FirmwareType::NVM,view);
        parseVersionEvent(bytes.data(),size,version);
        validateDownloadEvent(bytes.data(),size);
        validPacket(bytes.data(),size,false);
        H4Decoder decoder; decoder.reset(true);
        const size_t split = size / 2;
        decoder.feed(bytes.data(),split,sink);
        decoder.feed(bytes.data() + split,size - split,sink);
    }
}
int main() { boot(); firmware(); malformedInputs(); puts("Bluetooth boot pulses, firmware bounds/segments and 10000 malformed inputs passed"); }
