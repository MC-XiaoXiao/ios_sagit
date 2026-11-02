// SPDX-License-Identifier: BSD-2-Clause
#ifndef BRIDGE_WATCHDOG_H
#define BRIDGE_WATCHDOG_H
#include <stdint.h>
// MSM8998 downstream DT: wdt@17817000. Register layout and 32765 Hz clock:
// MiCode/Xiaomi_Kernel_OpenSource @ 3fb35ef1f0fe1f714be3df39724cbc8eb217181e
// drivers/soc/qcom/watchdog_v2.c. Debug recovery timer, deliberately not petted.
#define BRIDGE_WDT_BASE 0x17817000ULL
#define BRIDGE_WDT_HZ 32765U
// Both threshold registers are 20 bits; 32 seconds would wrap at this rate.
#define BRIDGE_WDT_MAX_SECONDS 31U
#ifndef BRIDGE_WATCHDOG_SECONDS
#define BRIDGE_WATCHDOG_SECONDS 15
#endif
#ifndef WDT_WRITE
#define WDT_WRITE(off, val)                                                                        \
    do {                                                                                           \
        *(volatile uint32_t *)(BRIDGE_WDT_BASE + (off)) = (val);                                   \
        __asm__ volatile("dsb sy" ::: "memory");                                                   \
    } while (0)
#endif
static inline int bridge_watchdog_arm(unsigned int seconds) {
    if (seconds > BRIDGE_WDT_MAX_SECONDS)
        return -1;
    WDT_WRITE(0x08, 0);
    if (!seconds)
        return 0;
    WDT_WRITE(0x04, 1);
    // Bark is later than bite: no unhandled bark IRQ ahead of hardware reset.
    WDT_WRITE(0x10, 0xfffffU);
    WDT_WRITE(0x14, BRIDGE_WDT_HZ * seconds);
    WDT_WRITE(0x08, 3); // EN | UNMASKED_INT_EN, matching qcom,wakeup-enable
    WDT_WRITE(0x04, 1);
    return 0;
}
#endif
