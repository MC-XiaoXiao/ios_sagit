// SPDX-License-Identifier: BSD-2-Clause
#pragma once

// Exported by the project's ARM serial console patch. These calls never sleep
// or allocate. Capture selection is an early boot argument; transport online
// changes do not discard captured output. The driver consumes TX only with DTR.
extern "C" {
unsigned ml_usb_console_read_tx(void *, unsigned);
unsigned ml_usb_console_write_rx(const void *, unsigned);
void ml_usb_console_set_enabled(bool);
}
