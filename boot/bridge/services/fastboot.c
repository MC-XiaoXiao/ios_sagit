// SPDX-License-Identifier: BSD-2-Clause
#include "fastboot.h"
#include "../core/bridge.h"

#define MAX_DOWNLOAD (64U * 1024 * 1024)
#define MAX_RETAINED (144U * 1024 * 1024)
static EFI_GUID transport_guid = {0x74bd9fe0,0x8902,0x11e3,{0xb9,0xd3,0xf7,0x22,0x38,0xfc,0x9a,0x31}};
static EFI_GUID platform_guid = {0x524685a0,0x89a0,0x11e3,{0x9d,0x4d,0xbf,0xa9,0xf6,0xa4,0x03,0x08}};
extern void *memcpy(void *, const void *, size_t);
extern void *memset(void *, int, size_t);
static UINTN length(const char *s) { UINTN n=0; while (s[n]) n++; return n; }
static int equal(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; } return *a == *b;
}
static int prefix(const char *s, const char *p) {
    while (*p) if (*s++ != *p++) return 0;
    return 1;
}
static UINT32 word(const unsigned char *p) { UINT32 n; memcpy(&n,p,4); return n; }
static int terminated(const unsigned char *p, UINTN n) {
    for (UINTN i=0; i<n; i++) if (!p[i]) return 1;
    return 0;
}
static int dt_node(const unsigned char *p, UINTN size, UINTN *offset, unsigned depth,
                   unsigned *budget, UINTN *map) {
    UINTN off=*offset;
    if (depth>48 || !*budget || off>size || size-off<8) return 0;
    --*budget;
    UINT32 props=word(p+off), children=word(p+off+4);
    UINTN start=off;
    int named=0;
    off+=8;
    if (props>4096 || children>4096) return 0;
    for (UINT32 i=0;i<props;i++) {
        if (off>size || size-off<36 || !terminated(p+off,32)) return 0;
        UINTN n=word(p+off+32);
        if (n>size-off-36) return 0;
        UINTN padded=(n+3)&~(UINTN)3;
        if (padded>size-off-36) return 0;
        if (equal((const char *)p+off,"name")) {
            if (named++ || !n || !terminated(p+off+36,n)) return 0;
            if (equal((const char *)p+off+36,"memory-map")) {
                if (*map) return 0;
                *map=start+1;
            }
        }
        off+=36+padded;
    }
    if (!named) return 0;
    for (UINT32 i=0;i<children;i++)
        if (!dt_node(p,size,&off,depth+1,budget,map)) return 0;
    *offset=off;
    return 1;
}
int bridge_payload_dt_valid(const void *data, UINTN size) {
    UINTN off=0,map=0; unsigned budget=8192;
    return data && size && size<=BRIDGE_MAX_DEVICE_TREE_SIZE &&
        dt_node(data,size,&off,0,&budget,&map) && off==size && map;
}
static int hex8(const char *p, UINT32 *result) {
    if (length(p)!=8) return 0;
    UINT32 n=0;
    for (unsigned i=0;i<8;i++) {
        unsigned c=(unsigned char)p[i], v;
        if (c>='0' && c<='9') v=c-'0';
        else if (c>='a' && c<='f') v=c-'a'+10;
        else if (c>='A' && c<='F') v=c-'A'+10;
        else return 0;
        n=(n<<4)|v;
    }
    *result=n; return 1;
}
static void format_hex(char *out, UINT32 value) {
    for (unsigned i=0;i<8;i++) out[i]="0123456789abcdef"[(value>>(28-4*i))&15];
    out[8]=0;
}
static void discard_download(struct bridge_fastboot *s) {
    if (s->download) s->system->BootServices->FreePool(s->download);
    s->download=NULL; s->expected=s->received=0; s->receiving=0;
}
static EFI_STATUS reply(struct bridge_fastboot *s, const char *code, const char *message) {
    char buffer[65]; UINTN n=length(message);
    if (n>60) n=60;
    memcpy(buffer,code,4); memcpy(buffer+4,message,n);
    EFI_TPL old=s->system->BootServices->RaiseTPL(TPL_CALLBACK);
    EFI_STATUS status=s->transport->Send(n+4,buffer,&s->fatal_event);
    s->system->BootServices->RestoreTPL(old);
    if (EFI_ERROR(status)) s->failed=1;
    return status;
}
static int valid_payload(unsigned id, const void *data, UINTN size) {
    if (id==BRIDGE_KERNEL) { struct kernel_info info; return !macho_info(data,size,&info); }
    if (id==BRIDGE_DT) return bridge_payload_dt_valid(data,size);
    // A block-backed RAM disk must contain complete 16 KiB XNU pages.
    if (id==BRIDGE_RAMDISK) return size>=16384 && !(size&16383);
    // Trust cache format is validated by XNU; require its version/count header.
    return size>=8 && size<=16*1024*1024;
}
static EFI_STATUS prepare_inputs(struct bridge_fastboot *);
static void commit(struct bridge_fastboot *s, const char *argument) {
    static const char *names[]={"kernel","dt","ramdisk","trustcache"};
    unsigned id;
    for (id=0;id<BRIDGE_PAYLOAD_COUNT;id++) {
        UINTN n=length(names[id]);
        if (prefix(argument,names[id]) && argument[n]==' ') {
            UINT32 expected,actual;
            if (!hex8(argument+n+1,&expected) || !s->download || s->received!=s->expected) break;
            if (EFI_ERROR(s->system->BootServices->CalculateCrc32(s->download,s->received,&actual)) || actual!=expected) {
                reply(s,"FAIL","CRC32 mismatch"); return;
            }
            if (!valid_payload(id,s->download,s->received)) { reply(s,"FAIL","Invalid payload format"); return; }
            if (s->owned[id]) s->system->BootServices->FreePool(s->owned[id]);
            s->owned[id]=s->download;
            s->inputs[id]=(struct bridge_payload){s->download,s->received};
            s->download=NULL; s->expected=s->received=0;
            reply(s,"OKAY",""); return;
        }
    }
    reply(s,"FAIL","Use: oem bridge load kernel|dt|ramdisk|trustcache CRC32");
}
static void command(struct bridge_fastboot *s, const char *cmd) {
    if (prefix(cmd,"getvar:")) {
        char value[61]={0}; EFI_STATUS status=EFI_SUCCESS;
        const char *name=cmd+7;
        if (equal(name,"bridge-service")) memcpy(value,"1",2);
        else if (equal(name,"bridge-state")) memcpy(value,"ready",6);
        else if (equal(name,"max-download-size")) memcpy(value,"0x04000000",11);
        else if (equal(name,"version")) memcpy(value,"0.4",4);
        else if (prefix(name,"bridge-crc32:")) {
            static const char *names[]={"kernel","dt","ramdisk","trustcache"};
            status=EFI_NOT_FOUND;
            for (unsigned i=0;i<BRIDGE_PAYLOAD_COUNT;i++) if (equal(name+13,names[i]) && s->inputs[i].size) {
                UINT32 crc;
                status=s->system->BootServices->CalculateCrc32((void *)s->inputs[i].data,s->inputs[i].size,&crc);
                if (!EFI_ERROR(status)) format_hex(value,crc);
                break;
            }
        } else status=s->platform->GetVar((CHAR8 *)name,value);
        reply(s,EFI_ERROR(status)?"FAIL":"OKAY",EFI_ERROR(status)?"Variable unavailable":value);
    } else if (prefix(cmd,"download:")) {
        UINT32 size;
        if (!hex8(cmd+9,&size) || !size || size>MAX_DOWNLOAD) { reply(s,"FAIL","Invalid download size"); return; }
        UINTN retained=0;
        for (unsigned i=0;i<BRIDGE_PAYLOAD_COUNT;i++) if (s->owned[i]) retained+=s->inputs[i].size;
        if (retained+size>MAX_RETAINED) { reply(s,"FAIL","Payload memory limit; reset unused payloads"); return; }
        discard_download(s);
        EFI_STATUS status=s->system->BootServices->AllocatePool(EfiLoaderData,size,&s->download);
        if (EFI_ERROR(status)) { s->download=NULL; reply(s,"FAIL","Out of memory"); return; }
        s->expected=size; s->receiving=1;
        reply(s,"DATA",cmd+9);
    } else if (prefix(cmd,"oem bridge load ")) commit(s,cmd+16);
    else if (equal(cmd,"oem crc32")) {
        UINT32 crc; char value[9];
        if (!s->download || s->received!=s->expected ||
            EFI_ERROR(s->system->BootServices->CalculateCrc32(s->download,s->received,&crc))) reply(s,"FAIL","No complete download");
        else { format_hex(value,crc); reply(s,"INFO",value); reply(s,"OKAY",""); }
    } else if (equal(cmd,"oem bridge reset")) {
        for (unsigned i=0;i<BRIDGE_PAYLOAD_COUNT;i++) {
            if (s->owned[i]) s->system->BootServices->FreePool(s->owned[i]);
            s->owned[i]=NULL; s->inputs[i]=s->defaults[i];
        }
        discard_download(s); reply(s,"OKAY","");
    } else if (equal(cmd,"continue") || equal(cmd,"oem bridge boot")) {
        if (EFI_ERROR(prepare_inputs(s))) reply(s,"FAIL","Boot inputs are invalid or exceed memory");
        else if (!EFI_ERROR(reply(s,"OKAY",""))) s->boot_requested=1;
    } else if (equal(cmd,"flash:boot")) {
        if (!s->download || s->received!=s->expected) reply(s,"FAIL","No complete download");
        else {
            EFI_STATUS status=s->platform->FlashPartition("boot",s->received,s->download);
            reply(s,EFI_ERROR(status)?"FAIL":"OKAY",EFI_ERROR(status)?"Boot write/readback failed":"");
            discard_download(s);
        }
    } else if (equal(cmd,"reboot") || equal(cmd,"reboot-bootloader")) {
        if (!EFI_ERROR(reply(s,"OKAY",""))) {
            bridge_fastboot_suspend(s);
            s->system->RuntimeServices->ResetSystem(EfiResetCold,EFI_SUCCESS,0,NULL);
        }
    } else reply(s,"FAIL","Unsupported bridge service command");
}
static VOID EFIAPI received(EFI_EVENT event, VOID *context) { (void)event; (void)context; }
static VOID EFIAPI fatal(EFI_EVENT event, VOID *context) {
    (void)event; ((struct bridge_fastboot *)context)->failed=1;
}
static VOID EFIAPI timeout(EFI_EVENT event, VOID *context) {
    (void)event;
    struct bridge_fastboot *s=context;
    if (s->receiving && s->idle_seconds<30) s->idle_seconds++;
}
EFI_STATUS bridge_fastboot_resume(struct bridge_fastboot *s) {
    if (s->running) return EFI_SUCCESS;
    EFI_BOOT_SERVICES *bs=s->system->BootServices;
    EFI_STATUS status=s->platform->Init();
    if (EFI_ERROR(status)) return status;
    s->initialized=1; s->failed=0;
    status=bs->CreateEvent(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,received,s,&s->receive_event);
    if (!EFI_ERROR(status)) status=bs->CreateEvent(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,fatal,s,&s->fatal_event);
    if (!EFI_ERROR(status)) status=bs->CreateEvent(EVT_TIMER|EVT_NOTIFY_SIGNAL,TPL_CALLBACK,timeout,s,&s->timeout_event);
    if (!EFI_ERROR(status)) status=bs->SetTimer(s->timeout_event,TimerPeriodic,10000000);
    if (!EFI_ERROR(status)) status=s->transport->Start(s->receive_event);
    if (EFI_ERROR(status)) { bridge_fastboot_suspend(s); return status; }
    s->running=1;
    return EFI_SUCCESS;
}
EFI_STATUS bridge_fastboot_start(struct bridge_fastboot *s, EFI_SYSTEM_TABLE *system,
                                 const struct bridge_payload defaults[BRIDGE_PAYLOAD_COUNT]) {
    memset(s,0,sizeof(*s)); s->system=system;
    memcpy(s->defaults,defaults,sizeof(s->defaults)); memcpy(s->inputs,defaults,sizeof(s->inputs));
    EFI_STATUS status=system->BootServices->LocateProtocol(&transport_guid,NULL,(void **)&s->transport);
    if (!EFI_ERROR(status)) status=system->BootServices->LocateProtocol(&platform_guid,NULL,(void **)&s->platform);
    return EFI_ERROR(status)?status:bridge_fastboot_resume(s);
}
EFI_STATUS bridge_fastboot_suspend(struct bridge_fastboot *s) {
    EFI_BOOT_SERVICES *bs=s->system->BootServices;
    EFI_STATUS status=EFI_SUCCESS;
    // Stop may also clean up a partially failed Start implementation.
    if (s->initialized) status=s->transport->Stop();
    if (EFI_ERROR(status)) return status;
    s->running=0;
    if (s->receive_event) bs->CloseEvent(s->receive_event);
    if (s->fatal_event) bs->CloseEvent(s->fatal_event);
    if (s->timeout_event) bs->CloseEvent(s->timeout_event);
    s->receive_event=s->fatal_event=s->timeout_event=NULL;
    if (s->initialized) s->platform->UnInit();
    s->initialized=0;
    discard_download(s);
    return status;
}
void bridge_fastboot_destroy(struct bridge_fastboot *s) {
    bridge_fastboot_suspend(s);
    for (unsigned i=0;i<BRIDGE_PAYLOAD_COUNT;i++) if (s->owned[i]) {
        s->system->BootServices->FreePool(s->owned[i]); s->owned[i]=NULL;
    }
}
int bridge_fastboot_busy(const struct bridge_fastboot *s) { return s->receiving; }
void bridge_fastboot_poll(struct bridge_fastboot *s) {
    if (!s->running) return;
    if (s->failed || (s->receiving && s->idle_seconds>=30)) {
        bridge_fastboot_suspend(s); bridge_fastboot_resume(s); return;
    }
    // Bound work per application-level dispatch so foreground input is serviced.
    for (unsigned i=0;i<4 && !s->boot_requested;i++) {
        UINTN size=0; void *packet=NULL;
        EFI_TPL old=s->system->BootServices->RaiseTPL(TPL_CALLBACK);
        EFI_STATUS status=s->transport->Receive(&size,&packet);
        s->system->BootServices->RestoreTPL(old);
        if (status==EFI_NOT_READY) break;
        if (EFI_ERROR(status)) { s->failed=1; break; }
        s->hold=1;
        s->idle_seconds=0;
        if (s->receiving) {
            if (!size || size>s->expected-s->received) { reply(s,"FAIL","Download overflow"); s->failed=1; }
            else {
                memcpy((unsigned char *)s->download+s->received,packet,size); s->received+=size;
                if (s->received==s->expected) { s->receiving=0; reply(s,"OKAY",""); }
            }
        } else {
            char cmd[65]; int valid=size>0 && size<=64;
            if (valid) {
                memcpy(cmd,packet,size); cmd[size]=0;
                for (UINTN j=0;j<size;j++) if ((unsigned char)cmd[j]<32 || (unsigned char)cmd[j]>126) valid=0;
            }
            if (valid) command(s,cmd); else reply(s,"FAIL","Invalid command");
        }
        if (packet) s->system->BootServices->FreePool(packet);
        if (s->failed || !s->running) break;
    }
}
static EFI_STATUS prepare_inputs(struct bridge_fastboot *s) {
    if (s->receiving) return EFI_NOT_READY;
    const struct bridge_payload *dt=&s->inputs[BRIDGE_DT];
    struct kernel_info info;
    if (macho_info(s->inputs[BRIDGE_KERNEL].data,s->inputs[BRIDGE_KERNEL].size,&info) ||
        !bridge_payload_dt_valid(dt->data,dt->size)) return EFI_LOAD_ERROR;
    UINT64 needed=info.end-info.virt;
    needed=((needed+0x3fff)&~0x3fffULL)+dt->size+52;
    needed=((needed+0x3fff)&~0x3fffULL)+s->inputs[BRIDGE_TRUSTCACHE].size;
    needed=((needed+0x3fff)&~0x3fffULL)+sizeof(struct boot_args);
    needed=((needed+0x3fff)&~0x3fffULL)+0x100000;
    needed=((needed+0x3fff)&~0x3fffULL)+s->reserved_bytes;
    needed=((needed+0x3fff)&~0x3fffULL)+s->inputs[BRIDGE_RAMDISK].size;
    if (needed>BRIDGE_ARENA_SIZE) return EFI_OUT_OF_RESOURCES;
    UINT32 range_bytes=0;
    void *ramdisk_range=dt_property((void *)dt->data,"memory-map","RAMDisk",&range_bytes);
    if (ramdisk_range && !s->inputs[BRIDGE_RAMDISK].size) return EFI_LOAD_ERROR;
    if (s->inputs[BRIDGE_RAMDISK].size) {
        UINT32 bytes;
        void *range=dt_property((void *)dt->data,"memory-map","RAMDisk",&bytes);
        if (range && bytes!=16) return EFI_LOAD_ERROR;
        if (!range) {
            if (dt->size>BRIDGE_MAX_DEVICE_TREE_SIZE-52) return EFI_BAD_BUFFER_SIZE;
            UINTN off=0,map=0; unsigned budget=8192;
            if (!dt_node(dt->data,dt->size,&off,0,&budget,&map)) return EFI_LOAD_ERROR;
            void *copy=NULL;
            EFI_STATUS status=s->system->BootServices->AllocatePool(EfiLoaderData,dt->size+52,&copy);
            if (EFI_ERROR(status)) return status;
            UINTN insert=map-1+8;
            memcpy(copy,dt->data,insert);
            memset((unsigned char *)copy+insert,0,52);
            memcpy((unsigned char *)copy+insert,"RAMDisk",8);
            UINT32 n=16, props=word((const unsigned char *)dt->data+map-1)+1;
            memcpy((unsigned char *)copy+insert+32,&n,4);
            memcpy((unsigned char *)copy+map-1,&props,4);
            memcpy((unsigned char *)copy+insert+52,(const unsigned char *)dt->data+insert,dt->size-insert);
            if (s->owned[BRIDGE_DT]) s->system->BootServices->FreePool(s->owned[BRIDGE_DT]);
            s->owned[BRIDGE_DT]=copy;
            s->inputs[BRIDGE_DT]=(struct bridge_payload){copy,dt->size+52};
        }
    }
    return EFI_SUCCESS;
}
EFI_STATUS bridge_fastboot_prepare_boot(struct bridge_fastboot *s) {
    EFI_STATUS status=prepare_inputs(s);
    return EFI_ERROR(status)?status:bridge_fastboot_suspend(s);
}
