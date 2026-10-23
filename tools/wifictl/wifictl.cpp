// SPDX-License-Identifier: BSD-2-Clause
#include <IOKitUser.h>
#include <WLANProtocol.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1],"--schema")) {
        puts("protocol=1; info: version,CE-count,DMA-bits,MMIO-base,MMIO-size,missing-mask; status: state,resources,generation,last-result,missing-mask,native-interface; power <0|1>: privileged transport request");
        return 0;
    }
    const bool info = argc == 2 && !strcmp(argv[1],"info");
    const bool status = argc == 2 && !strcmp(argv[1],"status");
    const bool power = argc == 3 && !strcmp(argv[1],"power") && (!strcmp(argv[2],"0") || !strcmp(argv[2],"1"));
    if (!info && !status && !power) {
        fputs("usage: wifictl info | status | power <0|1> | --schema\n",stderr); return 2;
    }
    CFMutableDictionaryRef matching = IOServiceMatching("QcomWLAN");
    if (!matching) return 1;
    mach_port_t iterator = MACH_PORT_NULL;
    kern_return_t result = IOServiceGetMatchingServices(MACH_PORT_NULL,matching,&iterator);
    if (result) { fprintf(stderr,"matching failed: 0x%x\n",result); return 1; }
    // Choose before opening; zero or multiple services must never mutate a
    // device selected merely by IORegistry enumeration order.
    mach_port_t selected = MACH_PORT_NULL; unsigned found = 0;
    for (mach_port_t service; (service = IOIteratorNext(iterator));) {
        ++found;
        if (!selected) selected = service; else IOObjectRelease(service);
    }
    IOObjectRelease(iterator);
    if (found != 1) {
        if (selected) IOObjectRelease(selected);
        fprintf(stderr,"expected one QcomWLAN transport, found %u\n",found); return 1;
    }
    mach_port_t connection = MACH_PORT_NULL;
    result = IOServiceOpen(selected,mach_task_self(),WLAN::DiagnosticClient,&connection);
    IOObjectRelease(selected);
    if (result) { fprintf(stderr,"open failed (administrator required): 0x%x\n",result); return 1; }
    uint64_t values[WLAN::InfoScalars] = {}; uint32_t count = WLAN::InfoScalars;
    result = IOConnectCallScalarMethod(connection,WLAN::GetInfo,nullptr,0,values,&count);
    if (!result && (count != WLAN::InfoScalars || values[0] != WLAN::ProtocolVersion)) result = KERN_INVALID_ARGUMENT;
    if (!result && info) printf("protocol=%llu chipset=WCN3990 bus=SNOC CE=%llu DMA=%llu MMIO=0x%llx/0x%llx missing=0x%llx\n",
        values[0],values[1],values[2],values[3],values[4],values[5]);
    if (!result && status) {
        count = WLAN::StatusScalars;
        result = IOConnectCallScalarMethod(connection,WLAN::GetStatus,nullptr,0,values,&count);
        if (!result && (count != WLAN::StatusScalars || values[0] > uint32_t(WLAN::State::Fault))) result = KERN_INVALID_ARGUMENT;
        if (!result) printf("state=%s resources=0x%llx generation=%llu result=%llu missing=0x%llx native-interface=%llu\n",
            WLAN::stateName(WLAN::State(values[0])),values[1],values[2],values[3],values[4],values[5]);
    }
    if (!result && power) {
        uint64_t input = uint64_t(argv[2][0] - '0'); count = 0;
        result = IOConnectCallScalarMethod(connection,WLAN::SetPower,&input,1,nullptr,&count);
        if (!result && count) result = KERN_INVALID_ARGUMENT;
        if (!result) puts("transport power request completed");
    }
    IOServiceClose(connection);
    if (result) { fprintf(stderr,"request failed: 0x%x (the initial framework has no hardware backend)\n",result); return 1; }
    return 0;
}
