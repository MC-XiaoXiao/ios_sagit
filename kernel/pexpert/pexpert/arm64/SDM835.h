/* SDM835/MSM8998 Kryo 280 platform. Register data: docs/sdm835/sources.json. */
#ifndef _PEXPERT_ARM64_SDM835_H
#define _PEXPERT_ARM64_SDM835_H
#define SDM835 1
#define NO_MONITOR 1
#define NO_CPU_OVRD 1
/* Use native Clutch/Edge scheduling with cluster types from the device tree.
 * CPU startup and IPIs remain on the generic PSCI/GIC backend. */
#define __ARM_AMP__ 1
#define __ARM_ARCH__ 8
#define __ARM_VMSA__ 8
#define __ARM_VFP__ 4
#define __ARM_COHERENT_CACHE__ 1
/* Peripheral DMA is noncoherent; keep __ARM_COHERENT_IO__ undefined. */
#define __ARM_IC_NOALIAS_ICACHE__ 1
#define __ARM_DEBUG__ 7
#define __ARM_ENABLE_SWAP__ 1
#define __ARM_V8_CRYPTO_EXTENSIONS__ 1
#define ARM_ARCH_TIMER
#define HAS_GIC_V3 1
#define HAS_CLUSTER 1
#define CORE_NCTRS 0 /* No platform PMU provider in this bring-up target. */
#define BTI_ENFORCED 0
/* No 16K/mixed pages, PAN, PAC, LSE, range TLBI, SME, AIC or Apple PMU. */
#define SDM835_GICD_BASE 0x17a00000
#define SDM835_GICD_SIZE 0x10000
#define SDM835_GICR_BASE 0x17b00000
#define SDM835_GICR_SIZE 0x100000
#define SDM835_TIMER_HZ 19200000
#define SDM835_VTIMER_INTID 19
#define SDM835_IPI_INTID 0
#define SDM835_UART_BASE 0x0c1b0000
#define SDM835_UART_SIZE 0x1000
/* Common Qualcomm GIC/PSCI backend; keep board constants public. */
#define QCOM_ARM64 1
#define QCOM_PLATFORM_NAME "SDM835"
#define QCOM_GICD_BASE SDM835_GICD_BASE
#define QCOM_GICD_SIZE SDM835_GICD_SIZE
#define QCOM_GICR_BASE SDM835_GICR_BASE
#define QCOM_GICR_SIZE SDM835_GICR_SIZE
#define QCOM_TIMER_HZ SDM835_TIMER_HZ
#define QCOM_VTIMER_INTID SDM835_VTIMER_INTID
#define QCOM_IPI_INTID SDM835_IPI_INTID
#ifndef ASSEMBLER
#define QCOM_UARTDM
#define PLATFORM_PANIC_LOG_DISABLED
#endif
#endif
