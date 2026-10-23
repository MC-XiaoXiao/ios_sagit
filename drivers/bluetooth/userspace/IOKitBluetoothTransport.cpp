// SPDX-License-Identifier: BSD-2-Clause
#include "IOKitBluetoothTransport.h"

namespace Bluetooth {
kern_return_t IOKitBluetoothTransport::call(uint32_t selector, const uint64_t *input, uint32_t inputs,
    uint64_t *output, uint32_t outputs) {
    if (!connection) return KERN_INVALID_ARGUMENT;
    uint32_t count = outputs;
    const auto result = IOConnectCallScalarMethod(connection,selector,input,inputs,output,&count);
    return result ? result : count == outputs ? KERN_SUCCESS : KERN_INVALID_ARGUMENT;
}
kern_return_t IOKitBluetoothTransport::open(uint32_t client) {
    if (connection) return KERN_INVALID_ARGUMENT;
    if (client != DiagnosticClient && client != BootstrapClient) return KERN_INVALID_ARGUMENT;
    auto *matching = IOServiceMatching("QcomBluetooth");
    if (!matching) return KERN_RESOURCE_SHORTAGE;
    mach_port_t iterator = MACH_PORT_NULL;
    auto result = IOServiceGetMatchingServices(MACH_PORT_NULL,matching,&iterator);
    if (result) return result; // matching dictionary was consumed
    mach_port_t selected = MACH_PORT_NULL;
    unsigned found = 0;
    for (mach_port_t service; (service = IOIteratorNext(iterator));) {
        ++found;
        if (!selected) selected = service; else IOObjectRelease(service);
    }
    IOObjectRelease(iterator);
    if (found != 1) {
        if (selected) IOObjectRelease(selected);
        return KERN_FAILURE;
    }
    result = IOServiceOpen(selected,mach_task_self(),client,&connection);
    IOObjectRelease(selected);
    if (result) { connection = MACH_PORT_NULL; return result; }
    uint64_t values[InfoScalars] = {};
    result = info(values);
    if (result) close();
    return result;
}
void IOKitBluetoothTransport::close() {
    if (connection) { IOServiceClose(connection); connection = MACH_PORT_NULL; }
}
kern_return_t IOKitBluetoothTransport::info(uint64_t (&v)[InfoScalars]) {
    const auto result = call(GetInfo,nullptr,0,v,InfoScalars);
    if (result) return result;
    return v[0] == ProtocolVersion && v[1] == UARTBase && v[2] == UARTSize && v[3] == UARTInterrupt &&
        v[4] == InitialBaud && v[5] == OperationalBaud && v[6] == MaxACLPayload && !(v[7] & ~uint64_t(InitialMissing))
        ? KERN_SUCCESS : KERN_INVALID_ARGUMENT;
}
kern_return_t IOKitBluetoothTransport::status(uint64_t (&v)[StatusScalars]) {
    const auto result = call(GetStatus,nullptr,0,v,StatusScalars);
    if (result) return result;
    return v[0] <= uint32_t(State::Fault) && !(v[1] & ~uint64_t(AllResources)) &&
        v[3] <= uint32_t(Result::Ignored) && !(v[4] & ~uint64_t(InitialMissing)) && v[5] <= 1 && !v[6] && !v[7]
        ? KERN_SUCCESS : KERN_INVALID_ARGUMENT;
}
kern_return_t IOKitBluetoothTransport::power(bool on) {
    const uint64_t value = on ? 1 : 0;
    return call(SetPower,&value,1,nullptr,0);
}
kern_return_t IOKitBluetoothTransport::bootstrap(uint64_t (&v)[BootstrapScalars]) {
    return call(GetBootstrap,nullptr,0,v,BootstrapScalars);
}
kern_return_t IOKitBluetoothTransport::bootstrapPower(bool on, uint64_t expected, uint64_t& accepted) {
    const uint64_t input[]={on?1ULL:0ULL,expected};
    return call(BootstrapSetPower,input,2,&accepted,1);
}
}
