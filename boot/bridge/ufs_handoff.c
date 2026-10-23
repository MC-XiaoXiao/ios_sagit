// SPDX-License-Identifier: BSD-2-Clause
// MSM8998 warm UFS handoff. No EFI storage callback is used after ExitBootServices.
#include <Uefi.h>
#include <Protocol/BlockIo.h>
#include "bridge.h"
extern void *memcpy(void *, const void *, size_t);
extern void *memset(void *, int, size_t);
struct clock_protocol;
typedef struct clock_protocol Clock;
struct clock_protocol {
    UINT64 version;
    EFI_STATUS (EFIAPI *get_id)(Clock *, const CHAR8 *, UINTN *);
    EFI_STATUS (EFIAPI *enable)(Clock *, UINTN);
    void *disable, *is_enabled;
    EFI_STATUS (EFIAPI *is_on)(Clock *, UINTN, BOOLEAN *);
    void *set_frequency;
    EFI_STATUS (EFIAPI *get_frequency)(Clock *, UINTN, UINT32 *);
};
struct transfer {
    uint32_t header, reserved, ocs, reserved2;
    uint64_t command;
    uint16_t response_length, response_offset, prd_length, prd_offset;
};
_Static_assert(sizeof(struct transfer)==32,"UFSHCI transfer ABI");
static unsigned char *area;
static unsigned lun;
static uint64_t apfs_lba;
static void *tree;
static uint32_t gear[6];
static const uint32_t gates[]={0x75008,0x7500c,0x75010,0x75014,0x76008,0x76040,0x82028,0x88004};
static uint32_t rd(uint64_t p) {
    uint32_t v=*(volatile uint32_t *)(uintptr_t)p;
    __asm__ volatile("dmb oshld" ::: "memory");return v;
}
static void wr(uint64_t p,uint32_t v) {
    __asm__ volatile("dmb oshst" ::: "memory");
    *(volatile uint32_t *)(uintptr_t)p=v;
    __asm__ volatile("dsb sy" ::: "memory");
}
static void delay(unsigned us) {
    uint64_t start,now,hz;
    __asm__ volatile("mrs %0,cntpct_el0":"=r"(start));
    __asm__ volatile("mrs %0,cntfrq_el0":"=r"(hz));
    do { __asm__ volatile("mrs %0,cntpct_el0":"=r"(now)); } while(now-start < (hz*us+999999)/1000000);
}
static int wait_reg(unsigned off,uint32_t mask,uint32_t expected,unsigned us) {
    for(unsigned n=0;n<=us/10;n++) { if((rd(0x1da4000+off)&mask)==expected)return 0;delay(10); }return -1;
}
static int uic(unsigned op,uint32_t a,uint32_t value,uint32_t *out) {
    if(wait_reg(0x30,8,8,500000))return -1;
    delay(1000);wr(0x1da4020,0x400);wr(0x1da4094,a);wr(0x1da4098,0);wr(0x1da409c,value);wr(0x1da4090,op);
    if(wait_reg(0x20,0x400,0x400,500000))return -1;
    unsigned result=rd(0x1da4098)&255;
    if(out)*out=rd(0x1da409c);
    wr(0x1da4020,0x400);return result?-1:0;
}
static void cache(void *data,size_t size,int invalidate) {
    uint64_t ctr;__asm__ volatile("mrs %0,ctr_el0":"=r"(ctr));
    uint64_t line=4ULL<<((ctr>>16)&15),end=(uintptr_t)data+size;
    for(uint64_t p=(uintptr_t)data&~(line-1);p<end;p+=line) {
        if(invalidate)__asm__ volatile("dc civac,%0"::"r"(p):"memory");
        else __asm__ volatile("dc cvac,%0"::"r"(p):"memory");
    }
    __asm__ volatile("dsb sy":::"memory");
}
static int put32(const char *property,uint32_t value) {
    uint32_t len;unsigned char *p=dt_property(tree,"storage@1da4000",property,&len);
    if(!p||len!=4)return -1;
    for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(value>>(i*8));return 0;
}
static void be32(unsigned char *p,uint32_t v) {for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(v>>(24-8*i));}
static int resources(void) {
    // Retain the already-trained firmware link and its rails/NoC resources.
    // No regulator, RPM, interconnect or clock provider is allowed to power it off.
    if(rd(0x17501c)!=0x105 || rd(0x17602c)!=0x107 || !(rd(0x175004)&0x80000000) || rd(0x175000)&1)return -1;
    for(unsigned i=0;i<sizeof(gates)/sizeof(gates[0]);i++)if(!(rd(0x100000+gates[i])&1))return -1;
    if(!(rd(0x1da40dc)&(1u<<26)) || !(rd(0x1da7d60)&1))return -1;
    return 0;
}
int bridge_ufs_prepare(EFI_BOOT_SERVICES *bs,void *apple_tree) {
    tree=apple_tree;
    uint32_t len;void *v=dt_property(tree,"storage@1da4000","qcom,ufs-handoff-version",&len);
    if(!v || len!=4)return -1;
    if(rd(0x1da40e4)!=0x30010000 || resources())return -2;
    EFI_GUID cg={0x241afae6,0x885f,0x4f6c,{0xa7,0xea,0xc2,0x8e,0xab,0x79,0xc3,0xe5}};
    Clock *clock=0;
    if(EFI_ERROR(bs->LocateProtocol(&cg,0,(void **)&clock)) || !clock || clock->version<0x10005)return -3;
    const char *names[]={"gcc_ufs_axi_clk","gcc_aggre1_ufs_axi_clk","gcc_ufs_ahb_clk","gcc_ufs_unipro_core_clk",
        "gcc_ufs_phy_aux_clk","gcc_ufs_clkref_clk","gcc_ufs_tx_symbol_0_clk","gcc_ufs_rx_symbol_0_clk"};
    for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++) {
        UINTN id;UINT32 hz=0;BOOLEAN on=0;
        if(EFI_ERROR(clock->get_id(clock,(const CHAR8 *)names[i],&id)) || EFI_ERROR(clock->enable(clock,id)) ||
           EFI_ERROR(clock->is_on(clock,id,&on)) || !on)return -4;
        if(i==0 || i==3) {
            if(EFI_ERROR(clock->get_frequency(clock,id,&hz)) || hz!=(i?150000000u:200000000u))return -5;
        }
    }
    EFI_PHYSICAL_ADDRESS memory=0;
    if(EFI_ERROR(bs->AllocatePages(AllocateAnyPages,EfiLoaderData,8,&memory)))return -6;
    area=(void *)(uintptr_t)memory;memset(area,0,32768);
    EFI_GUID bg=EFI_BLOCK_IO_PROTOCOL_GUID;EFI_HANDLE *handles=0;UINTN count=0;
    if(EFI_ERROR(bs->LocateHandleBuffer(ByProtocol,&bg,0,&count,&handles)))return -7;
    int found=0;
    for(UINTN i=0;i<count;i++) {
        EFI_BLOCK_IO_PROTOCOL *b=0;
        if(EFI_ERROR(bs->HandleProtocol(handles[i],&bg,(void **)&b)) || !b || !b->Media || b->Media->LogicalPartition)continue;
        if(b->Media->BlockSize!=4096 || b->Media->LastBlock!=13895679)continue;
        if(EFI_ERROR(b->FlushBlocks(b)) || EFI_ERROR(b->ReadBlocks(b,b->Media->MediaId,1,4096,area+16384)))continue;
        // Exact GPT disk identity, not merely a coincidentally matching capacity.
        const unsigned char id[]={0x32,0x1b,0x10,0x98,0xe2,0xbb,0xf2,0x4b,0xa0,0x6e,0x2b,0xb3,0x3d,0,0x0c,0x20};
        int equal=1;for(unsigned j=0;j<16;j++)if(area[16384+56+j]!=id[j])equal=0;
        if(!equal)continue;
        uint64_t table_lba;memcpy(&table_lba,area+16384+72,8);
        if(table_lba!=2 || EFI_ERROR(b->ReadBlocks(b,b->Media->MediaId,table_lba,4096,area+20480)))continue;
        const unsigned char partition_id[]={0x4d,0xf0,0xa2,0x1a,0x40,0xe4,0x33,0x46,0xb4,0x55,0xce,0xb4,0x54,0xf3,0x43,0x1e};
        for(unsigned entry=0;entry<32;entry++) {
            unsigned char *p=area+20480+entry*128;
            equal=1;for(unsigned j=0;j<16;j++)if(p[16+j]!=partition_id[j])equal=0;
            if(equal) {memcpy(&apfs_lba,p+32,8);break;}
        }
        if(!apfs_lba)continue;
        // UFSDxe leaves its completed UTRD in memory. Recover the actual LU number.
        uint64_t address=rd(0x1da4050)|((uint64_t)rd(0x1da4054)<<32);
        if(address<0x80000000 || address>0x1ffffffffULL)continue;
        struct transfer *t=(void *)(uintptr_t)address;cache(t,1024,1);
        for(unsigned slot=0;slot<32;slot++) {
            uint64_t cmd=t[slot].command;
            if(cmd<0x80000000 || cmd>0x1ffffffffULL)continue;
            unsigned char *p=(void *)(uintptr_t)cmd;cache(p,512,1);
            if(p[0]!=1 || p[2]>7 || (p[16]!=0x28 && p[16]!=0x88))continue;
            lun=p[2];found=1;break;
        }
        break;
    }
    bs->FreePool(handles);
    if(!found)return -8;
    static const uint16_t attrs[]={0x1583,0x1568,0x1580,0x1560,0x156a,0x1571};
    for(unsigned i=0;i<6;i++)if(uic(1,(uint32_t)attrs[i]<<16,0,&gear[i]))return -9;
    if(!gear[0] || gear[0]>3 || !gear[1] || gear[1]>3 || gear[2]!=1 || gear[3]!=1 || gear[4]!=2)return -10;
    if(gear[5]!=0x11 && gear[5]!=0x44)return -10;
    bridge_puts("[UFS handoff] prepared warm link, HCI=200MHz UniPro=150MHz LU=");bridge_hex(lun);
    bridge_puts(" RX gear=");bridge_hex(gear[0]);bridge_puts(" TX gear=");bridge_hex(gear[1]);bridge_puts(" power mode=");bridge_hex(gear[5]);bridge_puts("\n");
    return 0;
}
static int physical_read_once(uint64_t lba) {
    struct transfer *t=(void *)area;unsigned char *cmd=area+4096,*data=area+12288;
    memset(area,0,1024);memset(cmd,0,8192);memset(data,0,4096);
    t->header=(1u<<28)|(1u<<24)|(2u<<25);t->ocs=15;t->command=(uintptr_t)cmd;
    t->response_length=128;t->response_offset=128;t->prd_length=1;t->prd_offset=256;
    if(lba>0xffffffffULL)return -4;
    cmd[0]=1;cmd[1]=0x40;cmd[2]=(unsigned char)lun;be32(cmd+12,4096);cmd[16]=0x28;
    be32(cmd+18,(uint32_t)lba);cmd[24]=1;
    uint64_t p=(uintptr_t)data;memcpy(cmd+1024,&p,8);uint32_t bytes=4095;memcpy(cmd+1036,&bytes,4);
    cache(area,16384,1);wr(0x1da4020,~0u);wr(0x1da4058,1);
    if(wait_reg(0x58,1,0,1000000))return -1;
    cache(area,16384,1);
    unsigned sense_format=cmd[546]&0x7f;
    unsigned sense_key=(sense_format==0x70 || sense_format==0x71)?cmd[548]&15:cmd[547]&15;
    unsigned sense_asc=(sense_format==0x70 || sense_format==0x71)?cmd[558]:cmd[548];
    if(!t->ocs && cmd[512]==0x21 && cmd[519]==2 && sense_key==6 && sense_format>=0x70 && sense_format<=0x73) {
        bridge_puts("[UFS handoff] acknowledged reset unit attention ASC=");bridge_hex(sense_asc);bridge_puts("\n");return 1;
    }
    if(t->ocs || cmd[512]!=0x21 || cmd[514]!=(unsigned char)lun || cmd[518] || cmd[519]) {
        bridge_puts("[UFS handoff] read response OCS=");bridge_hex(t->ocs);bridge_puts(" UPIU=");bridge_hex(cmd[512]);bridge_puts(" status=");bridge_hex(cmd[519]);bridge_puts(" sense format/key/ASC=");bridge_hex(sense_format);bridge_hex(sense_key);bridge_hex(sense_asc);bridge_puts("\n");return -2;
    }
    return 0;
}
static int physical_read(uint64_t lba) {
    // Unit attention after HCE reset is expected. Retry only a completed READ,
    // never a transport failure or any write command.
    for(unsigned retry=0;retry<3;retry++) {int result=physical_read_once(lba);if(result!=1)return result;}
    return -3;
}
static int control(unsigned transaction,unsigned query_opcode,uint32_t *value) {
    struct transfer *t=(void *)area;unsigned char *cmd=area+4096;
    memset(area,0,1024);memset(cmd,0,8192);
    t->header=(1u<<28)|(1u<<24);t->ocs=15;t->command=(uintptr_t)cmd;
    t->response_length=128;t->response_offset=128;
    cmd[0]=(unsigned char)transaction;
    if(transaction==0x16) {cmd[5]=query_opcode==6?0x81:1;cmd[12]=(unsigned char)query_opcode;cmd[13]=1;}
    cache(area,12288,1);wr(0x1da4020,~0u);wr(0x1da4058,1);
    if(wait_reg(0x58,1,0,1000000))return -1;
    cache(area,12288,1);
    if(t->ocs || cmd[512]!=(transaction?0x36:0x20) || cmd[518] || cmd[519])return -2;
    if(value)*value=((uint32_t)cmd[532]<<24)|((uint32_t)cmd[533]<<16)|((uint32_t)cmd[534]<<8)|cmd[535];
    return 0;
}
int bridge_ufs_takeover(void) {
    // All firmware exit callbacks have completed. Mask interrupts and fence DMA
    // before replacing both descriptor lists; never reuse firmware DMA memory.
    if(!area || resources())return -1;
    uint32_t ctlr=rd(0x17a00000);
    bridge_puts("[UFS handoff] GICD_CTLR before=");bridge_hex(ctlr);bridge_puts("\n");
    // Enable the NS bank's affinity-routed Group1A forwarding. EnableGroup1's
    // other alias can be RAZ/WI, so require only the architected 0x12 interface.
    // Change ARE_NS with NS forwarding off; preserve DS and secure ownership.
    if((ctlr&0x12)!=0x12) {
        wr(0x17a00000,ctlr&~3u);
        for(unsigned i=0;i<10000 && (rd(0x17a00000)&0x80000000);i++)delay(10);
        if(rd(0x17a00000)&0x80000000)return -27;
        wr(0x17a00000,(ctlr&~3u)|0x10u);
        for(unsigned i=0;i<10000 && (rd(0x17a00000)&0x80000000);i++)delay(10);
        if(rd(0x17a00000)&0x80000000)return -28;
        wr(0x17a00000,ctlr|0x12u);
    }
    ctlr=rd(0x17a00000);
    bridge_puts("[UFS handoff] GICD_CTLR after=");bridge_hex(ctlr);bridge_puts("\n");
    if(put32("qcom,ufs-polled-completion",(ctlr&0x12)==0x12?0:1))return -26;
    wr(0x1da4024,0);wr(0x1da4060,0);wr(0x1da4080,0);wr(0x1da4034,0);
    if(wait_reg(0x34,1,0,100000))return -2;
    wr(0x1da4034,1);
    if(wait_reg(0x34,1,1,100000) || wait_reg(0x30,8,8,100000))return -3;
    if(!(rd(0x1da4030)&1) && uic(0x16,0,0,0))return -4;
    uint64_t pa=(uintptr_t)area;
    wr(0x1da4050,(uint32_t)pa);wr(0x1da4054,(uint32_t)(pa>>32));
    wr(0x1da4070,(uint32_t)(pa+2048));wr(0x1da4074,(uint32_t)((pa+2048)>>32));
    if(wait_reg(0x30,0xe,0xe,100000))return -5;
    wr(0x1da4060,1);
    if(control(0,0,0) || control(0x16,6,0))return -6;
    uint32_t initializing=1;
    for(unsigned i=0;i<100 && initializing;i++) {
        if(control(0x16,5,&initializing))return -7;
        if(initializing)delay(10000);
    }
    if(initializing)return -8;
    int result=physical_read(1);if(result)return -10+result;
    for(unsigned j=0;j<4096;j++)if(area[12288+j]!=area[16384+j])return -13;
    result=physical_read(apfs_lba);if(result)return -20+result;
    if(rd((uintptr_t)area+12288+32)!=0x4253584e)return -23;
    // XNU gets a stopped controller; it will allocate its own DMA lists.
    wr(0x1da4060,0);wr(0x1da4034,0);if(wait_reg(0x34,1,0,100000))return -24;
    if(put32("qcom,ufs-hs-gear",gear[0]<gear[1]?gear[0]:gear[1]) ||
       put32("qcom,ufs-hs-power-mode",gear[5]) ||
       put32("qcom,ufs-handoff-version",2) || put32("qcom,ufs-handoff-flags",31))return -25;
    bridge_puts("[UFS handoff] post-EFI physical DMA GPT + APFS NXSB verified; firmware ownership released\n");
    return 0;
}
