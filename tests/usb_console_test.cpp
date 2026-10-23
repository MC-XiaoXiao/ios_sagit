// SPDX-License-Identifier: BSD-2-Clause
#include <cassert>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
extern "C" {
#include <pexpert/arm/usb_console.h>
}

static usb_console_buffer b;
int main()
{
    unsigned char out[USB_CONSOLE_TX_BYTES + 10] = {};
    usb_console_putc(&b, 'x');
    usb_console_online(&b, true);
    assert(!usb_console_read_tx(&b, out, sizeof(out)));
    assert(!usb_console_write_rx(&b, "x", 1));
    usb_console_select(&b, true);
    usb_console_online(&b, false);
    usb_console_putc(&b, 'a');  // Early boot output persists without a host.
    assert(!usb_console_read_tx(&b, out, sizeof(out)));
    usb_console_online(&b, true);
    assert(usb_console_read_tx(&b, out, sizeof(out)) == 1 && out[0] == 'a');
    assert(usb_console_write_rx(&b, "hi", 2) == 2);
    assert(usb_console_getc(&b) == 'h');
    usb_console_online(&b, false);
    usb_console_online(&b, true);
    assert(usb_console_getc(&b) == -1); // Previous session's 'i' is discarded.

    // Disconnect under console lock must still revoke a buffered command.
    assert(usb_console_write_rx(&b, "old", 3) == 3);
    assert(usb_console_lock(&b));
    usb_console_online(&b, false);
    usb_console_online(&b, true);
    usb_console_putc(&b, 'x'); // Nested panic output must never spin.
    assert(!usb_console_read_tx(&b, out, 1));
    assert(!usb_console_write_rx(&b, "x", 1));
    usb_console_unlock(&b);
    assert(usb_console_getc(&b) == -1);

    // Full TX is drop-new, preserving the first failure rather than hanging.
    b.tx_read = b.tx_write = UINT32_MAX - 100;
    for (unsigned i = 0; i < USB_CONSOLE_TX_BYTES + 7; ++i)
        usb_console_putc(&b, (unsigned char)i);
    assert(b.tx_dropped == 7);
    assert(usb_console_read_tx(&b, out, sizeof(out)) == USB_CONSOLE_TX_BYTES);
    for (unsigned i = 0; i < USB_CONSOLE_TX_BYTES; ++i)
        assert(out[i] == (unsigned char)i);
    assert(!usb_console_read_tx(&b, nullptr, 1));
    assert(!usb_console_read_tx(&b, out, 0));
    assert(usb_console_getc(&b) == -1);

    memset(out, 0xfe, sizeof(out));
    b.rx_read = b.rx_write = UINT32_MAX - 100;
    assert(usb_console_write_rx(&b, out, sizeof(out)) == USB_CONSOLE_RX_BYTES);
    assert(!usb_console_write_rx(&b, "a", 1));
    for (unsigned i = 0; i < USB_CONSOLE_RX_BYTES; ++i)
        assert(usb_console_getc(&b) == 0xfe);
    assert(usb_console_getc(&b) == -1);
    assert(!usb_console_write_rx(&b, nullptr, 10));
    assert(!usb_console_write_rx(&b, out, 0));

    // Multiple logging CPUs cannot corrupt the producer cursors; loss on
    // contention is intentional and output must remain bounded.
    std::vector<std::thread> writers;
    for (int n = 0; n < 4; ++n)
        writers.emplace_back([n] {
            for (unsigned i = 0; i < 4000; ++i)
                usb_console_putc(&b, (unsigned char)('A' + n));
        });
    for (auto &w : writers) w.join();
    unsigned count = usb_console_read_tx(&b, out, sizeof(out));
    assert(count > 0 && count <= 16000);
    for (unsigned i = 0; i < count; ++i) assert(out[i] >= 'A' && out[i] <= 'D');
    puts("USB console: bounded TX/RX, wraparound, disconnection, contention and multiple writers passed");
}
