#include <pexpert/arm64/board_config.h>
#include <arm64/proc_reg.h>
#if !defined(SM8150) || !defined(QCOM_ARM64) || defined(SDM835) || defined(VMAPPLE) || defined(APPLEVIRTUALPLATFORM)
#error Incorrect physical platform
#endif
#if defined(__ARM_16K_PG__) || defined(__ARM_MIXED_PAGE_SIZE__) || defined(__ARM_ARCH_8_1__) || defined(HAS_APPLE_PAC) || defined(__ARM_RANGE_TLBI__) || defined(__ARM_PAN_AVAILABLE__) || defined(HAS_ARM_FEAT_SME)
#error Unverified processor feature selected
#endif
#if defined(__ARM_COHERENT_IO__) || defined(QCOM_UARTDM) || defined(APPLE_UART)
#error Incorrect SM8150 peripheral assumptions
#endif
_Static_assert(MAX_CPUS == 8 && MAX_CPU_CLUSTERS == 3, "incorrect scheduler groups");
_Static_assert(MAX_CPU_CLUSTER_PHY_ID == 7, "raw Aff1 indexes must cover all eight cores");
_Static_assert(ARM_PGBYTES == 4096, "expected 4K translation granule");
_Static_assert(TCR_IPS_VALUE == TCR_IPS_40BITS, "expected conservative 40-bit physical space");
_Static_assert(BTI_ENFORCED == 0 && CORE_NCTRS == 0, "unsupported Apple facility");
_Static_assert(SM8150_GICR_BASE == 0x17a60000, "must not use MSM8998 redistributor");
_Static_assert(SM8150_VTIMER_INTID == 19 && SM8150_TIMER_HZ == 19200000, "incorrect timer");
