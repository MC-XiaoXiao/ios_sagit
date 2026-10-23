// SPDX-License-Identifier: BSD-2-Clause
#include "IOKitBluetoothTransport.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1],"--schema")) {
        puts("protocol=1; info: version,UART-base,UART-size,SPI,initial-baud,operational-baud,ACL-limit,missing-mask; status: state,resources,generation,last-result,missing-mask,native-stack,reserved,reserved; power <0|1>");
        return 0;
    }
    const bool info = argc == 2 && !strcmp(argv[1],"info");
    const bool status = argc == 2 && !strcmp(argv[1],"status");
    const bool power = argc == 3 && !strcmp(argv[1],"power") && (!strcmp(argv[2],"0") || !strcmp(argv[2],"1"));
    if (!info && !status && !power) {
        fputs("usage: bluetoothctl info | status | power <0|1> | --schema\n",stderr); return 2;
    }
    Bluetooth::IOKitBluetoothTransport transport;
    auto result = transport.open();
    if (result) { fprintf(stderr,"open failed: 0x%x (one QcomBluetooth service and administrator access required)\n",result); return 1; }
    uint64_t values[Bluetooth::InfoScalars] = {};
    if (info) {
        result = transport.info(values);
        if (!result) printf("protocol=%llu chipset=WCN3990 bus=UART-H4-IBS MMIO=0x%llx/0x%llx SPI=%llu baud=%llu/%llu ACL-limit=%llu missing=0x%llx\n",
            values[0],values[1],values[2],values[3],values[4],values[5],values[6],values[7]);
    } else if (status) {
        result = transport.status(values);
        if (!result) printf("state=%s resources=0x%llx generation=%llu result=%llu missing=0x%llx native-stack=%llu\n",
            Bluetooth::stateName(Bluetooth::State(values[0])),values[1],values[2],values[3],values[4],values[5]);
    } else result = transport.power(argv[2][0] == '1');
    if (result) { fprintf(stderr,"request failed: 0x%x; inspect status and the firmware/resource policy\n",result); return 1; }
    if (power) puts("transport power request accepted; use status to observe completion");
    return 0;
}
