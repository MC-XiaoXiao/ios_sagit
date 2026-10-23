/* SPDX-License-Identifier: BSD-2-Clause */
static qcom_geni_console_t geni_console;
static vm_offset_t geni_base;
static unsigned char geni_lock;

/* Console, kprintf and debugger entry can overlap, including on this CPU.
 * Drop a contended operation instead of waiting for an interrupted owner. */
static int
geni_try_lock(void)
{
	return !__atomic_test_and_set(&geni_lock, __ATOMIC_ACQUIRE);
}

static void
geni_unlock(void)
{
	__atomic_clear(&geni_lock, __ATOMIC_RELEASE);
}

static uint32_t
geni_read32(void *context, uint32_t offset)
{
	return ml_io_read32((uintptr_t)context + offset);
}

static void
geni_write32(void *context, uint32_t offset, uint32_t value)
{
	ml_io_write32((uintptr_t)context + offset, value);
	__builtin_arm_dsb(DSB_SY);
}

static void
geni_init(void)
{
	if (geni_try_lock()) {
		(void)qcom_geni_console_init(&geni_console, (void *)geni_base, geni_read32, geni_write32);
		geni_unlock();
	}
}

static unsigned
geni_transmit_ready(void)
{
	/* putc owns its bounded hardware wait; never hang the serial multiplexer. */
	return 1;
}

static void
geni_transmit_data(uint8_t c)
{
	if (geni_try_lock()) {
		(void)qcom_geni_console_putc(&geni_console, c);
		geni_unlock();
	}
}

static int
geni_receive_character(void)
{
	int character = -1;
	if (geni_try_lock()) {
		character = qcom_geni_console_getc(&geni_console);
		geni_unlock();
	}
	return character;
}

static struct pe_serial_functions geni_serial_functions = {
	.init = geni_init,
	.transmit_ready = geni_transmit_ready,
	.transmit_data = geni_transmit_data,
	.try_receive = geni_receive_character,
	.device = SERIAL_QCOM_GENI
};

static void
qcom_geni_setup(const DeviceTreeNode *node)
{
	const uint32_t *handoff;
	const uint64_t *reg;
	const char *status;
	unsigned int size;
	/* Explicit board contract: firmware quiesced its UART user and retained
	 * powered clocks, pins, microcode and the configured baud rate. */
	if (SecureDTGetProperty(node, "qcom,geni-console-handoff", (const void **)&handoff, &size) != kSuccess ||
	    size != 4 || *handoff != 1) {
		return;
	}
	if (SecureDTGetProperty(node, "status", (const void **)&status, &size) == kSuccess &&
	    !((size == 5 && !memcmp(status, "okay", 5)) || (size == 3 && !memcmp(status, "ok", 3)))) {
		return;
	}
	if (SecureDTGetProperty(node, "reg", (const void **)&reg, &size) != kSuccess || size != 16) {
		return;
	}
	uint64_t address, bytes;
	memcpy(&address, reg, 8);
	memcpy(&bytes, reg + 1, 8);
	uint64_t base = pe_arm_get_soc_base_phys();
	if ((address & 3) || bytes < 0x1000 || bytes > 0x10000 ||
	    address >= (1ULL << 40) || base >= (1ULL << 40) - address ||
	    bytes > (1ULL << 40) - base - address) {
		return;
	}
	geni_base = ml_io_map(base + address, bytes);
	if (geni_base) {
		register_serial_functions(&geni_serial_functions);
	}
}
