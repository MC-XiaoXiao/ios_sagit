/* SPDX-License-Identifier: BSD-2-Clause
 * SM8150 raw MPIDR affinity and scheduler frequency-domain mapping.
 * Linux describes one DSU cluster and three CPU frequency domains (4+3+1).
 * These scheduler groups are not hardware MPIDR Aff1 clusters.
 */
#ifndef _PEXPERT_ARM64_SM8150_TOPOLOGY_H
#define _PEXPERT_ARM64_SM8150_TOPOLOGY_H
#include <stdint.h>
#include <stdbool.h>

static inline bool
sm8150_valid_affinity(uint32_t affinity)
{
	return (affinity & ~0x700U) == 0;
}

static inline unsigned int
sm8150_scheduler_group(uint32_t affinity)
{
	unsigned int core = affinity >> 8;
	return core < 4 ? 0 : core < 7 ? 1 : 2;
}

static inline unsigned int
sm8150_group_core(uint32_t affinity)
{
	unsigned int core = affinity >> 8;
	return core < 4 ? core : core < 7 ? core - 4 : 0;
}
#endif
