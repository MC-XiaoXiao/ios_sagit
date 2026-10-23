// SPDX-License-Identifier: GPL-2.0-only
#include "../drivers/clock/QcomOSM/OSMInitialization.h"
#include "../drivers/clock/QcomOSM/CPRhInitialization.h"
#include "../drivers/clock/QcomOSM/OSMHandoff.h"
#include "../drivers/clock/QcomOSM/OSMController.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <vector>
using namespace QcomOSMInit;

struct Model {
    struct Event { unsigned region; uint32_t offset, value; bool secure; };
    std::map<uint64_t, uint32_t> memory;
    std::map<uint32_t, uint32_t> secureMemory;
    std::vector<Event> events;
    unsigned polls = 0, delay = 0, barriers = 0;
    unsigned cprIdleCountdown[2] = {};
    unsigned failedRegion = 99, failACDRegion = 99;
    uint32_t failedOffset = 0, denySecureRead = 0, denySecureWrite = 0;
    bool dropWrite = false, badEnableReadback = false, dropSecureWrite = false;
    // Independent model of the unconditional OSM entries in the stock
    // monitor's 40-address table. Unlisted IO returns success but no data.
    static bool monitorAllows(uint32_t physical) {
        for (unsigned c = 0; c < 2; ++c)
            for (unsigned n : {15U, 31U, 32U, 57U, 58U, 72U, 76U, 88U})
                if (physical == 0x179c0300U + c * 0x2000U + n * 4U) return true;
        return false;
    }
    uint32_t &at(unsigned region, uint32_t offset) {
        assert(region < 6 && !(offset & 3));
        assert(offset < (region < 2 ? 0x2000U : region < 4 ? 0x800U : 0x4000U));
        return memory[(uint64_t(region) << 32) | offset];
    }
    Model() {
        for (unsigned c = 0; c < 2; ++c) {
            // Original v2 boot table, not the v1 Linux no-TZ setup.
            const struct { unsigned n; uint32_t value; } parameters[] = {
                {15, 0}, {31, 0}, {32, 0}, {88, 0},
                {57, c ? 0x0bU : 0x10U}, {58, c ? 0x0cU : 0x11U},
                {72, c ? 0x19U : 0x16U}, {76, c ? 0x5b9U : 0x4b9U},
                {55, 0x2c5ffd}, {56, 0x2f5ffd}
            };
            for (const auto &p : parameters) secureMemory[osmBase(c) + seq(p.n)] = p.value;
            at(c, 0x1070) = 0x40000000;
            at(c, 0x1034) = 0x80000003;
            at(c, 0x10a4) = 0x100;
        }
    }
    static bool read(void *context, unsigned region, uint32_t offset, uint32_t &value) {
        auto &m = *static_cast<Model *>(context);
        if (region >= 2 && region < 4 && (offset == 0x8c || offset == 0x94)) ++m.polls;
        if (region >= 4 && offset == 8 && !(m.at(region, 4) & 1) &&
            m.cprIdleCountdown[region - 4] && !--m.cprIdleCountdown[region - 4])
            m.at(region, offset) = 0;
        value = m.at(region, offset);
        return true;
    }
    static bool write(void *context, unsigned region, uint32_t offset, uint32_t value) {
        auto &m = *static_cast<Model *>(context);
        m.events.push_back({region, offset, value, false});
        if (m.failedRegion == region && m.failedOffset == offset) {
            if (m.dropWrite) return true;
            return false;
        }
        m.at(region, offset) = value;
        if (m.badEnableReadback && region < 2 && offset == 0x1004) m.at(region, offset) = 3;
        if (region >= 2 && region < 4 && region != m.failACDRegion) {
            if (offset == 0x84 && value == 1) m.at(region, 0x8c) = 1;
            if (offset == 0x90 && (value & 1)) m.at(region, 0x94) = 1U << (value >> 1);
        }
        return true;
    }
    static bool secureRead(void *context, uint32_t physical, uint32_t &value) {
        auto &m = *static_cast<Model *>(context);
        assert(permittedSecureRead(physical));
        if (physical == m.denySecureRead) return false;
        value = monitorAllows(physical) ? m.secureMemory[physical] : 0;
        return true;
    }
    static bool secureWrite(void *context, uint32_t physical, uint32_t value) {
        auto &m = *static_cast<Model *>(context);
        assert(permittedSecureWrite(physical));
        m.events.push_back({(physical - osmBase(0)) / 0x2000, physical, value, true});
        if (physical == m.denySecureWrite) return false;
        if (monitorAllows(physical) && !m.dropSecureWrite) m.secureMemory[physical] = value;
        return true;
    }
    static void barrier(void *context) { ++static_cast<Model *>(context)->barriers; }
    static void wait(void *context, unsigned us) {
        assert(us > 0 && us <= 5);
        static_cast<Model *>(context)->delay += us;
    }
    IO io() { return {this, read, write, secureRead, secureWrite, barrier, wait}; }
    static bool cprRead(void *context, unsigned cluster, uint32_t offset, uint32_t &value) {
        return read(context, cluster + 4, offset, value);
    }
    static bool cprWrite(void *context, unsigned cluster, uint32_t offset, uint32_t value) {
        return write(context, cluster + 4, offset, value);
    }
    QcomCPRh::RegisterIO cprIO() { return {this, cprRead, cprWrite, barrier, wait}; }
    static uint32_t runtimeRead(void *context, unsigned bank, uint32_t offset) {
        assert(bank == 0);
        return static_cast<Model *>(context)->at(offset / 0x2000, offset % 0x2000);
    }
    static void runtimeWrite(void *context, unsigned bank, uint32_t offset, uint32_t value) {
        assert(bank == 0);
        assert(write(context, offset / 0x2000, offset % 0x2000, value));
    }
    QcomPower::IO runtimeIO() { return {this, runtimeRead, runtimeWrite, wait, nullptr}; }
    bool enabled() const {
        for (const auto &event : events)
            if (!event.secure && event.region < 2 && event.offset == 0x1004) return true;
        return false;
    }
};
static const QcomCPRh::Identity identity{292, 0x20000, 30};
static const Clocks clocks{300000000, 200000000, 19200000, true};
static void plans(QcomCPRh::Plan (&out)[2]) {
    for (unsigned c = 0; c < 2; ++c) {
        auto &p = out[c];
        p = {};
        p.identity = identity; p.cluster = c; p.activeCount = 6; p.count = 8; p.osmCount = 6;
        p.apmThresholdCorner = 3; p.memAccThresholdCorner = 4;
        p.apmCrossoverCorner = 6; p.memAccCrossoverCorner = 7;
        const uint32_t lvalues[] = {15, 18, 22, 26, 30, 34};
        for (unsigned i = 0; i < 6; ++i) {
            const uint32_t hz = i ? lvalues[i] * 19200000 : 300000000;
            const uint32_t mv = 700 + 40 * i;
            p.corners[i].frequencyHz = hz; p.corners[i].ceilingUV = mv * 1000;
            p.osm[i] = {hz, (i ? 0x05040000U : 0x00040000U) | lvalues[i],
                (i << 16) | mv, 0x01200020, 1 + i / 2};
        }
        for (unsigned i = 6; i < 40; ++i) p.osm[i] = p.osm[5];
    }
}
int main() {
    QcomCPRh::Plan p[2]; plans(p);
    for (unsigned c = 0; c < 2; ++c) assert(Engine::validPlan(p[c], c));
    {
        Model m; Engine e; Result r;
        assert(e.inspect(m.io(), identity, clocks, p, r) && m.events.empty());
        assert(r.firmwareChecks[0] == 0xff && r.firmwareChecks[1] == 0xff);
        assert(!e.enable() && r.error == Error::NotPrepared && m.events.empty());
        assert(e.prepare(m.io(), identity, clocks, p, r));
        assert(r.stage == Stage::Prepared && !m.enabled());
        assert(m.at(0, 0x1150 + 39 * 32) == 39);
        assert(m.at(0, 0x1158 + 39 * 32) == p[0].osm[5].voltage);
        assert(m.at(1, 0x1070) == 0x4000003f && m.at(1, 0x1034) == 0x80000003);
        assert(m.at(0, 0x1074) == 0x03e807d0 && m.at(0, 0x1088) == 0x07d007d0);
        assert(m.at(0, 0x101c) == 0x00c800c8 && m.at(0, 0x10a4) == 0x80020108);
        assert(m.at(0, 0x1f00) == 0x113);
        assert(m.secureMemory[osmBase(0) + seq(57)] == 3);
        assert(m.secureMemory[osmBase(0) + seq(58)] == 4);
        for (unsigned c = 0; c < 2; ++c) {
            assert(m.secureMemory[osmBase(c) + seq(55)] == 0x2c5ffd);
            assert(m.secureMemory[osmBase(c) + seq(56)] == 0x2f5ffd);
        }
        assert(m.secureMemory[osmBase(1) + seq(88)] == 7);
        assert(m.secureMemory[osmBase(0) + seq(32)] == 30);
        assert(m.secureMemory[osmBase(1) + seq(76)] == (0x39 | (3 << 6)));
        assert(m.at(2, 0x80) == 0x9406 && m.at(3, 0x30) == 0x2cf9afe);
        size_t firstACD = m.events.size(), lastSecure = 0;
        for (size_t i = 0; i < m.events.size(); ++i) {
            if (m.events[i].secure) lastSecure = i;
            else if (m.events[i].region >= 2 && firstACD == m.events.size()) firstACD = i;
        }
        assert(lastSecure < firstACD);
        const size_t n = m.events.size();
        assert(e.enable() && r.stage == Stage::Complete && r.enabledMask == 3);
        assert(m.events.size() == n + 4);
        assert(m.events[n].offset == 0x1f10 && m.events[n + 1].offset == 0x1f10);
        assert(m.events[n + 2].offset == 0x1004 && m.events[n + 3].offset == 0x1004);
        assert(!e.enable() && r.error == Error::NotPrepared);
    }
    // Preserve every inherited LLM policy, including enabled limit votes.
    for (uint32_t llm=0;llm<4;++llm) {
        Model m; Engine e; Result r;
        m.at(0,0x1034)=0x80000000|llm; m.at(1,0x1034)=llm;
        assert(e.prepare(m.io(),identity,clocks,p,r));
        assert(m.at(0,0x1034)==(0x80000000|llm) && m.at(1,0x1034)==llm);
    }
    // Every admission failure must precede writes to either cluster.
    for (unsigned test = 0; test < 7; ++test) {
        Model m; Engine e; Result r; auto id = identity; auto ck = clocks; plans(p);
        if (test == 0) id.chipVersion = 0x10000;
        if (test == 1) ck.retained = false;
        if (test == 2) ck.osmHz = 192000000;
        if (test == 3) p[1].osm[39].voltage ^= 1;
        if (test == 4) m.at(1, 0x1004) = 1;
        if (test == 5) m.secureMemory[osmBase(1) + seq(72)] = 0;
        if (test == 6) m.denySecureRead = osmBase(1) + seq(15);
        assert(!e.prepare(m.io(), id, ck, p, r) && m.events.empty());
    }
    plans(p);
    for (unsigned threshold : {0U, 1U, 2U, 39U}) {
        Model m; Engine e; Result r;
        for (auto &plan : p) plan.memAccThresholdCorner = threshold;
        assert(e.prepare(m.io(), identity, clocks, p, r));
        for (unsigned c = 0; c < 2; ++c) {
            assert(m.secureMemory[osmBase(c) + seq(57)] == (threshold ? threshold - 1 : 0xff));
            assert(m.secureMemory[osmBase(c) + seq(58)] == threshold);
            assert(m.secureMemory[osmBase(c) + seq(55)] == 0x2c5ffd);
            assert(m.secureMemory[osmBase(c) + seq(56)] == 0x2f5ffd);
        }
    }
    plans(p);
    for (unsigned c = 0; c < 2; ++c)
        for (unsigned n : {15U, 31U, 32U, 57U, 58U, 72U, 76U, 88U}) {
            Model m; Engine e; Result r;
            m.secureMemory[osmBase(c) + seq(n)] ^= 1;
            assert(!e.inspect(m.io(), identity, clocks, p, r) && m.events.empty());
            assert(r.error == Error::TZNotReady && r.cluster == c && r.offset == seq(n));
        }
    {
        Model m; Engine e; Result r;
        m.secureMemory.clear(); // Denied/uninitialized reads must not pass.
        assert(!e.inspect(m.io(), identity, clocks, p, r) && m.events.empty());
        assert(r.error == Error::TZNotReady && r.offset == seq(57));
    }
    {
        Model m; Engine e; Result r; m.dropSecureWrite = true;
        assert(!e.prepare(m.io(), identity, clocks, p, r));
        assert(r.error == Error::ReadbackFailed && r.offset == seq(88) && !m.enabled());
    }
    {
        Model m; Engine e; Result r;
        m.denySecureWrite = osmBase(1) + seq(72);
        assert(!e.prepare(m.io(), identity, clocks, p, r));
        assert(r.error == Error::SecureWriteFailed && !m.enabled());
        for (const auto &event : m.events) assert(event.secure || event.region < 2);
    }
    {
        Model m; Engine e; Result r; m.failACDRegion = PerformanceACD;
        assert(!e.prepare(m.io(), identity, clocks, p, r));
        assert(r.error == Error::ACDTimeout && !m.enabled() && m.polls <= 25 && m.delay <= 25);
        assert(!e.enable());
    }
    {
        Model m; Engine e; Result r;
        m.failedRegion = PowerOSM; m.failedOffset = 0x1154; m.dropWrite = true;
        assert(!e.prepare(m.io(), identity, clocks, p, r));
        assert(r.error == Error::ReadbackFailed && !m.enabled());
    }
    {
        Model m; Engine e; Result r;
        assert(e.prepare(m.io(), identity, clocks, p, r));
        m.failedRegion = PerformanceOSM; m.failedOffset = 0x1004;
        assert(!e.enable() && r.enabledMask == 1 && m.at(0, 0x1004) == 1);
        assert(!m.events.empty() && m.events.back().value == 1);
    }
    {
        Model m; Engine e; Result r;
        assert(e.prepare(m.io(), identity, clocks, p, r));
        m.badEnableReadback = true;
        assert(!e.enable() && r.error == Error::ReadbackFailed && r.observed == 3);
        assert(r.enabledMask == 1 && !m.at(1, 0x1004));
    }
    for (uint32_t address = osmBase(0); address < osmBase(0) + 0x4000; ++address) {
        assert(permittedSecureWrite(address) == Model::monitorAllows(address));
        assert(permittedSecureRead(address) == Model::monitorAllows(address));
    }
    assert(!allowedSecureAddress(0x17916004, true));
    assert(!allowedSecureAddress(osmBase(0) + 0x500, true));
    assert(!allowedSecureAddress(osmBase(0) + seq(49), true));
    assert(!allowedSecureAddress(osmBase(0) + seq(0), false));
    assert(!allowedSecureAddress(osmBase(0) + seq(55), true));
    assert(!allowedSecureAddress(osmBase(1) + seq(56), true));
    assert(!allowedSecureAddress(0xffffffffU, false));
    // Integrate the production calibration output, including interleaved
    // single-core boost rows, all supported fuse bins/revisions and v2.1.
    // This is deliberately synthetic efuse data, not a device measurement.
    for (unsigned version = 0; version < 2; ++version)
        for (unsigned bin = 0; bin < 4; ++bin)
            for (unsigned revision = 0; revision < 8; ++revision) {
                QcomCPRh::FuseRows fuse{};
                fuse.row38 = uint64_t(bin) << 29;
                fuse.row39 = uint64_t(revision) << 51;
                fuse.row67 = (uint64_t(2200) << 46) | (uint64_t(1900 & 63) << 58);
                fuse.row68 = (uint64_t(1000) << 18) | (uint64_t(1300) << 6) | (1900 >> 6);
                fuse.row69 = uint64_t(2200 & 15) << 60;
                fuse.row70 = (uint64_t(1000) << 32) | (uint64_t(1300) << 20) |
                    (uint64_t(1900) << 8) | (2200 >> 4);
                auto id = identity; id.chipVersion += version;
                for (unsigned cluster = 0; cluster < 2; ++cluster) {
                    assert(QcomCPRh::calibrate(id, fuse, cluster, p[cluster]));
                    assert(Engine::validPlan(p[cluster], cluster));
                }
                Model m; Engine e; Result r;
                const bool takeover = version == 0 && bin == 0 && revision == 0;
                if (takeover) for (unsigned c = 0; c < 2; ++c) {
                    // Firmware's CPR loop is running with unmatched settings.
                    // BUSY is retained until a later triggered measurement.
                    m.at(4 + c, 4) = 1; m.at(4 + c, 8) = 1;
                    m.cprIdleCountdown[c] = 2;
                    assert(!QcomCPRh::inspect(p[c], m.cprIO()));
                }
                assert(e.inspect(m.io(), id, clocks, p, r));
                const QcomCPRh::Ownership ownership{true, true, true, true};
                assert(QcomCPRh::inspect(p[0], m.cprIO(), ownership) &&
                    QcomCPRh::inspect(p[1], m.cprIO(), ownership));
                assert(m.events.empty());
                assert(QcomCPRh::apply(p[0], m.cprIO(), ownership) &&
                    QcomCPRh::apply(p[1], m.cprIO(), ownership));
                if (takeover) {
                    assert(m.delay == 0);
                    assert(m.events[0].region == 4 && m.events[0].offset == 4 && m.events[0].value == 0x21e);
                }
                assert(QcomCPRh::verify(p[0], m.cprIO()) && QcomCPRh::verify(p[1], m.cprIO()));
                const size_t cprEnd = m.events.size();
                assert(cprEnd > 0 && m.events.back().region == 5 &&
                    m.events.back().offset == 4 && (m.events.back().value & 1));
                for (const auto &event : m.events) assert(event.region >= 4 && !event.secure);
                assert(e.prepare(m.io(), id, clocks, p, r));
                assert(m.events[cprEnd].region == 0 && m.events[cprEnd].offset == 0x1150);
                assert(e.enable() && r.enabledMask == 3);
                assert(QcomCPRh::verify(p[0], m.cprIO()) && QcomCPRh::verify(p[1], m.cprIO()));
                QcomPower::OSMController runtime;
                assert(runtime.initialize(m.runtimeIO()) == QcomPower::OK);
                assert(runtime.rate(0) == 300000000 && runtime.rate(1) == 300000000);
                for (unsigned c=0;c<2;++c) {
                    assert(runtime.setRate(c,QcomOSMClock::BootHz[c])==QcomPower::OK);
                    assert(runtime.rate(c)==QcomOSMClock::BootHz[c]);
                }
                QcomOSMHandoff::Record handoff{};
                handoff.bytes = sizeof(handoff); handoff.chipID = id.chipID;
                handoff.chipVersion = id.chipVersion; handoff.boardID = id.boardID;
                handoff.flags = QcomOSMHandoff::Required;
                handoff.firmwareProfile = QcomOSMTZ::Profile;
                handoff.auxHz = clocks.auxiliaryHz; handoff.osmHz = clocks.osmHz; handoff.xoHz = clocks.xoHz;
                auto read = [&](unsigned cluster, uint32_t off) { return m.at(cluster, off); };
                for (unsigned cluster = 0; cluster < 2; ++cluster) {
                    handoff.firmwareChecks[cluster] = r.firmwareChecks[cluster];
                    handoff.lutDigest[cluster] = QcomOSMHandoff::lutDigest(read, cluster);
                    handoff.enable[cluster] = m.at(cluster, 0x1004);
                    handoff.initialDesired[cluster] = m.at(cluster, 0x1f10);
                }
                assert(QcomOSMHandoff::validate(handoff, read) == QcomOSMHandoff::Status::Uncommitted);
                handoff.version = QcomOSMHandoff::Version;
                assert(QcomOSMHandoff::validate(handoff, read) == QcomOSMHandoff::Status::Accepted);
                assert(runtime.setRate(0, runtime.state(0, 1, false)) == QcomPower::OK);
                assert(runtime.setRate(1, runtime.state(1, 1, false)) == QcomPower::OK);
            }
    // A second-cluster CPR failure must stop the caller before preparing OSM;
    // the DT record remains uncommitted and no OSM/ACD write is issued.
    {
        Model m; Engine e; Result r;
        auto id = p[0].identity;
        assert(e.inspect(m.io(), id, clocks, p, r));
        assert(QcomCPRh::inspect(p[0], m.cprIO()) && QcomCPRh::inspect(p[1], m.cprIO()));
        m.failedRegion = 5; m.failedOffset = 0x3a00;
        assert(QcomCPRh::apply(p[0], m.cprIO()) && !QcomCPRh::apply(p[1], m.cprIO()));
        for (const auto &event : m.events) assert(event.region >= 4 && !event.secure);
        assert(!e.enable() && !m.enabled());
        QcomOSMHandoff::Record handoff{};
        assert(!handoff.version);
    }
    std::puts("OSM initialization: admission, TZ parameters, LUT/FSM/SCM/ACD, failure and enable ordering passed");
}
