// SPDX-License-Identifier: BSD-2-Clause
#include <Uefi.h>
#include "bridge.h"

static uint32_t rd(uint64_t address) {
    uint32_t value = *(volatile uint32_t *)(uintptr_t)address;
    __asm__ volatile("dmb oshld" ::: "memory");
    return value;
}
static void show(const char *label, uint64_t address) {
    bridge_puts(label); bridge_hex(rd(address));
}
void bridge_display_probe(void) {
    const uint64_t mdp = 0x0c901000;
    bridge_puts("[Display probe] MDSS");
    show(" version=", 0x0c900000);
    show(" top=", 0x0c901004);
    show(" irq-en=", mdp + 0x10);
    show(" irq-status=", mdp + 0x14);
    bridge_puts("\n");
    const unsigned pipes[] = {0x4000, 0x6000, 0x8000, 0xa000, 0x24000, 0x26000};
    for (unsigned i = 0; i < sizeof(pipes)/sizeof(pipes[0]); ++i) {
        uint64_t b = mdp + pipes[i];
        bridge_puts("[Display probe] SSPP="); bridge_hex(pipes[i]);
        show(" size=",b); show(" out=",b+0xc); show(" xy=",b+8);
        show(" dst=",b+0x10); show(" addr=",b+0x14); show(" stride=",b+0x24);
        show(" format=",b+0x30); show(" unpack=",b+0x34); show(" op=",b+0x38);
        bridge_puts("\n");
    }
    for (unsigned i = 0; i < 5; ++i) {
        uint64_t b = mdp + 0x1000 + i * 0x200;
        bridge_puts("[Display probe] CTL="); bridge_hex(i);
        show(" lm0=",b); show(" op=",b+0x14); show(" flush=",b+0x18);
        bridge_puts("\n");
    }
    for (unsigned i = 0; i < 4; ++i) {
        uint64_t b = mdp + 0x6a000 + i * 0x800;
        bridge_puts("[Display probe] INTF="); bridge_hex(i);
        show(" enabled=",b); show(" hsync=",b+8); show(" vperiod=",b+0xc);
        show(" frame=",b+0xa8); show(" line=",b+0xac);
        bridge_puts("\n");
    }
    for (unsigned i = 0; i < 2; ++i) {
        uint64_t b = 0x0c994000 + i * 0x2000;
        bridge_puts("[Display probe] DSI="); bridge_hex(i);
        show(" version=",b); show(" ctrl=",b+4); show(" status=",b+8);
        show(" clock=",b+0x11c); show(" legacy-version=",b+0x1f0);
        show(" shifted-version=",b+0x1f4); bridge_puts("\n");
        bridge_puts("[Display DSI transport] index=");bridge_hex(i);
        // 6G control registers start four bytes after the hardware-version word.
        show(" cmd-format=",b+0x40);show(" cmd-insert=",b+0x44);
        show(" stream-ctrl=",b+0x58);show(" stream-total=",b+0x5c);
        show(" trigger=",b+0x84);show(" lane-status=",b+0xa8);show(" lane-control=",b+0xac);
        bridge_puts(" phase=UEFI reads-only=1\n");

    }
    for (unsigned i = 0; i < 4; ++i) {
        uint64_t b = mdp + 0x70000 + i * 0x800;
        bridge_puts("[Display probe] PP="); bridge_hex(i);
        show(" tear-en=",b); show(" vsync=",b+4); show(" height=",b+8);
        show(" count=",b+0x14); show(" rdptr=",b+0x20); show(" auto=",b+0x30);
        bridge_puts("\n");
    }
}

#ifdef BRIDGE_DISPLAY_HANDOFF
#include "clock_protocol.h"
#ifdef BRIDGE_DISPLAY_SMMU_HANDOFF
int bridge_display_dma_finish(void *, uint64_t, uint64_t);
#endif
static void *display_tree;
static uint64_t framebuffer, framebuffer_size;
static uint32_t width, height, stride, source, intf, command, pp;
static const uint64_t mdp = 0x0c901000;
static int put(const char *key, uint64_t value, unsigned bytes) {
    uint32_t size; unsigned char *p = dt_property(display_tree,"display-controller@c901000",key,&size);
    if (!p || size != bytes) return -1;
    for (unsigned i = 0; i < bytes; ++i) p[i] = (unsigned char)(value >> (8*i));
    return 0;
}
static uint32_t get(const char *key) {
    uint32_t size; const unsigned char *p = dt_property(display_tree,"display-controller@c901000",key,&size);
    if (!p || size != 4) return 0;
    return p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static int live_raster(void) {
    const uint64_t mmcc = 0x0c8c0000;
    const unsigned branches[] = {0x2308,0x2310,0x231c,0x2328,
        intf==1?0x2314:0x2318,intf==1?0x233c:0x2340,
        intf==1?0x2344:0x2348,intf==1?0x2374:0x2378};
    if (!(rd(mmcc+0x2304)&(1u<<31))) return -8;
    for (unsigned i=0;i<sizeof(branches)/sizeof(branches[0]);++i)
        if (!(rd(mmcc+branches[i])&1)) return -9;
    uint64_t b = mdp + (source ? 0x24000 : 0x4000);
    if (rd(b) != (width | height<<16) || rd(b+0xc) != rd(b) || rd(b+8) || rd(b+0x10) ||
        rd(b+0x14) != framebuffer || (rd(b+0x24)&0xffff) != stride ||
        (rd(b+0x30)&~0x100u) != 0x236ff || rd(b+0x34) != 0x03020001 ||
        (rd(b+0x38)&((1u<<0)|(1u<<13)|(1u<<14)|(1u<<22)))) return -1;
    if (((rd(mdp+0x1014)>>4)&7) != intf+1 || !!(rd(mdp+0x1014)&(1u<<17)) != command) return -2;
    if (rd(mdp+0x1018)&(source ? 1u<<11 : 1)) return -3;
    uint64_t dsi = 0x0c994000 + (intf-1)*0x2000;
    if (rd(dsi+0x1f0) || rd(dsi+0x1f4)>>24 != 3 || !(rd(dsi+4)&1) ||
        !!(rd(dsi+4)&2) == command) return -4;
    if (command) {
        b = mdp + 0x70000 + pp*0x800;
        if (!(rd(b+4)&(1u<<19)) || (!rd(b) && !(rd(b+0x30)&(1u<<31)))) return -5;
    } else if (!rd(mdp+0x6a000+intf*0x800)) return -6;
    return 0;
}
int bridge_display_prepare(EFI_BOOT_SERVICES *bs, void *tree, struct boot_args *ba, uint64_t bytes) {
    display_tree = tree; framebuffer = ba->video[0]&~1ULL; framebuffer_size = bytes;
    width = ba->video[3]; height = ba->video[4]; stride = ba->video[2];
    if (!framebuffer || framebuffer>UINT32_MAX || (framebuffer&4095) || !width || !height ||
        width>4096 || height>4096 || stride<width*4 || stride>65536 ||
        bytes<(uint64_t)stride*height || bytes>(1u<<28) ||
        (framebuffer<ba->physBase+ba->memSize && framebuffer+bytes>ba->physBase)) return -1;
    unsigned matches = 0; const unsigned sources[] = {0,8};
    for (unsigned n = 0; n < 2; ++n) {
        uint64_t b = mdp + (sources[n] ? 0x24000 : 0x4000);
        if (rd(b+0x14)==framebuffer && rd(b)==(width|height<<16)) {source=sources[n];++matches;}
    }
    intf = ((rd(mdp+0x1014)>>4)&7)-1; command = !!(rd(mdp+0x1014)&(1u<<17));
    if (matches!=1 || intf<1 || intf>2) return -2;
    if (command) {
        matches = 0;
        for (unsigned n = 0; n < 4; ++n) {
            uint64_t b = mdp + 0x70000 + n*0x800;
            if ((rd(b+4)&(1u<<19)) && (rd(b) || (rd(b+0x30)&(1u<<31)))) {pp=n;++matches;}
        }
        if (matches!=1) return -3;
    }
    EFI_GUID guid = {0x241afae6,0x885f,0x4f6c,{0xa7,0xea,0xc2,0x8e,0xab,0x79,0xc3,0xe5}};
    BridgeClock *clock = 0;
    if (EFI_ERROR(bs->LocateProtocol(&guid,0,(void **)&clock)) || !clock || clock->version<0x10005) return -4;
    // Firmware reports cached/current configuration here; this is NOT a
    // live cycle measurement. Query the exact two CPU names in ClockDxe.
    // No enable/disable, rate, voltage, debug-mux or PMU write is issued.
    const char *cpuNames[] = {"apcs_cluster0_clk","apcs_cluster1_clk"};
    for(unsigned n=0;n<2;++n) {
        UINTN id=0;UINT32 hz=0;BOOLEAN on=0;
        EFI_STATUS found=clock->get_id(clock,(const CHAR8 *)cpuNames[n],&id);
        EFI_STATUS rate=EFI_ERROR(found)?found:clock->get_frequency(clock,id,&hz);
        EFI_STATUS active=EFI_ERROR(found)?found:clock->is_on(clock,id,&on);
        bridge_puts("[CPU firmware clock] name=");bridge_puts(cpuNames[n]);
        bridge_puts(" id-status=");bridge_hex(found);bridge_puts(" rate-status=");bridge_hex(rate);
        bridge_puts(" configured-hz=");bridge_hex(hz);bridge_puts(" on-status=");bridge_hex(active);
        bridge_puts(" on=");bridge_hex(on);bridge_puts(" measured=0\n");
        // The public CalcClockFreqHz slot measures through the firmware debug
        // path. Keep this separate from cached configuration; no rate vote.
        UINT32 observed=0;
        EFI_STATUS calculated=EFI_ERROR(found)?found:
            clock->calculate_frequency(clock,id,&observed);
        UINT64 affinity=0;__asm__ volatile("mrs %0,mpidr_el1":"=r"(affinity));
        bridge_puts("[CPU firmware measurement] name=");bridge_puts(cpuNames[n]);
        bridge_puts(" status=");bridge_hex(calculated);bridge_puts(" observed-hz=");bridge_hex(observed);
        bridge_puts(" boot-mpidr=");bridge_hex(affinity);bridge_puts(" phase=UEFI rate-writes=0\n");
    }
    const char *names[] = {"mmss_mdss_mdp_clk","mmss_mdss_ahb_clk","mmss_mdss_axi_clk","mmss_mdss_vsync_clk",
        intf==1?"mmss_mdss_pclk0_clk":"mmss_mdss_pclk1_clk",
        intf==1?"mmss_mdss_byte0_clk":"mmss_mdss_byte1_clk",
        intf==1?"mmss_mdss_esc0_clk":"mmss_mdss_esc1_clk",
        intf==1?"mmss_mdss_byte0_intf_clk":"mmss_mdss_byte1_intf_clk"};
    for (unsigned n=0;n<sizeof(names)/sizeof(names[0]);++n) {
        UINTN id=0; BOOLEAN on=0; UINT32 frequency=0;
        if (EFI_ERROR(clock->get_id(clock,(const CHAR8 *)names[n],&id)) ||
            EFI_ERROR(clock->enable(clock,id)) || EFI_ERROR(clock->is_on(clock,id,&on)) || !on) {
            bridge_puts("[Display handoff] cannot retain clock ");bridge_puts(names[n]);bridge_puts("\n");return -5;
        }
        clock->get_frequency(clock,id,&frequency);
        bridge_puts("[Display handoff] retained ");bridge_puts(names[n]);bridge_puts(" Hz=");bridge_hex(frequency);bridge_puts("\n");
        UINT32 observed=0;EFI_STATUS measured=clock->calculate_frequency(clock,id,&observed);
        bridge_puts("[Display clock measurement] name=");bridge_puts(names[n]);bridge_puts(" status=");bridge_hex(measured);
        bridge_puts(" debug-observed-hz=");bridge_hex(observed);bridge_puts(" phase=UEFI rate-writes=0\n");

    }
    int result = live_raster();
    bridge_puts("[Display handoff] prepared mode=");bridge_hex(command);bridge_puts(" source=");bridge_hex(source);
    bridge_puts(" interface=");bridge_hex(intf);bridge_puts(" pingpong=");bridge_hex(pp);
    bridge_puts(" result=");bridge_hex((uint64_t)(int64_t)result);bridge_puts("\n");
    return result;
}
int bridge_display_takeover(void) {
    int result = live_raster();
    if (result) return result;
#ifdef BRIDGE_DISPLAY_SMMU_HANDOFF
    if (bridge_display_dma_finish(display_tree, framebuffer, framebuffer_size)) return -11;
#endif
    // EFI has stopped; retained firmware resources and the GOP carveout remain
    // outside the XNU arena. Only the already configured scanout is admitted.
    uint32_t mm = get("display-physical-width-mm");
    if (!mm || put("qcom,display-handoff-version",1,4) ||
        put("qcom,display-handoff-flags",31|(command?64:0),4) ||
        put("qcom,display-interface",intf,4) || put("qcom,display-control",0,4) ||
        put("qcom,display-source",source,4) || put("qcom,display-pingpong",pp,4) ||
        put("qcom,display-vsync-bit",command?12+pp:25+2*intf,4) ||
        put("display-width",width,4) || put("display-height",height,4) || put("display-row-bytes",stride,4) ||
        // Original iOS18 IOMobileFramebufferOpen reads these CFData properties
        // from the service parent. This fixed-mode path renders at the actual
        // raster size and cannot advertise the implicit larger canvas.
        put("canvas-width",width,4) || put("canvas-height",height,4) ||
        put("dot-pitch",(width*254u+mm*5)/(mm*10),4) ||
        put("framebuffer-address",framebuffer,8) || put("framebuffer-size",framebuffer_size,8) ||
        put("qcom,scanout-device-address",framebuffer,8)) return -7;
    if (get("display-backlight")) {
        uint32_t size;
        unsigned char *record = dt_property(display_tree, "power-controller@800f000", "qcom,pmi8998-wled-handoff", &size);
        if (!record || size != 32) return -10;
        uint32_t identifier = record[8] | (uint32_t)record[9]<<8 | (uint32_t)record[10]<<16 | (uint32_t)record[11]<<24;
        if (identifier != get("display-backlight")) return -10;
        for (unsigned i = 0; i < 8; ++i) if (record[i]) return -10;
        // EFI no longer writes display resources. SPMI/WLED identity and
        // actual AP ownership are independently admitted by PMI8998WLED.
        record[0] = 1; record[4] = 3;
    }
    bridge_puts("[Display handoff] post-EFI raster and pinned resources verified; LCD enabled\n");
    return 0;
}
#endif
