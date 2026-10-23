// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKitUser.h>
#include <BluetoothProtocol.h>

namespace Bluetooth {
// Internal IOKit management session, opened either by an administrator or the
// separately entitled BlueTool backend. It owns no raw firmware/register/HCI
// access; native H4 data uses the Apple UART socket. One thread per session.
class IOKitBluetoothTransport {
    mach_port_t connection = MACH_PORT_NULL;
    kern_return_t call(uint32_t, const uint64_t *, uint32_t, uint64_t *, uint32_t);
public:
    IOKitBluetoothTransport() = default;
    ~IOKitBluetoothTransport() { close(); }
    IOKitBluetoothTransport(const IOKitBluetoothTransport &) = delete;
    IOKitBluetoothTransport &operator=(const IOKitBluetoothTransport &) = delete;
    kern_return_t open(uint32_t client = DiagnosticClient);
    void close();
    kern_return_t info(uint64_t (&values)[InfoScalars]);
    kern_return_t status(uint64_t (&values)[StatusScalars]);
    kern_return_t power(bool on);
    kern_return_t bootstrap(uint64_t (&values)[BootstrapScalars]);
    kern_return_t bootstrapPower(bool on, uint64_t expected, uint64_t& accepted);
};
}
