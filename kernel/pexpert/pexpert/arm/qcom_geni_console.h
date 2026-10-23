/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Polled, retained-firmware GENI UART console.
 *
 * Register facts: Linux v6.12 qcom_geni_serial.c, qcom-geni-se.c, geni-se.h;
 * pinned provenance is recorded in docs/sm8150/sources.json.
 *
 * Caller must own the engine exclusively, serialize calls, and provide ordered
 * 32-bit device MMIO callbacks. Firmware must retain the UART microcode, clocks,
 * pinmux and baud rate. This backend never programs clocks, pins, DMA or baud.
 * Only FIFO mode, 8N1, and four little-endian bytes per FIFO word are admitted.
 * A register read cannot establish board ownership or make unpowered MMIO safe.
 */
#ifndef _PEXPERT_QCOM_GENI_CONSOLE_H
#define _PEXPERT_QCOM_GENI_CONSOLE_H
#include <stdint.h>

#define QGC_STATUS             0x040U
#define QGC_M_CLK_CFG          0x048U
#define QGC_S_CLK_CFG          0x04cU
#define QGC_IF_DISABLE         0x064U
#define QGC_FW_REVISION        0x068U
#define QGC_BYTE_GRAN          0x254U
#define QGC_DMA_MODE           0x258U
#define QGC_TX_TRANS_CFG       0x25cU
#define QGC_TX_PACK0           0x260U
#define QGC_TX_PACK1           0x264U
#define QGC_TX_WORD_LEN        0x268U
#define QGC_TX_STOP_BITS       0x26cU
#define QGC_TX_TRANS_LEN       0x270U
#define QGC_RX_TRANS_CFG       0x280U
#define QGC_RX_PACK0           0x284U
#define QGC_RX_PACK1           0x288U
#define QGC_RX_WORD_LEN        0x28cU
#define QGC_RX_STALE_CNT       0x294U
#define QGC_TX_PARITY          0x2a4U
#define QGC_RX_PARITY          0x2a8U
#define QGC_M_CMD              0x600U
#define QGC_M_CMD_CTRL         0x604U
#define QGC_M_IRQ_STATUS       0x610U
#define QGC_M_IRQ_EN           0x614U
#define QGC_M_IRQ_CLEAR        0x618U
#define QGC_S_CMD              0x630U
#define QGC_S_IRQ_STATUS       0x640U
#define QGC_S_IRQ_EN           0x644U
#define QGC_S_IRQ_CLEAR        0x648U
#define QGC_TX_FIFO            0x700U
#define QGC_RX_FIFO            0x780U
#define QGC_TX_FIFO_STATUS     0x800U
#define QGC_RX_FIFO_STATUS     0x804U
#define QGC_GSI_EVENT_EN       0xe18U
#define QGC_HW_PARAM_TX        0xe24U
#define QGC_HW_PARAM_RX        0xe28U

#define QGC_MAIN_ACTIVE        (1U << 0)
#define QGC_SECONDARY_ACTIVE   (1U << 12)
#define QGC_START_UART         (1U << 27)
#define QGC_CMD_DONE           (1U << 0)
#define QGC_CMD_ABORTED        (1U << 5)
#define QGC_ABORT              (1U << 1)
#define QGC_TX_ERRORS          ((7U << 1) | (3U << 28))
#define QGC_RX_ERRORS          ((7U << 1) | (15U << 9) | (3U << 24))
#define QGC_RX_EVENTS          ((1U << 26) | (1U << 27))
#define QGC_RX_LAST            (1U << 31)
#define QGC_FIFO_WORDS         0x01ffffffU
#define QGC_PACK_4X8_0         0x0004380eU
#define QGC_PACK_4X8_1         0x000c3e0eU
#ifndef QGC_POLL_LIMIT
#define QGC_POLL_LIMIT         100000U
#endif

typedef uint32_t (*qcom_geni_read32_t)(void *, uint32_t);
typedef void (*qcom_geni_write32_t)(void *, uint32_t, uint32_t);
typedef struct qcom_geni_console {
    void *context;
    qcom_geni_read32_t read32;
    qcom_geni_write32_t write32;
    uint32_t rx_word;
    unsigned int rx_count;
    unsigned int rx_depth;
    unsigned int enabled;
} qcom_geni_console_t;

static inline uint32_t qgc_read(qcom_geni_console_t *s, uint32_t offset)
{
    return s->read32(s->context, offset);
}
static inline void qgc_write(qcom_geni_console_t *s, uint32_t offset, uint32_t value)
{
    s->write32(s->context, offset, value);
}
static inline int qgc_wait(qcom_geni_console_t *s, uint32_t offset,
    uint32_t mask, uint32_t expected)
{
    unsigned int i;
    for (i = 0; i < QGC_POLL_LIMIT; ++i)
        if ((qgc_read(s, offset) & mask) == expected) return 1;
    return 0;
}

static inline int qcom_geni_console_init(qcom_geni_console_t *s, void *context,
    qcom_geni_read32_t read32, qcom_geni_write32_t write32)
{
    uint32_t tx_hw, rx_hw, status, mclk, sclk;
    if (!s) return 0;
    s->enabled = 0;
    s->rx_count = 0;
    s->rx_word = 0;
    s->rx_depth = 0;
    s->context = context;
    s->read32 = read32;
    s->write32 = write32;
    if (!read32 || !write32) return 0;
    /* Validate the entire retained configuration before any register write. */
    if (((qgc_read(s, QGC_FW_REVISION) >> 8) & 0xffU) != 2 ||
        (qgc_read(s, QGC_IF_DISABLE) & 1U) ||
        (qgc_read(s, QGC_DMA_MODE) & 1U) ||
        qgc_read(s, QGC_GSI_EVENT_EN) != 0 ||
        qgc_read(s, QGC_BYTE_GRAN) != 0 ||
        qgc_read(s, QGC_TX_PACK0) != QGC_PACK_4X8_0 ||
        qgc_read(s, QGC_RX_PACK0) != QGC_PACK_4X8_0 ||
        qgc_read(s, QGC_TX_PACK1) != QGC_PACK_4X8_1 ||
        qgc_read(s, QGC_RX_PACK1) != QGC_PACK_4X8_1 ||
        (qgc_read(s, QGC_TX_WORD_LEN) & 0x3ffU) != 8 ||
        (qgc_read(s, QGC_RX_WORD_LEN) & 0x3ffU) != 8 ||
        (qgc_read(s, QGC_TX_TRANS_CFG) & 1U) ||
        (qgc_read(s, QGC_RX_TRANS_CFG) & 8U) ||
        (qgc_read(s, QGC_TX_PARITY) & 1U) ||
        (qgc_read(s, QGC_RX_PARITY) & 1U) ||
        (qgc_read(s, QGC_TX_STOP_BITS) & 3U) ||
        !(qgc_read(s, QGC_RX_STALE_CNT) & 0xffffffU)) return 0;
    mclk = qgc_read(s, QGC_M_CLK_CFG);
    sclk = qgc_read(s, QGC_S_CLK_CFG);
    if (!(mclk & 1U) || !(sclk & 1U) ||
        !(mclk & 0xfff0U) || !(sclk & 0xfff0U)) return 0;
    tx_hw = qgc_read(s, QGC_HW_PARAM_TX);
    rx_hw = qgc_read(s, QGC_HW_PARAM_RX);
    if (((tx_hw >> 24) & 0x3fU) != 32 ||
        ((rx_hw >> 24) & 0x3fU) != 32 ||
        !((tx_hw >> 16) & 0x3fU) || !((rx_hw >> 16) & 0x3fU)) return 0;
    status = qgc_read(s, QGC_STATUS);
    if ((status & QGC_MAIN_ACTIVE) || qgc_read(s, QGC_TX_FIFO_STATUS) != 0 ||
        ((status & QGC_SECONDARY_ACTIVE) &&
        (qgc_read(s, QGC_S_CMD) >> 27) != 1)) return 0;
    s->rx_depth = (rx_hw >> 16) & 0x3fU;
    qgc_write(s, QGC_M_IRQ_EN, 0);
    qgc_write(s, QGC_S_IRQ_EN, 0);
    qgc_write(s, QGC_M_IRQ_CLEAR, QGC_CMD_DONE | QGC_TX_ERRORS | QGC_CMD_ABORTED);
    if (!(status & QGC_SECONDARY_ACTIVE))
        qgc_write(s, QGC_S_CMD, QGC_START_UART);
    s->enabled = 1;
    return 1;
}

static inline int qcom_geni_console_putc(qcom_geni_console_t *s, uint8_t c)
{
    unsigned int i;
    if (!s || !s->enabled) return 0;
    if (!qgc_wait(s, QGC_STATUS, QGC_MAIN_ACTIVE, 0) ||
        !qgc_wait(s, QGC_TX_FIFO_STATUS, 0x0fffffffU, 0)) {
        s->enabled = 0;
        return 0;
    }
    qgc_write(s, QGC_M_IRQ_CLEAR, QGC_CMD_DONE | QGC_TX_ERRORS | QGC_CMD_ABORTED);
    qgc_write(s, QGC_TX_TRANS_LEN, 1);
    qgc_write(s, QGC_M_CMD, QGC_START_UART);
    qgc_write(s, QGC_TX_FIFO, c);
    for (i = 0; i < QGC_POLL_LIMIT; ++i) {
        uint32_t irq = qgc_read(s, QGC_M_IRQ_STATUS);
        if (irq & QGC_TX_ERRORS) break;
        if (irq & QGC_CMD_DONE) {
            qgc_write(s, QGC_M_IRQ_CLEAR, QGC_CMD_DONE);
            return 1;
        }
    }
    /* Abort only our own command. Retire this console on timeout or error. */
    qgc_write(s, QGC_M_CMD_CTRL, QGC_ABORT);
    (void)qgc_wait(s, QGC_M_IRQ_STATUS, QGC_CMD_ABORTED, QGC_CMD_ABORTED);
    qgc_write(s, QGC_M_IRQ_CLEAR, QGC_CMD_DONE | QGC_TX_ERRORS | QGC_CMD_ABORTED);
    s->enabled = 0;
    return 0;
}

static inline unsigned int qcom_geni_console_rx_ready(qcom_geni_console_t *s)
{
    uint32_t irq, status, words, bytes;
    if (!s || !s->enabled) return 0;
    if (s->rx_count) return 1;
    irq = qgc_read(s, QGC_S_IRQ_STATUS);
    if (irq & QGC_RX_ERRORS) {
        qgc_write(s, QGC_S_IRQ_CLEAR, irq & (QGC_RX_ERRORS | QGC_RX_EVENTS));
        s->enabled = 0; // Never expose corrupted bytes as debugger input.
        return 0;
    }
    status = qgc_read(s, QGC_RX_FIFO_STATUS);
    words = status & QGC_FIFO_WORDS;
    if (!words) return 0;
    if (words > s->rx_depth) { s->enabled = 0; return 0; }
    bytes = 4;
    if (words == 1 && (status & QGC_RX_LAST)) {
        bytes = (status >> 28) & 7U;
        if (!bytes) bytes = 4; // Zero encodes a full final word.
        if (bytes > 4) { s->enabled = 0; return 0; }
    }
    s->rx_word = qgc_read(s, QGC_RX_FIFO);
    s->rx_count = bytes;
    qgc_write(s, QGC_S_IRQ_CLEAR, irq & QGC_RX_EVENTS);
    return 1;
}

static inline int qcom_geni_console_getc(qcom_geni_console_t *s)
{
    int result;
    if (!qcom_geni_console_rx_ready(s)) return -1;
    result = s->rx_word & 0xffU;
    s->rx_word >>= 8;
    --s->rx_count;
    return result;
}
#endif
