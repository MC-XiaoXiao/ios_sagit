/* Polled SDM835 console, included by pe_serial.c. No BAM or UART IRQs. */
#include <pexpert/arm64/qcom_uartdm.h>
#ifndef QCOM_UARTDM_RELAX
#define QCOM_UARTDM_RELAX() __asm__ volatile("yield")
#endif

static vm_offset_t qcom_uartdm_base;
static uint32_t qcom_uartdm_rx_word;
static unsigned int qcom_uartdm_rx_count;
static unsigned int qcom_uartdm_rx_consumed;
static unsigned int qcom_uartdm_rx_remaining;
static bool qcom_uartdm_stale_requested;
static uint32_t qcom_uartdm_tx_lock;

static uint32_t
qcom_uartdm_read(unsigned int reg)
{
	return ml_io_read32(qcom_uartdm_base + reg);
}

static void
qcom_uartdm_write(unsigned int reg, uint32_t value)
{
	ml_io_write32(qcom_uartdm_base + reg, value);
	__builtin_arm_dsb(DSB_SY);
}

static void
qcom_uartdm_init(void)
{
	/* Keep the firmware's CSR divisor, GCC clock and TLMM pinmux. */
	qcom_uartdm_write(QUDM_IMR, 0);
	qcom_uartdm_write(QUDM_DMEN, 0);
	qcom_uartdm_write(QUDM_CR, QUDM_DISABLE);
	qcom_uartdm_write(QUDM_CR, QUDM_RESET_RX);
	qcom_uartdm_write(QUDM_CR, QUDM_RESET_TX);
	qcom_uartdm_write(QUDM_CR, QUDM_RESET_ERR);
	qcom_uartdm_write(QUDM_CR, QUDM_STALE_OFF);
	qcom_uartdm_write(QUDM_CR, QUDM_RESET_STALE);
	qcom_uartdm_write(QUDM_MR1, 0); /* No RTS/CTS dependency. */
	qcom_uartdm_write(QUDM_MR2, QUDM_8N1);
	qcom_uartdm_write(QUDM_IPR, 1);
	qcom_uartdm_write(QUDM_RFWR, 0);
	qcom_uartdm_write(QUDM_TFWR, 0);
	qcom_uartdm_write(QUDM_DMRX, QUDM_RX_LIMIT);
	qcom_uartdm_write(QUDM_CR, QUDM_ENABLE);
	qcom_uartdm_rx_count = 0;
	qcom_uartdm_rx_consumed = 0;
	qcom_uartdm_rx_remaining = 0;
	qcom_uartdm_stale_requested = false;
}

static unsigned int
qcom_uartdm_transmit_ready(void)
{
	/* Each FIFO word is a separate one-byte transfer; don't overlap NCF_TX. */
	return (qcom_uartdm_read(QUDM_SR) & QUDM_TX_EMPTY) != 0;
}

static void
qcom_uartdm_transmit_data(uint8_t c)
{
	/* kprintf and the console have separate upstream locks. UARTDM's
	 * count/status/FIFO sequence must still be one hardware transaction.
	 * IRQ masking prevents a same-CPU interrupt from taking our lock. */
	bool locked = kernel_debugger_entry_count == 0;
	bool state = false;
	if (locked) {
		state = ml_set_interrupts_enabled(false);
		while (__atomic_exchange_n(&qcom_uartdm_tx_lock, 1, __ATOMIC_ACQUIRE)) {
			ml_set_interrupts_enabled(state);
			QCOM_UARTDM_RELAX();
			ml_set_interrupts_enabled(false);
		}
	}
	/* transmit_ready was queried before taking the lock; recheck here. */
	while (!(qcom_uartdm_read(QUDM_SR) & QUDM_TX_EMPTY)) {
		QCOM_UARTDM_RELAX();
	}
	qcom_uartdm_write(QUDM_NCF_TX, 1);
	(void)qcom_uartdm_read(QUDM_NCF_TX); /* Complete the count write. */
	while (!(qcom_uartdm_read(QUDM_SR) & QUDM_TX_READY)) {
		QCOM_UARTDM_RELAX();
	}
	qcom_uartdm_write(QUDM_FIFO, c);
	if (locked) {
		__atomic_store_n(&qcom_uartdm_tx_lock, 0, __ATOMIC_RELEASE);
		ml_set_interrupts_enabled(state);
	}
}

static unsigned int
qcom_uartdm_receive_ready(void)
{
	if (qcom_uartdm_rx_count) {
		return 1;
	}
	uint32_t status = qcom_uartdm_read(QUDM_SR);
	if (status & QUDM_OVERRUN) {
		qcom_uartdm_write(QUDM_CR, QUDM_RESET_ERR);
	}
	if (!qcom_uartdm_stale_requested && (status & QUDM_RX_READY)) {
		qcom_uartdm_rx_word = qcom_uartdm_read(QUDM_FIFO);
		qcom_uartdm_rx_count = 4;
		qcom_uartdm_rx_consumed += 4;
	} else {
		/* RX_READY describes complete words. Flush a short packing buffer
		 * explicitly so a single keystroke can be returned immediately. */
		if (!qcom_uartdm_stale_requested) {
			unsigned int packed = (qcom_uartdm_read(QUDM_RXFS) >> 7) & 7;
			if (!packed) {
				return 0;
			}
			qcom_uartdm_write(QUDM_CR, QUDM_FORCE_STALE);
			qcom_uartdm_stale_requested = true;
		}
		/* Stale completion and the snapshot, rather than the earlier RXFS
		 * sample, account for bytes arriving while the flush is in flight. */
		if (!(qcom_uartdm_read(QUDM_ISR) & QUDM_RX_STALE)) {
			return 0;
		}
		if (!qcom_uartdm_rx_remaining) {
			uint32_t snapshot = qcom_uartdm_read(QUDM_RX_SNAP);
			if (snapshot < qcom_uartdm_rx_consumed || snapshot > QUDM_RX_LIMIT) {
				panic("UARTDM: invalid receive snapshot");
			}
			qcom_uartdm_rx_remaining = snapshot - qcom_uartdm_rx_consumed;
		}
		if (!qcom_uartdm_rx_remaining) {
			qcom_uartdm_write(QUDM_CR, QUDM_RESET_STALE);
			qcom_uartdm_write(QUDM_DMRX, QUDM_RX_LIMIT);
			qcom_uartdm_rx_consumed = 0;
			qcom_uartdm_stale_requested = false;
			return 0;
		}
		qcom_uartdm_rx_word = qcom_uartdm_read(QUDM_FIFO);
		qcom_uartdm_rx_count = qcom_uartdm_rx_remaining < 4 ? qcom_uartdm_rx_remaining : 4;
		qcom_uartdm_rx_remaining -= qcom_uartdm_rx_count;
		qcom_uartdm_rx_consumed += qcom_uartdm_rx_count;
		if (!qcom_uartdm_rx_remaining) {
			qcom_uartdm_write(QUDM_CR, QUDM_RESET_STALE);
			qcom_uartdm_write(QUDM_DMRX, QUDM_RX_LIMIT);
			qcom_uartdm_rx_consumed = 0;
			qcom_uartdm_stale_requested = false;
		}
	}
	return 1;
}

static uint8_t
qcom_uartdm_receive_data(void)
{
	uint8_t c = (uint8_t)qcom_uartdm_rx_word;
	qcom_uartdm_rx_word >>= 8;
	qcom_uartdm_rx_count--;
	return c;
}

SECURITY_READ_ONLY_LATE(static struct pe_serial_functions) qcom_uartdm_serial_functions = {
	.init = qcom_uartdm_init,
	.transmit_ready = qcom_uartdm_transmit_ready,
	.transmit_data = qcom_uartdm_transmit_data,
	.receive_ready = qcom_uartdm_receive_ready,
	.receive_data = qcom_uartdm_receive_data,
	.device = SERIAL_QCOM_UARTDM
};

static void
qcom_uartdm_setup(const DeviceTreeNode *const node)
{
	const uint64_t *reg;
	unsigned int size;
	if (SecureDTGetProperty(node, "reg", (const void **)&reg, &size) != kSuccess ||
	    size != 2 * sizeof(uint64_t) || reg[1] < QUDM_FIFO + sizeof(uint32_t) ||
	    (reg[0] & 3) || reg[0] > UINT64_MAX - pe_arm_get_soc_base_phys() ||
	    reg[1] > UINT64_MAX - (reg[0] + pe_arm_get_soc_base_phys())) {
		panic("UARTDM requires a valid Apple DT reg {uint64 offset, uint64 size}");
	}
	qcom_uartdm_base = ml_io_map(pe_arm_get_soc_base_phys() + reg[0], reg[1]);
	if (!qcom_uartdm_base) {
		panic("Unable to map UARTDM registers");
	}
	register_serial_functions(&qcom_uartdm_serial_functions);
}
