/* SPDX-License-Identifier: BSD-2-Clause
 * Include the production XNU adapter, not a copy of its locking or DT logic.
 * MMIO callbacks deliberately re-enter all console operations while the
 * interrupted operation owns its lock. A separate test runs real TX/RX
 * pthreads; contended output may be dropped, but commands/input cannot mix.
 */
#define _POSIX_C_SOURCE 200809L
#define QGC_POLL_LIMIT 16U
#include <pexpert/arm/qcom_geni_console.h>
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

typedef uintptr_t vm_offset_t;
typedef int DeviceTreeNode;
#define DSB_SY 0
#define __builtin_arm_dsb(x) ((void)(x))
#define SERIAL_QCOM_GENI 32
#define kSuccess 0
struct pe_serial_functions {
    void (*init)(void);
    unsigned (*transmit_ready)(void);
    void (*transmit_data)(uint8_t);
    unsigned (*receive_ready)(void);
    uint8_t (*receive_data)(void);
    int (*try_receive)(void);
    int device;
};

static struct {
    uint32_t handoff;
    uint64_t reg[2], soc_base;
    const char *status;
    unsigned handoff_size, reg_size, status_size;
    bool missing_handoff, missing_reg, missing_status, mapping_failure;
} dt;
static struct {
    uint32_t regs[0x1000 / 4];
    uint8_t rx[4096], tx[8192];
    unsigned rx_size, rx_offset, tx_size, reads, writes, maps, registrations;
    uint64_t mapped_physical, mapped_size;
    bool inject_reentry, concurrent;
    unsigned reentries;
} model;
static unsigned char callback_busy;
static _Thread_local unsigned commands_sent;
static const uintptr_t virtual_base = 0x10000000U;
static struct pe_serial_functions *registered;
static void attempt_reentry(void);

static unsigned enter_mmio(uintptr_t address)
{
    /* If the production lock is removed/bypassed, fail immediately rather
     * than allowing recursive accesses to make this mock appear correct. */
    assert(!__atomic_test_and_set(&callback_busy, __ATOMIC_ACQUIRE));
    assert(address >= virtual_base && address < virtual_base + sizeof(model.regs));
    const unsigned offset = (unsigned)(address - virtual_base);
    assert(!(offset & 3));
    if (model.inject_reentry) attempt_reentry();
    if (model.concurrent) sched_yield();
    return offset;
}

static uint32_t ml_io_read32(uintptr_t address)
{
    const unsigned offset = enter_mmio(address);
    uint32_t value = model.regs[offset / 4];
    ++model.reads;
    if (offset == QGC_RX_FIFO_STATUS) {
        const unsigned remaining = model.rx_size - model.rx_offset;
        unsigned words = (remaining + 3) / 4;
        if (words > 16) words = 16;
        value = words;
        if (words == 1) value |= QGC_RX_LAST | ((remaining & 3U) << 28);
    }
    if (offset == QGC_RX_FIFO) {
        unsigned remaining = model.rx_size - model.rx_offset;
        assert(remaining);
        if (remaining > 4) remaining = 4;
        value = 0;
        for (unsigned i = 0; i < remaining; ++i)
            value |= (uint32_t)model.rx[model.rx_offset++] << (8 * i);
    }
    __atomic_clear(&callback_busy, __ATOMIC_RELEASE);
    return value;
}

static void ml_io_write32(uintptr_t address, uint32_t value)
{
    const unsigned offset = enter_mmio(address);
    ++model.writes;
    switch (offset) {
    case QGC_M_IRQ_EN: case QGC_S_IRQ_EN:
        assert(value == 0); model.regs[offset / 4] = value; break;
    case QGC_M_IRQ_CLEAR: model.regs[QGC_M_IRQ_STATUS / 4] &= ~value; break;
    case QGC_S_IRQ_CLEAR: model.regs[QGC_S_IRQ_STATUS / 4] &= ~value; break;
    case QGC_S_CMD:
        assert(value == QGC_START_UART);
        model.regs[offset / 4] = value;
        model.regs[QGC_STATUS / 4] |= QGC_SECONDARY_ACTIVE;
        break;
    case QGC_TX_TRANS_LEN:
        assert(value == 1 && !(model.regs[QGC_STATUS / 4] & QGC_MAIN_ACTIVE));
        model.regs[offset / 4] = value; break;
    case QGC_M_CMD:
        assert(value == QGC_START_UART && model.regs[QGC_TX_TRANS_LEN / 4] == 1);
        assert(!(model.regs[QGC_STATUS / 4] & QGC_MAIN_ACTIVE));
        model.regs[QGC_STATUS / 4] |= QGC_MAIN_ACTIVE; break;
    case QGC_TX_FIFO:
        assert(model.regs[QGC_STATUS / 4] & QGC_MAIN_ACTIVE);
        assert(value <= 255 && model.tx_size < sizeof(model.tx));
        model.tx[model.tx_size++] = (uint8_t)value;
        ++commands_sent;
        model.regs[QGC_STATUS / 4] &= ~QGC_MAIN_ACTIVE;
        model.regs[QGC_M_IRQ_STATUS / 4] |= QGC_CMD_DONE;
        break;
    default:
        /* In particular, no baud/clock/packing writes are authorized. */
        assert(!"unexpected production MMIO write");
    }
    __atomic_clear(&callback_busy, __ATOMIC_RELEASE);
}

static int SecureDTGetProperty(const DeviceTreeNode *node, const char *name,
                              const void **value, unsigned *size)
{
    assert(node);
    if (!strcmp(name, "qcom,geni-console-handoff")) {
        if (dt.missing_handoff) return -1;
        *value = &dt.handoff; *size = dt.handoff_size; return kSuccess;
    }
    if (!strcmp(name, "status")) {
        if (dt.missing_status) return -1;
        *value = dt.status; *size = dt.status_size; return kSuccess;
    }
    assert(!strcmp(name, "reg"));
    if (dt.missing_reg) return -1;
    *value = dt.reg; *size = dt.reg_size; return kSuccess;
}
static uint64_t pe_arm_get_soc_base_phys(void) { return dt.soc_base; }
static vm_offset_t ml_io_map(uint64_t physical, uint64_t bytes)
{
    ++model.maps; model.mapped_physical = physical; model.mapped_size = bytes;
    return dt.mapping_failure ? 0 : virtual_base;
}
static void register_serial_functions(struct pe_serial_functions *functions)
{
    registered = functions; ++model.registrations;
}
#include "../kernel/pexpert/arm/pe_serial_geni.inc"

static void attempt_reentry(void)
{
    qcom_geni_console_t before;
    memcpy(&before, &geni_console, sizeof(before));
    const unsigned reads = model.reads, writes = model.writes;
    assert(geni_transmit_ready() == 1);
    geni_transmit_data('!');
    assert(geni_receive_character() == -1);
    geni_init();
    assert(!memcmp(&before, &geni_console, sizeof(before)));
    assert(model.reads == reads && model.writes == writes);
    ++model.reentries;
}

static void reset(void)
{
    memset(&dt, 0, sizeof(dt)); memset(&model, 0, sizeof(model));
    memset(&geni_console, 0, sizeof(geni_console));
    geni_base = 0; geni_lock = callback_busy = 0; registered = NULL;
    dt.handoff = 1; dt.handoff_size = 4;
    dt.reg[0] = 0x00a90000; dt.reg[1] = 0x1000; dt.reg_size = 16;
    dt.soc_base = 0x01000000; dt.status = "okay"; dt.status_size = 5;
    model.regs[QGC_FW_REVISION / 4] = 2U << 8;
    model.regs[QGC_TX_PACK0 / 4] = model.regs[QGC_RX_PACK0 / 4] = QGC_PACK_4X8_0;
    model.regs[QGC_TX_PACK1 / 4] = model.regs[QGC_RX_PACK1 / 4] = QGC_PACK_4X8_1;
    model.regs[QGC_TX_WORD_LEN / 4] = model.regs[QGC_RX_WORD_LEN / 4] = 8;
    model.regs[QGC_M_CLK_CFG / 4] = model.regs[QGC_S_CLK_CFG / 4] = 0x101;
    model.regs[QGC_HW_PARAM_TX / 4] = model.regs[QGC_HW_PARAM_RX / 4] = (32U << 24) | (16U << 16);
    model.regs[QGC_RX_STALE_CNT / 4] = 160;
}

static void setup(void)
{
    DeviceTreeNode node = 0;
    qcom_geni_setup(&node);
    assert(model.maps == 1 && model.registrations == 1 && registered == &geni_serial_functions);
    assert(model.mapped_physical == dt.soc_base + dt.reg[0] && model.mapped_size == dt.reg[1]);
    assert(registered->device == SERIAL_QCOM_GENI && registered->try_receive);
    assert(!registered->receive_ready && !registered->receive_data);
    assert(!model.reads && !model.writes); /* setup never touches retained hardware */
}

static void device_tree_admission(void)
{
    for (unsigned kind = 0; kind < 21; ++kind) {
        reset();
        switch (kind) {
        case 0: dt.missing_handoff = true; break;
        case 1: dt.handoff_size = 0; break;
        case 2: dt.handoff_size = 3; break;
        case 3: dt.handoff_size = 8; break;
        case 4: dt.handoff = 0; break;
        case 5: dt.handoff = 2; break;
        case 6: dt.status = "disabled"; dt.status_size = 9; break;
        case 7: dt.status_size = 4; break; /* unterminated "okay" */
        case 8: dt.status = "okX"; dt.status_size = 3; break;
        case 9: dt.status_size = 0; break;
        case 10: dt.missing_reg = true; break;
        case 11: dt.reg_size = 8; break;
        case 12: dt.reg_size = 24; break;
        case 13: dt.reg[0]++; break;
        case 14: dt.reg[1] = 0xfff; break;
        case 15: dt.reg[1] = 0x10001; break;
        case 16: dt.reg[0] = UINT64_MAX - 3; break;
        case 17: dt.reg[0] = 1ULL << 40; break;
        case 18: dt.soc_base = (1ULL << 40) - dt.reg[0]; break;
        case 19: dt.soc_base = (1ULL << 40) - dt.reg[0] - 0x800; break;
        case 20: dt.soc_base = UINT64_MAX; break;
        }
        DeviceTreeNode node = 0;
        qcom_geni_setup(&node);
        assert(!registered && !model.maps && !model.registrations && !model.reads && !model.writes);
    }
    reset(); setup();
    reset(); dt.status = "ok"; dt.status_size = 3; setup();
    reset(); dt.missing_status = true; setup();
    reset(); dt.reg[1] = 0x10000; setup();
    reset(); dt.soc_base = (1ULL << 40) - dt.reg[0] - dt.reg[1]; setup();
    reset(); dt.mapping_failure = true;
    DeviceTreeNode node = 0; qcom_geni_setup(&node);
    assert(model.maps == 1 && !model.registrations && !registered && !model.reads);
}

static void reentrant_console(void)
{
    reset(); setup();
    model.inject_reentry = true;
    registered->init();
    assert(geni_console.enabled && model.reentries > 20);
    model.rx[0] = 0; model.rx[1] = 'B'; model.rx[2] = 'C'; model.rx[3] = 'D'; model.rx_size = 4;
    assert(registered->try_receive() == 0); /* NUL is a valid received byte */
    assert(geni_console.rx_count == 3);
    registered->transmit_data('X');
    assert(geni_console.rx_count == 3 && geni_console.enabled);
    assert(registered->try_receive() == 'B');
    assert(registered->try_receive() == 'C');
    assert(registered->try_receive() == 'D');
    assert(registered->try_receive() == -1);
    assert(model.tx_size == 1 && model.tx[0] == 'X');
    model.inject_reentry = false;
    /* Exercise the optional callback as the multiplexer does: contention is
     * -1, never a fabricated NUL and never a paired ready/data race. */
    const unsigned reads = model.reads, writes = model.writes;
    __atomic_test_and_set(&geni_lock, __ATOMIC_ACQUIRE);
    registered->init(); registered->transmit_data('Y');
    assert(registered->try_receive() == -1 && registered->transmit_ready());
    assert(model.reads == reads && model.writes == writes && geni_console.enabled);
    __atomic_clear(&geni_lock, __ATOMIC_RELEASE);
    registered->transmit_data('Z');
    assert(model.tx_size == 2 && model.tx[1] == 'Z');
}

static pthread_barrier_t barrier;
static uint8_t received[1024];
static void start_together(void)
{
    const int result = pthread_barrier_wait(&barrier);
    assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
}
static void *transmitter(void *argument)
{
    start_together();
    for (unsigned i = 0; i < 512; ++i) {
        registered->transmit_data((uint8_t)(uintptr_t)argument);
        sched_yield();
    }
    /* Require actual commands from every writer, even when the deliberately
     * slow MMIO owner causes most first-pass trylock attempts to drop. */
    for (unsigned i = 0; commands_sent < 8 && i < 1000000; ++i) {
        registered->transmit_data((uint8_t)(uintptr_t)argument);
        sched_yield();
    }
    assert(commands_sent >= 8);
    return NULL;
}
static void *receiver(void *argument)
{
    (void)argument; start_together();
    unsigned count = 0;
    for (unsigned attempts = 0; count < sizeof(received) && attempts < 1000000; ++attempts) {
        const int value = registered->try_receive();
        if (value >= 0) received[count++] = (uint8_t)value;
        else sched_yield();
    }
    assert(count == sizeof(received));
    return NULL;
}
static void concurrent_console(void)
{
    reset(); setup(); registered->init();
    model.rx_size = sizeof(received);
    for (unsigned i = 0; i < model.rx_size; ++i) model.rx[i] = (uint8_t)(i * 37);
    model.concurrent = true;
    assert(!pthread_barrier_init(&barrier, NULL, 5));
    pthread_t tx[4], rx;
    for (unsigned i = 0; i < 4; ++i)
        assert(!pthread_create(&tx[i], NULL, transmitter, (void *)(uintptr_t)('A' + i)));
    assert(!pthread_create(&rx, NULL, receiver, NULL));
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_join(tx[i], NULL));
    assert(!pthread_join(rx, NULL)); assert(!pthread_barrier_destroy(&barrier));
    model.concurrent = false;
    assert(geni_console.enabled && !geni_console.rx_count && model.rx_offset == model.rx_size);
    assert(!memcmp(model.rx, received, sizeof(received)));
    assert(model.tx_size >= 32 && model.tx_size <= 4 * (512 + 8));
    unsigned counts[4] = {0};
    for (unsigned i = 0; i < model.tx_size; ++i) {
        assert(model.tx[i] >= 'A' && model.tx[i] <= 'D'); ++counts[model.tx[i] - 'A'];
    }
    for (unsigned i = 0; i < 4; ++i) assert(counts[i] >= 8);
    /* A final uncontended command proves the console was not disabled or
     * left with a half-written transfer. */
    registered->transmit_data('Z');
    assert(model.tx[model.tx_size - 1] == 'Z');
}

int main(void)
{
    alarm(20); /* A regression to a recursive spin lock must fail, not hang CI. */
    device_tree_admission(); reentrant_console(); concurrent_console();
    alarm(0);
    puts("GENI adapter: explicit DT admission, optional RX, reentrant init/TX/RX and concurrent TX/RX passed");
    return 0;
}
