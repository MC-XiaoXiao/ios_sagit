// SPDX-License-Identifier: GPL-2.0-only
#include "BasebandAdmission.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
using namespace Baseband;
struct Board {
    std::vector<std::string> trace;
    bool held = false, mapped = false, signals = false, pinned = false, unknown = false;
    bool failClose = false, failRelease = false;
    unsigned calls = 0, fail = 0, confirmations = 0;
    Result rejection = Result::Unsupported;
    Result step(const char *name) { trace.emplace_back(name); return ++calls == fail ? rejection : Result::OK; }
    Result pin() { pinned = true; return Result::Quarantined; }
    bool hasAmbiguity() { return unknown; }
    Result acquireResources() { const auto r = step("resources"); if (r == Result::OK) held = true; return r; }
    Result probeSecure() { assert(held && !mapped); return step("probe"); }
    Result confirmHost(const Region &r) {
        assert(held && !mapped && !signals);
        const uint64_t addresses[] = {0x8d000000,0x94500000,0xb0000000};
        assert(confirmations < 3 && r.address == addresses[confirmations]);
        const auto result = step("confirm"); if (result == Result::OK) ++confirmations; return result;
    }
    Result mapConfirmedMemory() { assert(held && confirmations == 3); const auto r = step("map"); mapped = r == Result::OK; return r; }
    Result initializeSignals() { assert(held && mapped); signals = true; return step("signals"); }
    void closeSignals() { assert(held); signals = false; trace.emplace_back("close-signals"); }
    bool closeMemory() { assert(held && !signals); trace.emplace_back("close-memory"); if (failClose) return false; mapped = false; return true; }
    Result releaseResources() { assert(held && !mapped && !signals); trace.emplace_back("release"); if (failRelease) return Result::IO; held = false; return Result::OK; }
    void disconnectRoutes() { trace.emplace_back("routes-off"); }
};
int main() {
    BootLayout layout;layout.metadata={0xb0000000,0x4000};
    Board io;Admission<Board> a(io);
    assert(a.acquire(layout)==Result::OK && a.held() && io.mapped && io.signals && io.confirmations==3);
    assert(a.acquire(layout)==Result::Busy && io.calls==7);
    assert(a.release()==Result::OK && !a.held() && !io.held && !io.mapped && !io.signals);
    assert(io.trace[io.trace.size()-3]=="close-signals" && io.trace.back()=="release");
    for(unsigned step=1;step<=7;++step) {
        Board failed;failed.fail=step;Admission<Board> b(failed);
        assert(b.acquire(layout)==Result::Unsupported && !b.held() && !b.quarantined() && !failed.held);
        assert(!failed.mapped && !failed.signals);
        if(step<=5)for(const auto &call:failed.trace)assert(call!="map" && call!="signals");
        Board uncertain;uncertain.fail=step;uncertain.rejection=Result::Quarantined;
        Admission<Board> u(uncertain);
        assert(u.acquire(layout)==Result::Quarantined && u.quarantined() && uncertain.pinned);
        const auto size=uncertain.trace.size();
        assert(u.acquire(layout)==Result::Quarantined && u.release()==Result::Quarantined && uncertain.trace.size()==size);
        if(step>1)assert(uncertain.held); // No clock release or map destruction after unknown completion.
    }
    Board close;Admission<Board> c(close);assert(c.acquire(layout)==Result::OK);close.failClose=true;
    assert(c.release()==Result::Quarantined && close.held && close.mapped && c.held());
    Board release;Admission<Board> r(release);assert(r.acquire(layout)==Result::OK);release.failRelease=true;
    assert(r.release()==Result::Quarantined && release.held && r.held());
    Board invalid;Admission<Board> v(invalid);
    assert(v.acquire(BootLayout{})==Result::Invalid && invalid.trace.empty());
    std::puts("baseband native admission/three SCM confirmations/map ordering/rollback checks passed");
}
