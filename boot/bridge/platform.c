// SPDX-License-Identifier: BSD-2-Clause
#include "bridge.h"
#include "watchdog.h"
#include "../../kernel/pexpert/pexpert/arm/qcom_geni_console.h"
void bridge_clean(uint64_t start, uint64_t end) {
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uint64_t line = 4ULL << ((ctr >> 16) & 15);
    for (uint64_t p = start & ~(line - 1); p < end; p += line)
        __asm__ volatile("dc cvac, %0" ::"r"(p) : "memory");
    __asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
}
void bridge_hex(uint64_t n) {
    char s[19] = "0x0000000000000000";
    const char *h = "0123456789abcdef";
    for (int i = 17; i >= 2; i--) {
        s[i] = h[n & 15];
        n >>= 4;
    }
    bridge_puts(s);
}

// The loader shares one console contract across ARM64 boards: firmware has
// already configured clocks, pinmux and baud rate for the selected UART.
static uint64_t uart_base;
static unsigned int uart_kind, uart_shift, uart_width;
static int has_qcom_watchdog;
static qcom_geni_console_t geni_console;
static uint32_t bridge_geni_read(void *context, uint32_t offset) {
    uint32_t value = *(volatile uint32_t *)((uintptr_t)context + offset);
    __asm__ volatile("dmb sy" ::: "memory");
    return value;
}
static void bridge_geni_write(void *context, uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)((uintptr_t)context + offset) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
static uint32_t read32(const void *p) {
    const unsigned char *s = p;
    return (uint32_t)s[0] | ((uint32_t)s[1] << 8) | ((uint32_t)s[2] << 16) | ((uint32_t)s[3] << 24);
}
static uint64_t read64(const void *p) {
    return read32(p) | ((uint64_t)read32((const unsigned char *)p + 4) << 32);
}
static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}
static int compatible(const char *data, uint32_t size, const char *needle) {
    for (uint32_t i = 0; i < size;) {
        const char *p = data + i;
        uint32_t start = i;
        while (i < size && data[i])
            i++;
        if (i == size)
            return 0;
        if (str_eq(p, needle))
            return 1;
        i++;
        if (i <= start)
            return 0;
    }
    return 0;
}
int platform_console_init(void *tree) {
    uint32_t n;
    uart_kind = 0;
    uart_base = 0;
    char *root_compatible = dt_node_property(tree, "compatible", &n);
    has_qcom_watchdog = root_compatible && compatible(root_compatible, n, "qcom,msm8998");
    void *ph = dt_property(tree, "chosen", "serial-device", &n);
    if (ph && n != 4)
        return -1;
    if (!ph)
        ph = dt_property(tree, "defaults", "serial-device", &n);
    if (ph && n != 4)
        return -1;
    if (!ph) {
        void *allow = dt_property(tree, "chosen", "xnu,allow-no-uart", &n);
        if (!ph && allow && n == 4 && read32(allow) == 1)
            return 0;
        return -1;
    }
    void *uart = dt_find_phandle(tree, read32(ph));
    if (!uart)
        return -2;
    void *reg = dt_node_property(uart, "reg", &n);
    if (!reg || n < 16)
        return -3;
    uint64_t uart_offset = read64(reg);
    uart_base = uart_offset;
    void *ranges = dt_property(tree, "arm-io", "ranges", &n);
    if (!ranges || n < 24)
        return -4;
    uart_base += read64((unsigned char *)ranges + 8);
    char *comp = dt_node_property(uart, "compatible", &n);
    if (!comp)
        return -5;
    if (compatible(comp, n, "qcom,msm-uartdm-v1.4") || compatible(comp, n, "qcom,msm-uartdm"))
        uart_kind = 1;
    else if (compatible(comp, n, "arm,pl011"))
        uart_kind = 2;
    else if (compatible(comp, n, "ns16550a") || compatible(comp, n, "snps,dw-apb-uart"))
        uart_kind = 3;
    else if (compatible(comp, n, "qcom,geni-debug-uart") || compatible(comp, n, "qcom,geni-uart")) {
        void *handoff = dt_node_property(uart, "qcom,geni-console-handoff", &n);
        if (!handoff || n != 4 || read32(handoff) != 1)
            return -8;
        void *geni_ranges = dt_property(tree, "arm-io", "ranges", &n);
        if (!geni_ranges || n != 24)
            return -9;
        uint64_t child = read64(geni_ranges), parent = read64((unsigned char *)geni_ranges + 8);
        uint64_t span = read64((unsigned char *)geni_ranges + 16);
        if (!span || uart_offset < child || uart_offset - child >= span ||
            parent >= (1ULL << 40) || uart_offset - child >= (1ULL << 40) - parent)
            return -9;
        uart_base = parent + uart_offset - child;
        void *region = dt_node_property(uart, "reg", &n);
        if (!region || n != 16 || read64((unsigned char *)region + 8) < 0x1000 ||
            read64((unsigned char *)region + 8) > 0x10000 || (uart_base & 3) ||
            read64((unsigned char *)region + 8) > span - (uart_offset - child) ||
            read64((unsigned char *)region + 8) > (1ULL << 40) - uart_base)
            return -9;
        void *status = dt_node_property(uart, "status", &n);
        if (status && !((n == 5 && str_eq(status, "okay")) || (n == 3 && str_eq(status, "ok"))))
            return -10;
        if (!qcom_geni_console_init(&geni_console, (void *)(uintptr_t)uart_base,
                bridge_geni_read, bridge_geni_write))
            return -11;
        uart_kind = 4;
    }
    else
        return -6;
    void *v = dt_node_property(uart, "reg-shift", &n);
    uart_shift = v && n == 4 ? read32(v) : 0;
    v = dt_node_property(uart, "reg-io-width", &n);
    uart_width = v && n == 4 ? read32(v) : 1;
    if (uart_shift > 4 || (uart_width != 1 && uart_width != 4))
        return -7;
    return 0;
}
static uint32_t uart_read(unsigned int offset) {
    uint64_t p = uart_base + ((uint64_t)offset << uart_shift);
    return uart_width == 4 ? *(volatile uint32_t *)p : *(volatile uint8_t *)p;
}
static void uart_write(unsigned int offset, uint32_t value) {
    uint64_t p = uart_base + ((uint64_t)offset << uart_shift);
    if (uart_width == 4)
        *(volatile uint32_t *)p = value;
    else
        *(volatile uint8_t *)p = value;
}
void bridge_puts(const char *s) {
    while (*s) {
        if (*s == '\n')
            bridge_puts("\r");
        unsigned int timeout = 1000000;
        if (uart_kind == 1) {
            while (!(*(volatile uint32_t *)(uart_base + 8) & 8) && --timeout)
                __asm__ volatile("yield");
            if (!timeout)
                return;
            *(volatile uint32_t *)(uart_base + 0x40) = 1;
            __asm__ volatile("dsb sy" ::: "memory");
            *(volatile uint32_t *)(uart_base + 0x70) = (unsigned char)*s++;
        } else if (uart_kind == 2) {
            while ((*(volatile uint32_t *)(uart_base + 0x18) & 0x20) && --timeout)
                __asm__ volatile("yield");
            if (!timeout)
                return;
            *(volatile uint32_t *)uart_base = (unsigned char)*s++;
        } else if (uart_kind == 3) {
            while (!(uart_read(5) & 0x20) && --timeout)
                __asm__ volatile("yield");
            if (!timeout)
                return;
            uart_write(0, (unsigned char)*s++);
        } else if (uart_kind == 4) {
            if (!qcom_geni_console_putc(&geni_console, (unsigned char)*s++))
                return;
        } else
            return;
        __asm__ volatile("dsb sy" ::: "memory");
    }
}
int platform_watchdog_arm(unsigned int seconds) {
    return has_qcom_watchdog ? bridge_watchdog_arm(seconds) : -1;
}
uint32_t platform_watchdog_read(unsigned int offset) {
    return has_qcom_watchdog ? *(volatile uint32_t *)(BRIDGE_WDT_BASE + offset) : 0;
}
