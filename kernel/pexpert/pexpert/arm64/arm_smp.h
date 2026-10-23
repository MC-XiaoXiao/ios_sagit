/* SPDX-License-Identifier: BSD-2-Clause
 * ARMv8 GIC-500 local interrupt handoff. Shared by XNU and register-model tests.
 */
#ifndef _PEXPERT_ARM64_ARM_SMP_H
#define _PEXPERT_ARM64_ARM_SMP_H
#include <stdint.h>
#include <stdbool.h>
#if defined(ARM64_BOARD_CONFIG_SM8150)
#include <pexpert/arm64/sm8150_topology.h>
#endif

#define ARM_GICR_STRIDE       0x20000U
#define ARM_GICR_TYPER        0x00008U
#define ARM_GICR_CTLR         0x00000U
#define ARM_GICR_WAKER        0x00014U
#define ARM_GICR_IGROUPR0     0x10080U
#define ARM_GICR_ISENABLER0   0x10100U
#define ARM_GICR_ICENABLER0   0x10180U
#define ARM_GICR_ICPENDR0     0x10280U
#define ARM_GICR_ICACTIVER0   0x10380U
#define ARM_GICR_IPRIORITYR0  0x10400U
#define ARM_GICR_ICFGR1       0x10c04U
#define ARM_GICR_IGRPMODR0    0x10d00U

struct arm_gic_io {
	void *context;
	uint32_t (*read32)(void *, uint32_t);
	uint64_t (*read64)(void *, uint32_t);
	void (*write32)(void *, uint32_t, uint32_t);
	void (*barrier)(void *);
	uint64_t (*counter)(void *);
	void (*relax)(void *);
};

enum arm_gic_result {
	ARM_GIC_OK, ARM_GIC_BAD_LAYOUT, ARM_GIC_NO_CPU, ARM_GIC_DUPLICATE_CPU,
	ARM_GIC_WAKE_TIMEOUT, ARM_GIC_WRITE_TIMEOUT, ARM_GIC_NOT_DELEGATED
};

static inline bool
arm_smp_valid_affinity(uint32_t affinity)
{
#if defined(ARM64_BOARD_CONFIG_SM8150)
	return sm8150_valid_affinity(affinity);
#else
	/* MSM8998: Aff1 0/1, Aff0 0..3; no Aff2/Aff3 CPUs. */
	return (affinity & ~0x103U) == 0;
#endif
}

static inline uint32_t
arm_smp_cpu_bit(uint32_t affinity)
{
#if defined(ARM64_BOARD_CONFIG_SM8150)
	return arm_smp_valid_affinity(affinity) ? 1U << (affinity >> 8) : 0;
#else
	return arm_smp_valid_affinity(affinity) ?
	       1U << (((affinity >> 8) * 4) + (affinity & 3)) : 0;
#endif
}

static inline bool
arm_smp_sgi(uint32_t affinity, unsigned int interrupt, uint64_t *value)
{
	if (!value || !arm_smp_valid_affinity(affinity) || interrupt >= 16) {
		return false;
	}
	*value = ((uint64_t)(affinity >> 8) << 16) |
	    ((uint64_t)interrupt << 24) | (1ULL << (affinity & 3));
	return true;
}

static inline enum arm_gic_result
arm_gic_find_cpu(const struct arm_gic_io *io, uint32_t length,
    uint32_t affinity, uint32_t *local)
{
	bool found = false;
	if (!length || length % ARM_GICR_STRIDE) {
		return ARM_GIC_BAD_LAYOUT;
	}
	for (uint32_t offset = 0; offset < length; offset += ARM_GICR_STRIDE) {
		uint64_t typer = io->read64(io->context, offset + ARM_GICR_TYPER);
		/* GIC-500 has no extra VLPI frames. Never guess their stride. */
		if (typer & 2) {
			return ARM_GIC_BAD_LAYOUT;
		}
		if ((uint32_t)(typer >> 32) == affinity) {
			if (found) {
				return ARM_GIC_DUPLICATE_CPU;
			}
			*local = offset;
			found = true;
		}
		if (typer & 16) {
			break;
		}
	}
	return found ? ARM_GIC_OK : ARM_GIC_NO_CPU;
}

static inline bool
arm_gic_wait_clear(const struct arm_gic_io *io, uint32_t offset,
    uint32_t mask, uint64_t timeout_ticks)
{
	uint64_t start = io->counter(io->context);
	/* The counter bounds elapsed time; the cap also handles a broken counter. */
	for (unsigned int n = 0; n < 1000000; n++) {
		if (!(io->read32(io->context, offset) & mask)) {
			return true;
		}
		if (io->counter(io->context) - start >= timeout_ticks) {
			break;
		}
		io->relax(io->context);
	}
	return false;
}

static inline enum arm_gic_result
arm_gic_init_cpu(const struct arm_gic_io *io, uint32_t local,
    unsigned int timer, unsigned int ipi, uint64_t timeout_ticks, bool security_disabled)
{
	if (timer < 16 || timer >= 32 || ipi >= 16 || !timeout_ticks) {
		return ARM_GIC_BAD_LAYOUT;
	}
	uint32_t mask = (1U << timer) | (1U << ipi);
	uint32_t value = io->read32(io->context, local + ARM_GICR_WAKER);
	io->write32(io->context, local + ARM_GICR_WAKER, value & ~2U);
	io->barrier(io->context);
	if (!arm_gic_wait_clear(io, local + ARM_GICR_WAKER, 4, timeout_ticks)) {
		return ARM_GIC_WAKE_TIMEOUT;
	}
	io->write32(io->context, local + ARM_GICR_ICENABLER0, mask);
	io->barrier(io->context);
	if (!arm_gic_wait_clear(io, local + ARM_GICR_CTLR, 8, timeout_ticks)) {
		return ARM_GIC_WRITE_TIMEOUT;
	}
	/* With GICD_CTLR.DS=0, IGROUPR/IGRPMODR are Secure and RAZ/WI
	 * from NS EL1. Only firmware can delegate those interrupts. When
	 * DS=1, IGROUPR is writable and IGRPMODR is itself RAZ/WI. */
	if (security_disabled) {
		value = io->read32(io->context, local + ARM_GICR_IGROUPR0);
		io->write32(io->context, local + ARM_GICR_IGROUPR0, value | mask);
	}
	/* No firmware pending/active state may survive into XNU's own sources. */
	io->write32(io->context, local + ARM_GICR_ICPENDR0, mask);
	io->write32(io->context, local + ARM_GICR_ICACTIVER0, mask);
	unsigned int sources[2] = { ipi, timer };
	for (unsigned int i = 0; i < 2; i++) {
		uint32_t offset = local + ARM_GICR_IPRIORITYR0 + (sources[i] & ~3U);
		unsigned int shift = (sources[i] & 3) * 8;
		value = io->read32(io->context, offset);
		io->write32(io->context, offset, (value & ~(0xffU << shift)) | (0x80U << shift));
		if ((io->read32(io->context, offset) & (0xf8U << shift)) != (0x80U << shift)) {
			return ARM_GIC_NOT_DELEGATED;
		}
	}
	/* The architectural virtual timer is level sensitive. SGIs are fixed edge. */
	value = io->read32(io->context, local + ARM_GICR_ICFGR1);
	io->write32(io->context, local + ARM_GICR_ICFGR1,
	    value & ~(2U << ((timer - 16) * 2)));
	io->barrier(io->context);
	if ((security_disabled &&
	    (io->read32(io->context, local + ARM_GICR_IGROUPR0) & mask) != mask) ||
	    (io->read32(io->context, local + ARM_GICR_ICFGR1) & (2U << ((timer - 16) * 2)))) {
		return ARM_GIC_NOT_DELEGATED;
	}
	io->write32(io->context, local + ARM_GICR_ISENABLER0, mask);
	io->barrier(io->context);
	if (!arm_gic_wait_clear(io, local + ARM_GICR_CTLR, 8, timeout_ticks)) {
		return ARM_GIC_WRITE_TIMEOUT;
	}
	return (io->read32(io->context, local + ARM_GICR_ISENABLER0) & mask) == mask ?
	       ARM_GIC_OK : ARM_GIC_NOT_DELEGATED;
}
#endif
