// SPDX-License-Identifier: BSD-2-Clause
#include "WatchdogHardware.h"
#include <cassert>
#include <utility>
#include <vector>
struct Registers {
    std::vector<std::pair<uint32_t,uint32_t>> writes;
    void write(uint32_t address, uint32_t value) { writes.emplace_back(address,value); }
};
int main() {
    Registers io;
    using namespace QcomWatchdogHardware;
    assert(!arm(io, 0) && !arm(io, 32) && !arm(io, 120) && io.writes.empty());
    assert(arm(io, 31) && io.writes.size() == 6);
    assert(io.writes[2].first == 0x10 && io.writes[2].second == 0xfffff);
    assert(io.writes[3].first == 0x14 && io.writes[3].second == Frequency*31);
    assert(io.writes[3].second < (1U<<20));
    io.writes.clear(); feed(io);
    assert(io.writes.size() == 1 && io.writes[0].first == 4 && io.writes[0].second == 1);
    io.writes.clear(); disable(io);
    assert(io.writes.size() == 1 && io.writes[0].first == 8 && io.writes[0].second == 0);
    io.writes.clear(); assert(arm(io,5) && io.writes[3].second == Frequency*5);
}
