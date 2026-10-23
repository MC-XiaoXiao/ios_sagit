/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef _GICV3_H
#define _GICV3_H

#include <stdint.h>
#include <stddef.h>

namespace GICv3 {
enum : uint32_t {
	kDistributorSize = 0x10000,
	kRedistributorStride = 0x20000,
	kFirstSPI = 32,
	kSpecialINTID = 1020,
	kCTLR = 0x0000,
	kTYPER = 0x0004,
	kISENABLER = 0x0100,
	kICENABLER = 0x0180,
	kISPENDR = 0x0200,
	kICPENDR = 0x0280,
	kIPRIORITYR = 0x0400,
	kICFGR = 0x0c00,
	kIROUTER = 0x6000,
	kPIDR2 = 0xffe8,
	kRWP = 1U << 31,
	/* GICD_CTLR's Non-secure view with DS=0 and ARE_NS=1. */
	kARE = 1U << 4,
	kEnableGroup1A = 1U << 1,
	kPriority = 0xa0,
	kPollLimit = 1000000
};

inline uint32_t bit(uint32_t id) { return 1U << (id & 31); }
inline uint32_t bank(uint32_t id) { return (id / 32) * 4; }
inline uint32_t vectorCount(uint32_t typer)
{
	uint32_t count = ((typer & 31) + 1) * 32;
	return count < kSpecialINTID ? count : kSpecialINTID;
}

/* Apple IOInterruptController ABI: <absolute INTID, kIOInterruptType>.
 * Read cells individually: OSData is not required to be naturally aligned. */
inline bool decode(const void *bytes, size_t length, uint32_t count,
    uint32_t &id, uint32_t &type)
{
	if (!bytes || length != 8) {
		return false;
	}
	const uint8_t *p = static_cast<const uint8_t *>(bytes);
	id = uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
	    (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
	type = uint32_t(p[4]) | (uint32_t(p[5]) << 8) |
	    (uint32_t(p[6]) << 16) | (uint32_t(p[7]) << 24);
	return id >= kFirstSPI && id < count && id < kSpecialINTID && type <= 1;
}

inline uint32_t affinity(uint64_t mpidr)
{
	return uint32_t(mpidr & 0x00ffffff) | uint32_t((mpidr >> 8) & 0xff000000);
}

inline uint64_t route(uint64_t mpidr) { return mpidr & 0xff00ffffffULL; }

/* This implementation uses TargetList, without the optional RSS extension. */
inline bool sgi(uint64_t mpidr, unsigned id, uint64_t &value)
{
	if ((mpidr & 0xff) >= 16 || id >= 16) {
		return false;
	}
	value = ((mpidr & 0xff00000000ULL) << 16) |
	    ((mpidr & 0x00ff0000) << 16) | (uint64_t(id) << 24) |
	    ((mpidr & 0x0000ff00) << 8) | (1ULL << (mpidr & 0xff));
	return true;
}

/* Bus supplies ordered MMIO and a bounded polling hint. The same code runs
 * against a register model in host tests and real MMIO in the kext. */
template <class Bus> bool waitForWrites(Bus &bus)
{
	for (unsigned n = 0; n < kPollLimit; n++) {
		if (!(bus.read32(kCTLR) & kRWP)) {
			return true;
		}
		bus.relax();
	}
	return false;
}

template <class Bus> bool disable(Bus &bus, uint32_t id)
{
	bus.write32(kICENABLER + bank(id), bit(id));
	return waitForWrites(bus);
}

template <class Bus> bool configure(Bus &bus, uint32_t id,
    uint32_t type, uint64_t target)
{
	if (!disable(bus, id)) {
		return false;
	}
	bus.write32(kICPENDR + bank(id), bit(id));
	bus.write8(kIPRIORITYR + id, kPriority);
	uint32_t offset = kICFGR + (id / 16) * 4;
	uint32_t mask = 2U << ((id & 15) * 2);
	uint32_t config = bus.read32(offset) & ~mask;
	/* kIOInterruptTypeEdge is zero, kIOInterruptTypeLevel is one. */
	if (type == 0) {
		config |= mask;
	}
	bus.write32(offset, config);
	bus.write64(kIROUTER + id * 8, target);
	/* Secure/unavailable SPIs are RAZ/WI at NS EL1. Do not claim success
 * when firmware has not delegated the interrupt to Group1NS. */
	return waitForWrites(bus) &&
	    (bus.read8(kIPRIORITYR + id) & 0xe0) == kPriority &&
	    (bus.read32(offset) & mask) == (config & mask) &&
	    (bus.read64(kIROUTER + id * 8) & 0xff00ffffffULL) == target;
}
}
#endif
