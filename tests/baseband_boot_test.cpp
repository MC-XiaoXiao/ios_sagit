// SPDX-License-Identifier: GPL-2.0-only
#include "BasebandBoot.h"
#include "BasebandSecure.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
using namespace Baseband;
struct BootRegisters {
    uint32_t regs[2][1024]{};
    uint32_t codeBytes = 0;
    uint64_t time = 0;
    bool rejectAuth = false;
    BootRegisters() { regs[1][1] = 1; regs[1][3] = 2; }
    bool read(unsigned space, uint32_t at, uint32_t &v) { v = regs[space][at / 4]; return true; }
    bool write(unsigned space, uint32_t at, uint32_t v) {
        regs[space][at / 4] = v;
        if (space == 1 && at == Q6::Command && v == 1) regs[1][3] = rejectAuth ? uint32_t(-3) : 3;
        if (space == 1 && at == Q6::CodeLength && v == codeBytes) regs[1][3] = 4;
        return true;
    }
    uint64_t nowUS() { return time; }
    void delayUS(unsigned n) { time += n; }
};
struct Board {
    BootLayout layout;
    Ownership owners[3]{Ownership::Host, Ownership::Host, Ownership::Host};
    bool cold = true, reserved = true, acquired = false, halted = true, released = false;
    bool failHalt = false, failCopy = false, ambiguous = false;
    unsigned assignments = 0, failAssignment = 0, writes = 0;
    std::vector<std::string> trace;
    Result admit(const BootLayout &m) {
        if (!cold || !reserved) return Result::NotReady;
        layout = m; return Result::OK;
    }
    Result acquire() { trace.push_back("acquire"); acquired = true; return Result::OK; }
    int index(uint64_t a, size_t n) {
        const Region regions[] = {layout.modem, layout.mba, layout.metadata};
        for (unsigned i = 0; i < 3; ++i)
            if (a >= regions[i].address && range(a - regions[i].address, n, regions[i].bytes)) return int(i);
        return -1;
    }
    Result assign(const Region &m, Ownership from, Ownership to) {
        const int i = index(m.address, m.bytes); assert(i >= 0 && owners[i] == from);
        trace.push_back("assign" + std::to_string(i));
        if (++assignments == failAssignment) return ambiguous ? Result::Quarantined : Result::Authentication;
        owners[i] = to; return Result::OK;
    }
    bool copy(uint64_t a, const uint8_t *, size_t n) {
        const int i = index(a, n); assert(i >= 0 && owners[i] != Ownership::Modem);
        ++writes; trace.push_back("copy" + std::to_string(i)); return !failCopy;
    }
    bool zero(uint64_t a, size_t n) { return !n || copy(a, nullptr, n); }
    bool publish(uint64_t a, size_t n) { assert(index(a, n) >= 0); return true; }
    Result deassertReset() { halted = false; trace.push_back("reset-off"); return Result::OK; }
    Result stageMetadata(const FirmwareImage &image, const Region &m) {
        assert(owners[2] == Ownership::Host);
        std::vector<uint8_t> metadata(image.metadataBytes());
        return image.metadata(metadata.data(), metadata.size()) == Result::OK &&
            copy(m.address, metadata.data(), metadata.size()) ? Result::OK : Result::IO;
    }
    Result waitReady(uint64_t epoch, unsigned timeout) {
        assert(epoch && timeout == 5000000 && owners[0] == Ownership::Modem);
        trace.push_back("ready"); return Result::OK;
    }
    Result requestStop(uint64_t, unsigned) { trace.push_back("stop"); return Result::Timeout; }
    Result quiesceAndReset() {
        trace.push_back("halt-reset");
        if (failHalt) return Result::Timeout;
        halted = true; return Result::OK;
    }
    Result release() {
        assert(halted);
        for (auto owner : owners) assert(owner == Ownership::Host);
        trace.push_back("release"); released = true; acquired = false; return Result::OK;
    }
};
struct SecureBackend {
    uint8_t scratch[256]{};
    std::vector<Secure::Registers> calls;
    std::vector<int32_t> returns;
    unsigned index = 0, sleeps = 0, publications = 0;
    void smc(Secure::Registers &r) {
        calls.push_back(r);
        const auto code = index < returns.size() ? returns[index++] : 0;
        r.x[0] = uint32_t(code); r.x[1] = code ? 0 : 1; r.x[6] = 0xabcdef;
        if ((calls.back().x[0] & 0xffff) == 0xc16 || calls.back().x[1] == 0x1117) r.x[1] = 0;
    }
    void delayMS(unsigned ms) { assert(ms == 30); ++sleeps; }
    bool publish(const void *p, uint64_t a, size_t n) {
        assert(p == scratch && a == 0xa0010000 && n == sizeof(scratch)); ++publications; return true;
    }
};
static void assignmentChecks() {
    SecureBackend io;
    Secure::Assignment<SecureBackend> assign(io, io.scratch, 0xa0010000);
    const Region region{0x8d000000, 0x7000000};
    assert(assign.change(region, Ownership::Host, Ownership::Modem) == Result::NotReady);
    assert(assign.probe() == Result::OK && assign.is64());
    const auto before=io.calls.size();
    assert(assign.change(region,Ownership::Host,Ownership::Host,true)==Result::OK);
    assert(io.calls.size()==before+1 && io.calls.back().x[1]==0x1117 && read32(io.scratch)==3);
    assert(read32(io.scratch+128)==3 && read32(io.scratch+132)==7);
    assert(assign.change(region,Ownership::Modem,Ownership::Modem,true)==Result::Invalid);
    const auto publications=io.publications;
    // SMC busy retries a fresh transaction; an interruption resumes x0=1
    // with the original buffer arguments and the secure A6 cookie.
    io.returns = {-12, 1, 0}; io.index = 0;
    assert(assign.change(region, Ownership::Host, Ownership::Shared) == Result::OK);
    assert(io.sleeps == 1 && io.publications == publications+1);
    assert(read32(io.scratch) == 3 && read32(io.scratch + 128) == 3);
    assert(read32(io.scratch + 132) == 7 && read32(io.scratch + 152) == 15 && read32(io.scratch + 156) == 6);
    const auto &initial = io.calls[io.calls.size() - 2], &resume = io.calls.back();
    assert(initial.x[0] == 0x42000c16 && initial.x[1] == 0x1117);
    assert(initial.x[2] == 0xa0010040 && initial.x[3] == 16 && initial.x[4] == 0xa0010000 && initial.x[5] == 0xa00100c0);
    assert(resume.x[0] == 1 && resume.x[6] == 0xabcdef && resume.x[5] == initial.x[5]);
    assert(read32(io.scratch + 192) == 4 && read32(io.scratch + 208) == 48);
    const Region rmtfs{0x88f00000,0x200000};
    assert(assign.change(rmtfs,Ownership::Host,Ownership::Shared,false,6)==Result::OK);
    assert(read32(io.scratch+132)==6 && read32(io.scratch+156)==6);
    assert(assign.change(rmtfs,Ownership::Shared,Ownership::Host,false,6)==Result::OK);
    assert(read32(io.scratch)==3 && read32(io.scratch+4)==15 && read32(io.scratch+132)==6);
    assert(assign.change(rmtfs,Ownership::Host,Ownership::Host,true,6)==Result::OK && read32(io.scratch+132)==6);
    assert(assign.change(rmtfs,Ownership::Host,Ownership::Shared,false,5)==Result::Invalid);
    assert(assign.change({0xa0010000, 4096}, Ownership::Host, Ownership::Modem) == Result::Invalid);
    io.returns = {2}; io.index = 0;
    assert(assign.change(region, Ownership::Shared, Ownership::Host) == Result::Quarantined);
    const auto calls = io.calls.size();
    assert(assign.change(region, Ownership::Shared, Ownership::Host) == Result::Quarantined && io.calls.size() == calls);
    SecureBackend fallback; fallback.returns = {-1, 0, 0};
    Secure::Assignment<SecureBackend> smc32(fallback, fallback.scratch, 0xa0010000);
    assert(smc32.probe() == Result::OK && !smc32.is64());
    assert(smc32.change(region, Ownership::Modem, Ownership::Host) == Result::OK);
    assert(fallback.calls.back().x[0] == 0x02000c16);
    assert(read32(fallback.scratch) == 15 && read32(fallback.scratch + 192) == 4);
    assert(read32(fallback.scratch + 196) == 0xa0010080 && read32(fallback.scratch + 200) == 24);
}
static std::vector<uint8_t> load(const char *name) {
    std::ifstream input(name, std::ios::binary); assert(input);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(input)), {});
}
int main() {
    assignmentChecks();
    const auto mbaBytes = load("firmware/sagit-baseband/mba.mbn");
    const auto modemBytes = load("firmware/sagit-baseband/modem.mbn");
    FirmwareImage mba, modem;
    assert(mba.parse(mbaBytes.data(), mbaBytes.size()) == Result::OK);
    assert(modem.parse(modemBytes.data(), modemBytes.size()) == Result::OK);
    BootLayout layout; assert(!layout.valid()); layout.metadata = {0xa0000000, 0x4000};
    assert(layout.valid());
    Board board; BootRegisters regs; regs.codeBytes = modem.codeLength();
    Boot<Board, BootRegisters> boot(board, regs);
    board.reserved = false;
    assert(boot.start(1, mba, modem, layout) == Result::NotReady && !board.writes);
    board.reserved = true;
    assert(boot.start(1, mba, modem, layout) == Result::OK);
    assert(boot.state() == State::Running && boot.owner(0) == Ownership::Modem);
    assert(boot.owner(1) == Ownership::Host && boot.owner(2) == Ownership::Host);
    assert(board.trace[1] == "copy1"); // Entire MBA first; MPSS still untouched.
    assert(regs.regs[1][Q6::CodeLength / 4] == modem.codeLength());
    assert(boot.stop(0) == Result::Stale);
    assert(boot.stop(1) == Result::OK && board.released && !boot.retainsResources());
    assert(boot.start(1, mba, modem, layout) == Result::Invalid);
    for (unsigned failure = 1; failure <= 8; ++failure) {
        Board rejected; BootRegisters modeled; modeled.codeBytes = modem.codeLength();
        rejected.failAssignment = failure;
        Boot<Board, BootRegisters> b(rejected, modeled);
        assert(b.start(1, mba, modem, layout) != Result::OK);
        assert(rejected.released && !b.quarantined());
    }
    Board pinned; BootRegisters bad; bad.rejectAuth = true;
    pinned.failHalt = true;
    Boot<Board, BootRegisters> stuck(pinned, bad);
    assert(stuck.start(1, mba, modem, layout) == Result::Quarantined);
    assert(!pinned.released && stuck.retainsResources());
    assert(stuck.owner(2)==Ownership::Modem); // Failed metadata auth must halt/reset before reclaim.
    assert(stuck.stop(1) == Result::Quarantined);
    Board unknown; BootRegisters uncertain;
    unknown.failAssignment = 1; unknown.ambiguous = true;
    Boot<Board, BootRegisters> unsafe(unknown, uncertain);
    assert(unsafe.start(1, mba, modem, layout) == Result::Quarantined);
    assert(unsafe.owner(0) == Ownership::Unknown && !unknown.released && unsafe.retainsResources());
    std::puts("baseband real-image boot/ownership/SCM failure checks passed (modeled hardware)");
}
