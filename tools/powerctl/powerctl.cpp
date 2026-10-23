// SPDX-License-Identifier: BSD-2-Clause
// Internal iOS diagnostic client, linked only against the SDK's actual IOKit
// exports. The scalar ABI comes from PowerUserProtocol.h, not a private PMGR.
#include <IOKitUser.h>
#include <PowerUserProtocol.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static bool number(const char *text, uint64_t &out) {
    if (!text || !*text || *text == '-') return false;
    errno = 0; char *end = nullptr; out = strtoull(text,&end,0); return !errno && end && !*end;
}
int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1],"--schema")) {
        puts("protocol=2; selectors: 0 info, 1 read(operation,id), 2 OSM limit(cluster,hz), 3 OSM frequency(cluster,hz), 4 OSM automatic(cluster,0/1); controllers: GCC=1 RPM=2 SPMI=3 MMCC=4 GPUCC=5 OSM=6; zero limit clears the ceiling");
        return 0;
    }
    bool list = argc == 2 && !strcmp(argv[1],"list");
    bool read = argc == 5 && !strcmp(argv[1],"read");
    bool limit = argc == 4 && !strcmp(argv[1],"limit");
    bool frequency = argc == 4 && !strcmp(argv[1],"frequency");
    bool automatic = argc == 4 && !strcmp(argv[1],"auto");
    uint64_t controller = ClockController::OSM, input[2] = {};
    if (!(list || read || limit || frequency || automatic) || (read && (!number(argv[2],controller) || !number(argv[3],input[0]) ||
        !number(argv[4],input[1]) || input[0] > UINT32_MAX || !PowerUser::readable(input[0]) || input[1] > UINT32_MAX)) ||
        ((limit || frequency || automatic) && (!number(argv[2],input[0]) || !number(argv[3],input[1]) || input[0] > 1 || (frequency && !input[1]) || (automatic && input[1]>1))) ||
        controller < ClockController::GCC || controller > ClockController::OSM) {
        fputs("usage: powerctl list | read <controller> <operation> <id> | limit <cluster> <Hz or 0> | frequency <cluster> <Hz> | auto <cluster> <0 or 1> | --schema\n",stderr); return 2;
    }
    CFMutableDictionaryRef matching = IOServiceMatching("GenericClockController");
    if (!matching) return 1;
    mach_port_t iterator = MACH_PORT_NULL;
    kern_return_t status = IOServiceGetMatchingServices(MACH_PORT_NULL,matching,&iterator);
    if (status) { fprintf(stderr,"matching failed: 0x%x\n",status); return 1; }
    unsigned found = 0; int result = 1; mach_port_t selected = MACH_PORT_NULL;
    for (mach_port_t service; (service = IOIteratorNext(iterator));) {
        mach_port_t connection = MACH_PORT_NULL;
        status = IOServiceOpen(service,mach_task_self(),0,&connection); IOObjectRelease(service);
        if (status) { if (list) fprintf(stderr,"open failed: 0x%x\n",status); continue; }
        uint64_t info[3] = {}; uint32_t count = 3;
        status = IOConnectCallScalarMethod(connection,PowerUser::GetInfo,nullptr,0,info,&count);
        if (!status && count == 3 && info[0] == 2 && (list || info[1] == controller)) {
            ++found;
            if (list) { printf("controller=%llu protocol=%llu capabilities=0x%llx\n",info[1],info[0],info[2]); result = 0; }
            else if (!selected) { selected = connection; connection = MACH_PORT_NULL; }
        }
        if (connection) IOServiceClose(connection);
    }
    IOObjectRelease(iterator);
    if (!list && found != 1) { if (selected) IOServiceClose(selected); fprintf(stderr,"expected one controller, found %u\n",found); return 1; }
    if (selected) {
        uint64_t value = 0; uint32_t count = 1;
        status = IOConnectCallScalarMethod(selected,read ? PowerUser::ReadResource : frequency ? PowerUser::SetCPUFrequency : automatic ? PowerUser::SetAutomatic : PowerUser::SetLimit,input,2,&value,&count);
        IOServiceClose(selected);
        if (status || count != 1) { fprintf(stderr,"request failed: 0x%x\n",status); result = 1; }
        else { printf("%llu\n",value); result = 0; }
    }
    return result;
}
