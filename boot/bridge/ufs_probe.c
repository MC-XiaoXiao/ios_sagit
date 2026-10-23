// SPDX-License-Identifier: BSD-2-Clause
// Read-only firmware diagnostics before attempting UFS ownership transfer.
#include <Uefi.h>
#include <Protocol/BlockIo.h>
#include "bridge.h"
typedef struct clock_protocol Clock;
struct clock_protocol {
    UINT64 version;
    EFI_STATUS (EFIAPI *get_id)(Clock *, const CHAR8 *, UINTN *);
    void *enable, *disable, *is_enabled;
    EFI_STATUS (EFIAPI *is_on)(Clock *, UINTN, BOOLEAN *);
    void *set_frequency;
    EFI_STATUS (EFIAPI *get_frequency)(Clock *, UINTN, UINT32 *);
};
static uint32_t reg(uint64_t p) {
    uint32_t v = *(volatile uint32_t *)(uintptr_t)p;
    __asm__ volatile("dmb oshld" ::: "memory"); return v;
}
static void value(const char *name, uint64_t v) {
    bridge_puts(name); bridge_hex(v); bridge_puts("\n");
}
EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    (void)image;
    if (platform_console_init((void *)dt_blob)) return EFI_UNSUPPORTED;
    bridge_puts("\n[UFS probe] read-only firmware state\n");
    EFI_GUID guid = {0x241afae6,0x885f,0x4f6c,{0xa7,0xea,0xc2,0x8e,0xab,0x79,0xc3,0xe5}};
    Clock *c = 0;
    EFI_BOOT_SERVICES *bs = st->BootServices;
    EFI_STATUS status = bs->LocateProtocol(&guid,0,(void **)&c);
    value("Clock protocol status=",status);
    if (!EFI_ERROR(status) && c) {
        value("Clock protocol version=",c->version);
        const char *names[] = {"gcc_ufs_axi_clk","gcc_aggre1_ufs_axi_clk","gcc_ufs_ahb_clk",
            "gcc_ufs_unipro_core_clk","gcc_ufs_phy_aux_clk","gcc_ufs_clkref_clk",
            "gcc_ufs_tx_symbol_0_clk","gcc_ufs_rx_symbol_0_clk","gcc_ufs_rx_symbol_1_clk",
            "gcc_aggre1_noc_clk","gcc_bimc_clk"};
        for (unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++) {
            UINTN id=0; UINT32 hz=0; BOOLEAN on=0;
            bridge_puts(names[i]);
            status=c->get_id(c,(const CHAR8 *)names[i],&id);
            bridge_puts(" id-status=");bridge_hex(status);
            if (!EFI_ERROR(status)) {
                bridge_puts(" id=");bridge_hex(id);
                status=c->get_frequency(c,id,&hz);bridge_puts(" freq-status=");bridge_hex(status);
                bridge_puts(" Hz=");bridge_hex(hz);
                status=c->is_on(c,id,&on);bridge_puts(" on-status=");bridge_hex(status);
                bridge_puts(" on=");bridge_hex(on);
            }
            bridge_puts("\n");
        }
    }
    const uint32_t offsets[]={0,8,0x18,0x20,0x24,0x30,0x34,0x50,0x54,0x58,0x60,0x70,0x74,0x78,0x80,0xc0,0xdc,0xe4};
    for (unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++) {
        bridge_puts("UFS ");bridge_hex(offsets[i]);bridge_puts("=");bridge_hex(reg(0x1da4000+offsets[i]));bridge_puts("\n");
    }
    const uint32_t gates[]={0x75000,0x75004,0x75008,0x7500c,0x75010,0x75014,0x75018,0x7501c,
        0x76008,0x7600c,0x76028,0x7602c,0x76040,0x7605c,0x82028,0x88004};
    for (unsigned i=0;i<sizeof(gates)/sizeof(gates[0]);i++) {
        bridge_puts("GCC ");bridge_hex(gates[i]);bridge_puts("=");bridge_hex(reg(0x100000+gates[i]));bridge_puts("\n");
    }
    EFI_GUID block_guid=EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_HANDLE *handles=0;UINTN count=0;
    status=bs->LocateHandleBuffer(ByProtocol,&block_guid,0,&count,&handles);
    if (!EFI_ERROR(status)) for (UINTN i=0;i<count;i++) {
        EFI_BLOCK_IO_PROTOCOL *b=0;
        if (EFI_ERROR(bs->HandleProtocol(handles[i],&block_guid,(void **)&b)) || !b || !b->Media || b->Media->LogicalPartition) continue;
        value("Block device last LBA=",b->Media->LastBlock);
        value("Block device block size=",b->Media->BlockSize);
        if (b->Media->BlockSize!=4096 || b->Media->LastBlock!=13895679) continue;
        EFI_PHYSICAL_ADDRESS buffer=0;
        if (EFI_ERROR(bs->AllocatePages(AllocateAnyPages,EfiLoaderData,1,&buffer))) continue;
        status=b->ReadBlocks(b,b->Media->MediaId,1,4096,(void *)(uintptr_t)buffer);
        value("GPT read status=",status);value("GPT read physical buffer=",buffer);
        if (!EFI_ERROR(status)) {
            const unsigned char *p=(void *)(uintptr_t)buffer;
            value("GPT signature=",*(const uint64_t *)p);
        }
        bs->FreePages(buffer,1);
    }
    if (handles) bs->FreePool(handles);
    bridge_puts("[UFS probe] done; returning to firmware\n");
    return EFI_SUCCESS;
}
