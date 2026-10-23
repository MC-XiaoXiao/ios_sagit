/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef _PEXPERT_ARM_USB_CONSOLE_H_
#define _PEXPERT_ARM_USB_CONSOLE_H_

#include <stdint.h>
#include <stdbool.h>

/*
 * Bounded console storage, independent of IOKit and USB DMA. A producer never
 * waits for a consumer, an interrupt, a lock owner or a connected host.
 * Try-locking also makes nested panic output safe: contention drops bytes.
 * Every access to ring state is protected by the same acquire/release lock.
 */
#define USB_CONSOLE_TX_BYTES 65536u
#define USB_CONSOLE_RX_BYTES 4096u
struct usb_console_buffer {
	unsigned char lock;
	bool selected, online;
	uint32_t generation, rx_generation;
	uint32_t tx_read, tx_write, rx_read, rx_write;
	uint64_t tx_dropped;
	uint8_t tx[USB_CONSOLE_TX_BYTES], rx[USB_CONSOLE_RX_BYTES];
};

static inline bool
usb_console_lock(struct usb_console_buffer *b)
{
	return !__atomic_test_and_set(&b->lock, __ATOMIC_ACQUIRE);
}

static inline void
usb_console_unlock(struct usb_console_buffer *b)
{
	__atomic_clear(&b->lock, __ATOMIC_RELEASE);
}

static inline void
usb_console_select(struct usb_console_buffer *b, bool selected)
{
	/* Called once on the boot CPU, before console and USB workers exist. */
	b->selected = selected;
}

static inline void
usb_console_online(struct usb_console_buffer *b, bool online)
{
	/*
	 * Publish the transport flag independently: disconnect must revoke RX
	 * even when it races a console writer. RX indices remain lock protected.
	 */
	__atomic_store_n(&b->online, online, __ATOMIC_RELEASE);
	if (!online) {
		__atomic_add_fetch(&b->generation, 1u, __ATOMIC_RELEASE);
	}
}

static inline void
usb_console_putc(struct usb_console_buffer *b, uint8_t c)
{
	if (!b->selected || !usb_console_lock(b)) {
		return;
	}
	if ((uint32_t)(b->tx_write - b->tx_read) < USB_CONSOLE_TX_BYTES) {
		b->tx[b->tx_write++ & (USB_CONSOLE_TX_BYTES - 1)] = c;
	} else {
		b->tx_dropped++;
	}
	usb_console_unlock(b);
}

static inline unsigned
usb_console_read_tx(struct usb_console_buffer *b, void *bytes, unsigned count)
{
	if (!bytes || !count || !b->selected ||
	    !__atomic_load_n(&b->online, __ATOMIC_ACQUIRE) || !usb_console_lock(b)) {
		return 0;
	}
	uint8_t *out = (uint8_t *)bytes;
	unsigned done = 0;
	while (done < count && b->tx_read != b->tx_write) {
		out[done++] = b->tx[b->tx_read++ & (USB_CONSOLE_TX_BYTES - 1)];
	}
	usb_console_unlock(b);
	return done;
}

static inline unsigned
usb_console_write_rx(struct usb_console_buffer *b, const void *bytes, unsigned count)
{
	if (!bytes || !count || !b->selected ||
	    !__atomic_load_n(&b->online, __ATOMIC_ACQUIRE) || !usb_console_lock(b)) {
		return 0;
	}
	const uint8_t *in = (const uint8_t *)bytes;
	unsigned done = 0;
	uint32_t generation = __atomic_load_n(&b->generation, __ATOMIC_ACQUIRE);
	if (b->rx_generation != generation) {
		b->rx_read = b->rx_write;
		b->rx_generation = generation;
	}
	if (__atomic_load_n(&b->online, __ATOMIC_ACQUIRE)) {
		while (done < count && (uint32_t)(b->rx_write - b->rx_read) < USB_CONSOLE_RX_BYTES) {
			b->rx[b->rx_write++ & (USB_CONSOLE_RX_BYTES - 1)] = in[done++];
		}
	}
	usb_console_unlock(b);
	return done;
}

static inline int
usb_console_getc(struct usb_console_buffer *b)
{
	if (!b->selected || !usb_console_lock(b)) {
		return -1;
	}
	int c = -1;
	uint32_t generation = __atomic_load_n(&b->generation, __ATOMIC_ACQUIRE);
	if (b->rx_generation != generation) {
		b->rx_read = b->rx_write;
		b->rx_generation = generation;
	}
	if (!__atomic_load_n(&b->online, __ATOMIC_ACQUIRE)) {
		b->rx_read = b->rx_write;
	} else if (b->rx_read != b->rx_write) {
		c = b->rx[b->rx_read++ & (USB_CONSOLE_RX_BYTES - 1)];
	}
	usb_console_unlock(b);
	return c;
}
#endif
