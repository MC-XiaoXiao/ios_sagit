/* Register model exercises the production distributor code. */
#include "GICv3.h"
#include <array>
#include <vector>
#include <cstring>
#include <cassert>
#include <cstdio>

struct Model {
	std::array<uint8_t, 0x10000> bytes{};
	std::vector<uint32_t> writes;
	unsigned rwp = 0;
	unsigned polls = 0;
	bool secure = false;
	uint8_t read8(uint32_t offset) { return bytes.at(offset); }
	uint32_t read32(uint32_t offset) {
		if (offset == GICv3::kCTLR) {
			polls++;
			if (rwp) { rwp--; return GICv3::kRWP; }
		}
		uint32_t value; std::memcpy(&value, bytes.data() + offset, 4); return value;
	}
	uint64_t read64(uint32_t offset) {
		uint64_t value; std::memcpy(&value, bytes.data() + offset, 8); return value;
	}
	void write8(uint32_t offset, uint8_t value) {
		writes.push_back(offset);
		if (!secure) bytes.at(offset) = value;
	}
	void write32(uint32_t offset, uint32_t value) {
		writes.push_back(offset);
		if (secure) return;
		if (offset >= GICv3::kICENABLER && offset < GICv3::kICENABLER + 128) {
			uint32_t enable = offset - GICv3::kICENABLER + GICv3::kISENABLER;
			uint32_t previous = read32(enable) & ~value;
			std::memcpy(bytes.data() + enable, &previous, 4);
		} else if (offset >= GICv3::kISENABLER && offset < GICv3::kISENABLER + 128) {
			value |= read32(offset);
		}
		std::memcpy(bytes.data() + offset, &value, 4);
	}
	void write64(uint32_t offset, uint64_t value) {
		writes.push_back(offset);
		if (!secure) std::memcpy(bytes.data() + offset, &value, 8);
	}
	void relax() {}
};

int main()
{
	using namespace GICv3;
	static_assert(sizeof(uint32_t) == 4);
	assert(vectorCount(0) == 32 && vectorCount(1) == 64 && vectorCount(31) == 1020);
	uint32_t id, type;
	uint32_t cells[] = {146, 1};
	assert(decode(cells, 8, 1020, id, type) && id == 146 && type == 1);
	cells[1] = 0;
	assert(decode(cells, 8, 1020, id, type) && type == 0);
	cells[1] = 4; // Linux IRQ_TYPE_LEVEL_HIGH must not silently become an Apple type.
	assert(!decode(cells, 8, 1020, id, type));
	cells[1] = 1;
	for (uint32_t invalid : {0U, 16U, 27U, 31U, 1020U, 1023U, 0xffffffffU}) {
		cells[0] = invalid; assert(!decode(cells, 8, 1020, id, type));
	}
	cells[0] = 64;
	assert(!decode(cells, 8, 64, id, type));
	assert(!decode(nullptr, 8, 1020, id, type));
	for (size_t size : {0U, 4U, 7U, 9U, 12U}) assert(!decode(cells, size, 1020, id, type));
	uint8_t unaligned[9]; std::memcpy(unaligned + 1, cells, 8);
	assert(decode(unaligned + 1, 8, 1020, id, type) && id == 64);

	// Both SDM835 clusters and nonzero Aff2/Aff3 are encoded independently.
	uint64_t value;
	for (unsigned cluster = 0; cluster < 2; cluster++) {
		for (unsigned core = 0; core < 4; core++) {
			assert(sgi((cluster << 8) | core, 0, value));
			assert(value == (uint64_t(cluster) << 16 | 1ULL << core));
		}
	}
	assert(sgi(0x1200345603ULL, 7, value));
	assert(value == (0x12ULL << 48 | 0x34ULL << 32 | 7ULL << 24 | 0x56ULL << 16 | 8));
	assert(!sgi(16, 0, value) && !sgi(0, 16, value));
	assert(affinity(0x1280345603ULL) == 0x12345603);
	assert(route(0x1280345603ULL) == 0x1200345603ULL);

	Model bus;
	// INTID 146 is in a nonzero enable bank/configuration word. Preserve neighbors.
	bus.write32(kISENABLER + bank(146), 0xffffffff);
	bus.write32(kICFGR + (146 / 16) * 4, 0xaaaaaaaa);
	bus.writes.clear(); bus.rwp = 3;
	assert(configure(bus, 146, 1, 0x103));
	assert(bus.writes.front() == kICENABLER + bank(146));
	assert(bus.read32(kISENABLER + bank(146)) == (0xffffffffU & ~bit(146)));
	assert(bus.read32(kICFGR + (146 / 16) * 4) == (0xaaaaaaaa & ~(2U << 4)));
	assert(bus.read64(0x6000 + 146 * 8) == 0x103); // Architectural IROUTER<INTID>.
	assert(bus.read8(kIPRIORITYR + 146) == kPriority);
	assert(bus.polls >= 4);
	assert(configure(bus, 146, 0, 0));
	assert((bus.read32(kICFGR + (146 / 16) * 4) & (2U << 4)) != 0);
	for (uint32_t address : bus.writes) {
		assert(address != kCTLR); // No distributor-wide reset/toggle.
		assert(address < 0x80 || address >= 0x100); // No IGROUPR writes.
		assert(address < 0xd00 || address >= 0xd80); // No IGRPMODR writes.
	}
	// Secure RAZ/WI and a wedged RWP must fail, without enabling the SPI.
	Model secure; secure.secure = true;
	assert(!configure(secure, 146, 1, 0));
	Model wedged; wedged.rwp = kPollLimit + 1;
	assert(!configure(wedged, 146, 1, 0));
	assert(wedged.polls == kPollLimit && wedged.writes.size() == 1);
	// Exercise the full implemented SPI range, including the final bank.
	for (unsigned irq = 32; irq < 1020; irq++) {
		assert(configure(bus, irq, irq & 1, 0x102));
		assert(bus.read64(kIROUTER + irq * 8) == 0x102);
	}
	std::puts("GICv3: Apple specifiers, SPI configuration, routing, security and RWP passed");
}
