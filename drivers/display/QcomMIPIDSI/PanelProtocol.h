/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef _MIPI_PANEL_PROTOCOL_H
#define _MIPI_PANEL_PROTOCOL_H
#include "MIPIDSIProtocol.h"
namespace MIPIDSI {
struct PanelCommand { uint8_t type; const uint8_t *data; uint16_t length, delayMS; };
// Binary DT data: type, reserved=0, LE16 payload length, LE16 delay-ms,
// LE16 reserved=0, payload. Validate the whole stream before sending anything.
inline bool panelSequence(const uint8_t *data, size_t size) {
    if (!data || !size || size > 65536) return false;
    size_t pos = 0; unsigned count = 0, delay = 0;
    while (pos < size) {
        if (++count > 256 || size - pos < 8) return false;
        uint16_t length = data[pos + 2] | uint16_t(data[pos + 3]) << 8;
        unsigned ms = data[pos + 4] | unsigned(data[pos + 5]) << 8;
        if (data[pos + 1] || data[pos + 6] || data[pos + 7] ||
            !length || length > MaxPayload || length > size - pos - 8 || ms > 500) return false;
        uint8_t type = data[pos];
        if (!((type == 0x05 || type == 0x13) && length == 1) &&
            !((type == 0x15 || type == 0x23) && length == 2) &&
            type != 0x29 && type != 0x39) return false;
        delay += ms;
        if (delay > 5000) return false;
        pos += 8 + length;
    }
    return pos == size;
}
}
#endif
