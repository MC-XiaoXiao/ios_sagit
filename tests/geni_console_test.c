#define QGC_POLL_LIMIT 16U
#include "../kernel/pexpert/pexpert/arm/qcom_geni_console.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct fake_mmio {
    uint32_t regs[0x1000 / 4];
    unsigned int writes, reads, fifo_reads, stalled, aborted;
    uint8_t transmitted;
};

static uint32_t read_reg(void *context, uint32_t offset)
{
    struct fake_mmio *f = context;
    assert(!(offset & 3) && offset < sizeof(f->regs));
    ++f->reads;
    if (offset == QGC_RX_FIFO) {
        ++f->fifo_reads;
        f->regs[QGC_RX_FIFO_STATUS / 4] = 0;
    }
    return f->regs[offset / 4];
}
static void write_reg(void *context, uint32_t offset, uint32_t value)
{
    struct fake_mmio *f = context;
    assert(!(offset & 3) && offset < sizeof(f->regs));
    ++f->writes;
    f->regs[offset / 4] = value;
    if (offset == QGC_M_IRQ_CLEAR) f->regs[QGC_M_IRQ_STATUS / 4] &= ~value;
    if (offset == QGC_S_IRQ_CLEAR) f->regs[QGC_S_IRQ_STATUS / 4] &= ~value;
    if (offset == QGC_S_CMD) f->regs[QGC_STATUS / 4] |= QGC_SECONDARY_ACTIVE;
    if (offset == QGC_M_CMD) f->regs[QGC_STATUS / 4] |= QGC_MAIN_ACTIVE;
    if (offset == QGC_TX_FIFO) {
        f->transmitted = value;
        if (!f->stalled) {
            f->regs[QGC_STATUS / 4] &= ~QGC_MAIN_ACTIVE;
            f->regs[QGC_M_IRQ_STATUS / 4] |= QGC_CMD_DONE;
        }
    }
    if (offset == QGC_M_CMD_CTRL && value == QGC_ABORT) {
        ++f->aborted;
        if (f->stalled != 2) {
            f->regs[QGC_STATUS / 4] &= ~QGC_MAIN_ACTIVE;
            f->regs[QGC_M_IRQ_STATUS / 4] |= QGC_CMD_ABORTED;
        }
    }
}
static void good_config(struct fake_mmio *f)
{
    memset(f, 0, sizeof(*f));
    f->regs[QGC_FW_REVISION / 4] = 2U << 8;
    f->regs[QGC_TX_PACK0 / 4] = f->regs[QGC_RX_PACK0 / 4] = QGC_PACK_4X8_0;
    f->regs[QGC_TX_PACK1 / 4] = f->regs[QGC_RX_PACK1 / 4] = QGC_PACK_4X8_1;
    f->regs[QGC_TX_WORD_LEN / 4] = f->regs[QGC_RX_WORD_LEN / 4] = 8;
    f->regs[QGC_M_CLK_CFG / 4] = f->regs[QGC_S_CLK_CFG / 4] = 0x101;
    f->regs[QGC_HW_PARAM_TX / 4] = f->regs[QGC_HW_PARAM_RX / 4] = (32U << 24) | (16U << 16);
    f->regs[QGC_RX_STALE_CNT / 4] = 160;
}
static void rejection(uint32_t offset, uint32_t value)
{
    qcom_geni_console_t s;
    struct fake_mmio f;
    good_config(&f);
    f.regs[offset / 4] = value;
    assert(!qcom_geni_console_init(&s, &f, read_reg, write_reg));
    assert(!s.enabled && f.writes == 0);
}
int main(void)
{
    qcom_geni_console_t s;
    struct fake_mmio f;
    unsigned int reads, writes, i;
    rejection(QGC_FW_REVISION, 1U << 8);
    rejection(QGC_DMA_MODE, 1);
    rejection(QGC_GSI_EVENT_EN, 4);
    rejection(QGC_IF_DISABLE, 1);
    rejection(QGC_TX_PACK0, 0xf); // Firmware's one-byte mode is not four-byte mode.
    rejection(QGC_RX_PACK1, 0);
    rejection(QGC_BYTE_GRAN, 1);
    rejection(QGC_TX_WORD_LEN, 7);
    rejection(QGC_RX_TRANS_CFG, 8);
    rejection(QGC_TX_STOP_BITS, 2);
    rejection(QGC_M_CLK_CFG, 0);
    rejection(QGC_RX_STALE_CNT, 0);
    rejection(QGC_HW_PARAM_TX, 16U << 24);
    rejection(QGC_STATUS, QGC_MAIN_ACTIVE);
    rejection(QGC_TX_FIFO_STATUS, 1);
    rejection(QGC_STATUS, QGC_SECONDARY_ACTIVE); // Missing retained READ command.
    good_config(&f);
    assert(qcom_geni_console_init(&s, &f, read_reg, write_reg));
    assert(s.enabled && f.regs[QGC_S_CMD / 4] == QGC_START_UART);
    assert(qcom_geni_console_putc(&s, 'X'));
    assert(f.transmitted == 'X' && f.regs[QGC_TX_TRANS_LEN / 4] == 1);
    assert(!qcom_geni_console_rx_ready(&s));
    assert(qcom_geni_console_getc(&s) == -1);
    // GENI's LAST=1, VALID=3 means three bytes; bytes cached by ready().
    f.regs[QGC_RX_FIFO_STATUS / 4] = QGC_RX_LAST | (3U << 28) | 1;
    f.regs[QGC_RX_FIFO / 4] = 0xff434241;
    f.regs[QGC_S_IRQ_STATUS / 4] = QGC_RX_EVENTS;
    assert(qcom_geni_console_rx_ready(&s));
    assert(qcom_geni_console_rx_ready(&s));
    assert(f.fifo_reads == 1);
    assert(qcom_geni_console_getc(&s) == 'A');
    assert(qcom_geni_console_getc(&s) == 'B');
    assert(qcom_geni_console_getc(&s) == 'C');
    assert(qcom_geni_console_getc(&s) == -1);
    // Zero VALID bits on LAST means four bytes, not an empty word.
    f.regs[QGC_RX_FIFO_STATUS / 4] = QGC_RX_LAST | 1;
    f.regs[QGC_RX_FIFO / 4] = 0x04030201;
    for (i = 1; i <= 4; ++i) assert(qcom_geni_console_getc(&s) == (int)i);
    // A dead engine has finite TX and abort waits and remains disabled.
    for (i = 1; i <= 2; ++i) {
        good_config(&f);
        assert(qcom_geni_console_init(&s, &f, read_reg, write_reg));
        f.stalled = i;
        reads = f.reads;
        assert(!qcom_geni_console_putc(&s, 'Y'));
        assert(!s.enabled && f.aborted == 1);
        assert(f.reads - reads <= 2 * QGC_POLL_LIMIT + 2);
        writes = f.writes;
        assert(!qcom_geni_console_putc(&s, 'Z'));
        assert(f.writes == writes);
    }
    good_config(&f);
    assert(qcom_geni_console_init(&s, &f, read_reg, write_reg));
    f.regs[QGC_RX_FIFO_STATUS / 4] = QGC_RX_LAST | (7U << 28) | 1;
    assert(qcom_geni_console_getc(&s) == -1 && !s.enabled && !f.fifo_reads);
    good_config(&f);
    assert(qcom_geni_console_init(&s, &f, read_reg, write_reg));
    f.regs[QGC_S_IRQ_STATUS / 4] = 1U << 9; // Parity error.
    f.regs[QGC_RX_FIFO_STATUS / 4] = 1;
    assert(qcom_geni_console_getc(&s) == -1 && !s.enabled && !f.fifo_reads);
    assert(!qcom_geni_console_init(0, &f, read_reg, write_reg));
    puts("GENI: retained-mode admission, byte I/O, stale-tail, errors and bounded timeouts passed");
    return 0;
}
