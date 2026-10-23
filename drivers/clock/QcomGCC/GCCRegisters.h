// SPDX-License-Identifier: GPL-2.0-only
// MSM8998 GCC register/frequency data from Linux v6.12, The Linux Foundation.
// Reviewed subset: BLSP, UFS, USB, GPU, MSS, PCIe, SDCC, PDM and TSIF. IDs are GCC IDs.
#pragma once
#include <stdint.h>
namespace GCC {
constexpr uint32_t NoClock = 0xffffffffU;
struct Rate { uint32_t hz, source, divider, m, n; };
struct Root { uint32_t id; const char *name; uint32_t reg, width; const Rate *rates; unsigned count; };
struct Branch { uint32_t id; const char *name; uint32_t reg, mask, halt, parent, mode; int domain; uint32_t pllDivider = 0; bool critical = false; };
struct Reset { uint32_t id; const char *name; uint32_t reg; };
struct Domain { uint32_t id; const char *name; uint32_t reg; bool retain, alwaysOn; };
static constexpr Rate rates_blsp1_qup1_i2c_apps_clk_src[] = {{19200000,0,1,0,0},{50000000,1,23,0,0}};
static constexpr Rate rates_blsp1_qup1_spi_apps_clk_src[] = {{960000,0,19,1,2},{4800000,0,7,0,0},{9600000,0,3,0,0},{15000000,1,19,1,4},{19200000,0,1,0,0},{25000000,1,23,1,2},{50000000,1,23,0,0}};
static constexpr Rate rates_blsp1_qup2_i2c_apps_clk_src[] = {{19200000,0,1,0,0},{50000000,1,23,0,0}};
static constexpr Rate rates_blsp1_qup2_spi_apps_clk_src[] = {{960000,0,19,1,2},{4800000,0,7,0,0},{9600000,0,3,0,0},{15000000,1,19,1,4},{19200000,0,1,0,0},{25000000,1,23,1,2},{50000000,1,23,0,0}};
static constexpr Rate rates_blsp1_qup3_i2c_apps_clk_src[] = {{19200000,0,1,0,0},{50000000,1,23,0,0}};
static constexpr Rate rates_blsp1_qup3_spi_apps_clk_src[] = {{960000,0,19,1,2},{4800000,0,7,0,0},{9600000,0,3,0,0},{15000000,1,19,1,4},{19200000,0,1,0,0},{25000000,1,23,1,2},{50000000,1,23,0,0}};
static constexpr Rate rates_blsp1_qup4_i2c_apps_clk_src[] = {{19200000,0,1,0,0},{50000000,1,23,0,0}};
static constexpr Rate rates_blsp1_qup4_spi_apps_clk_src[] = {{960000,0,19,1,2},{4800000,0,7,0,0},{9600000,0,3,0,0},{15000000,1,19,1,4},{19200000,0,1,0,0},{25000000,1,23,1,2},{50000000,1,23,0,0}};
static constexpr Rate rates_blsp1_qup5_i2c_apps_clk_src[] = {{19200000,0,1,0,0},{50000000,1,23,0,0}};
static constexpr Rate rates_blsp1_qup5_spi_apps_clk_src[] = {{960000,0,19,1,2},{4800000,0,7,0,0},{9600000,0,3,0,0},{15000000,1,19,1,4},{19200000,0,1,0,0},{25000000,1,23,1,2},{50000000,1,23,0,0}};
static constexpr Rate rates_blsp1_qup6_i2c_apps_clk_src[] = {{19200000,0,1,0,0},{50000000,1,23,0,0}};
static constexpr Rate rates_blsp1_qup6_spi_apps_clk_src[] = {{960000,0,19,1,2},{4800000,0,7,0,0},{9600000,0,3,0,0},{15000000,1,19,1,4},{19200000,0,1,0,0},{25000000,1,23,1,2},{50000000,1,23,0,0}};
static constexpr Rate rates_blsp1_uart1_apps_clk_src[] = {{3686400,1,1,96,15625},{7372800,1,1,192,15625},{14745600,1,1,384,15625},{16000000,1,9,2,15},{19200000,0,1,0,0},{24000000,1,9,1,5},{32000000,1,1,4,75},{40000000,1,29,0,0},{46400000,1,1,29,375},{48000000,1,24,0,0},{51200000,1,1,32,375},{56000000,1,1,7,75},{58982400,1,1,1536,15625},{60000000,1,19,0,0},{63157895,1,18,0,0}};
static constexpr Rate rates_blsp1_uart2_apps_clk_src[] = {{3686400,1,1,96,15625},{7372800,1,1,192,15625},{14745600,1,1,384,15625},{16000000,1,9,2,15},{19200000,0,1,0,0},{24000000,1,9,1,5},{32000000,1,1,4,75},{40000000,1,29,0,0},{46400000,1,1,29,375},{48000000,1,24,0,0},{51200000,1,1,32,375},{56000000,1,1,7,75},{58982400,1,1,1536,15625},{60000000,1,19,0,0},{63157895,1,18,0,0}};
static constexpr Rate rates_blsp1_uart3_apps_clk_src[] = {{3686400,1,1,96,15625},{7372800,1,1,192,15625},{14745600,1,1,384,15625},{16000000,1,9,2,15},{19200000,0,1,0,0},{24000000,1,9,1,5},{32000000,1,1,4,75},{40000000,1,29,0,0},{46400000,1,1,29,375},{48000000,1,24,0,0},{51200000,1,1,32,375},{56000000,1,1,7,75},{58982400,1,1,1536,15625},{60000000,1,19,0,0},{63157895,1,18,0,0}};
static constexpr Rate rates_blsp2_qup1_i2c_apps_clk_src[] = {{19200000,0,1,0,0},{50000000,1,23,0,0}};
static constexpr Rate rates_blsp2_qup1_spi_apps_clk_src[] = {{960000,0,19,1,2},{4800000,0,7,0,0},{9600000,0,3,0,0},{15000000,1,19,1,4},{19200000,0,1,0,0},{25000000,1,23,1,2},{50000000,1,23,0,0}};
static constexpr Rate rates_blsp2_qup2_i2c_apps_clk_src[] = {{19200000,0,1,0,0},{50000000,1,23,0,0}};
static constexpr Rate rates_blsp2_qup2_spi_apps_clk_src[] = {{960000,0,19,1,2},{4800000,0,7,0,0},{9600000,0,3,0,0},{15000000,1,19,1,4},{19200000,0,1,0,0},{25000000,1,23,1,2},{50000000,1,23,0,0}};
static constexpr Rate rates_blsp2_qup3_i2c_apps_clk_src[] = {{19200000,0,1,0,0},{50000000,1,23,0,0}};
static constexpr Rate rates_blsp2_qup3_spi_apps_clk_src[] = {{960000,0,19,1,2},{4800000,0,7,0,0},{9600000,0,3,0,0},{15000000,1,19,1,4},{19200000,0,1,0,0},{25000000,1,23,1,2},{50000000,1,23,0,0}};
static constexpr Rate rates_blsp2_qup4_i2c_apps_clk_src[] = {{19200000,0,1,0,0},{50000000,1,23,0,0}};
static constexpr Rate rates_blsp2_qup4_spi_apps_clk_src[] = {{960000,0,19,1,2},{4800000,0,7,0,0},{9600000,0,3,0,0},{15000000,1,19,1,4},{19200000,0,1,0,0},{25000000,1,23,1,2},{50000000,1,23,0,0}};
static constexpr Rate rates_blsp2_qup5_i2c_apps_clk_src[] = {{19200000,0,1,0,0},{50000000,1,23,0,0}};
static constexpr Rate rates_blsp2_qup5_spi_apps_clk_src[] = {{960000,0,19,1,2},{4800000,0,7,0,0},{9600000,0,3,0,0},{15000000,1,19,1,4},{19200000,0,1,0,0},{25000000,1,23,1,2},{50000000,1,23,0,0}};
static constexpr Rate rates_blsp2_qup6_i2c_apps_clk_src[] = {{19200000,0,1,0,0},{50000000,1,23,0,0}};
static constexpr Rate rates_blsp2_qup6_spi_apps_clk_src[] = {{960000,0,19,1,2},{4800000,0,7,0,0},{9600000,0,3,0,0},{15000000,1,19,1,4},{19200000,0,1,0,0},{25000000,1,23,1,2},{50000000,1,23,0,0}};
static constexpr Rate rates_blsp2_uart1_apps_clk_src[] = {{3686400,1,1,96,15625},{7372800,1,1,192,15625},{14745600,1,1,384,15625},{16000000,1,9,2,15},{19200000,0,1,0,0},{24000000,1,9,1,5},{32000000,1,1,4,75},{40000000,1,29,0,0},{46400000,1,1,29,375},{48000000,1,24,0,0},{51200000,1,1,32,375},{56000000,1,1,7,75},{58982400,1,1,1536,15625},{60000000,1,19,0,0},{63157895,1,18,0,0}};
static constexpr Rate rates_blsp2_uart2_apps_clk_src[] = {{3686400,1,1,96,15625},{7372800,1,1,192,15625},{14745600,1,1,384,15625},{16000000,1,9,2,15},{19200000,0,1,0,0},{24000000,1,9,1,5},{32000000,1,1,4,75},{40000000,1,29,0,0},{46400000,1,1,29,375},{48000000,1,24,0,0},{51200000,1,1,32,375},{56000000,1,1,7,75},{58982400,1,1,1536,15625},{60000000,1,19,0,0},{63157895,1,18,0,0}};
static constexpr Rate rates_blsp2_uart3_apps_clk_src[] = {{3686400,1,1,96,15625},{7372800,1,1,192,15625},{14745600,1,1,384,15625},{16000000,1,9,2,15},{19200000,0,1,0,0},{24000000,1,9,1,5},{32000000,1,1,4,75},{40000000,1,29,0,0},{46400000,1,1,29,375},{48000000,1,24,0,0},{51200000,1,1,32,375},{56000000,1,1,7,75},{58982400,1,1,1536,15625},{60000000,1,19,0,0},{63157895,1,18,0,0}};
static constexpr Rate rates_pcie_aux_clk_src[] = {{1010526,0,1,1,19}};
static constexpr Rate rates_pdm2_clk_src[] = {{60000000,1,19,0,0}};
static constexpr Rate rates_sdcc2_apps_clk_src[] = {{144000,0,31,3,25},{400000,0,23,1,4},{20000000,1,29,1,2},{25000000,1,23,1,2},{50000000,1,23,0,0},{100000000,1,11,0,0},{200000000,1,5,0,0}};
static constexpr Rate rates_sdcc4_apps_clk_src[] = {{144000,0,31,3,25},{400000,0,23,1,4},{20000000,1,29,1,2},{25000000,1,23,1,2},{50000000,1,23,0,0},{100000000,1,11,0,0}};
static constexpr Rate rates_tsif_ref_clk_src[] = {{105495,0,1,1,182}};
static constexpr Rate rates_ufs_axi_clk_src[] = {{100000000,1,11,0,0},{200000000,1,5,0,0},{240000000,1,4,0,0}};
static constexpr Rate rates_ufs_unipro_core_clk_src[] = {{37500000,1,31,0,0},{75000000,1,15,0,0},{150000000,1,7,0,0}};
static constexpr Rate rates_usb30_master_clk_src[] = {{19200000,0,1,0,0},{60000000,1,19,0,0},{120000000,1,9,0,0},{150000000,1,7,0,0}};
static constexpr Rate rates_usb30_mock_utmi_clk_src[] = {{19200000,0,1,0,0}};
static constexpr Rate rates_usb3_phy_aux_clk_src[] = {{1200000,0,31,0,0}};
static constexpr Root roots[] = {
    {0,"blsp1_qup1_i2c_apps_clk_src",0x19020,0,rates_blsp1_qup1_i2c_apps_clk_src,2},
    {1,"blsp1_qup1_spi_apps_clk_src",0x1900c,8,rates_blsp1_qup1_spi_apps_clk_src,7},
    {2,"blsp1_qup2_i2c_apps_clk_src",0x1b020,0,rates_blsp1_qup2_i2c_apps_clk_src,2},
    {3,"blsp1_qup2_spi_apps_clk_src",0x1b00c,8,rates_blsp1_qup2_spi_apps_clk_src,7},
    {4,"blsp1_qup3_i2c_apps_clk_src",0x1d020,0,rates_blsp1_qup3_i2c_apps_clk_src,2},
    {5,"blsp1_qup3_spi_apps_clk_src",0x1d00c,8,rates_blsp1_qup3_spi_apps_clk_src,7},
    {6,"blsp1_qup4_i2c_apps_clk_src",0x1f020,0,rates_blsp1_qup4_i2c_apps_clk_src,2},
    {7,"blsp1_qup4_spi_apps_clk_src",0x1f00c,8,rates_blsp1_qup4_spi_apps_clk_src,7},
    {8,"blsp1_qup5_i2c_apps_clk_src",0x21020,0,rates_blsp1_qup5_i2c_apps_clk_src,2},
    {9,"blsp1_qup5_spi_apps_clk_src",0x2100c,8,rates_blsp1_qup5_spi_apps_clk_src,7},
    {10,"blsp1_qup6_i2c_apps_clk_src",0x23020,0,rates_blsp1_qup6_i2c_apps_clk_src,2},
    {11,"blsp1_qup6_spi_apps_clk_src",0x2300c,8,rates_blsp1_qup6_spi_apps_clk_src,7},
    {12,"blsp1_uart1_apps_clk_src",0x1a00c,16,rates_blsp1_uart1_apps_clk_src,15},
    {13,"blsp1_uart2_apps_clk_src",0x1c00c,16,rates_blsp1_uart2_apps_clk_src,15},
    {14,"blsp1_uart3_apps_clk_src",0x1e00c,16,rates_blsp1_uart3_apps_clk_src,15},
    {15,"blsp2_qup1_i2c_apps_clk_src",0x26020,0,rates_blsp2_qup1_i2c_apps_clk_src,2},
    {16,"blsp2_qup1_spi_apps_clk_src",0x2600c,8,rates_blsp2_qup1_spi_apps_clk_src,7},
    {17,"blsp2_qup2_i2c_apps_clk_src",0x28020,0,rates_blsp2_qup2_i2c_apps_clk_src,2},
    {18,"blsp2_qup2_spi_apps_clk_src",0x2800c,8,rates_blsp2_qup2_spi_apps_clk_src,7},
    {19,"blsp2_qup3_i2c_apps_clk_src",0x2a020,0,rates_blsp2_qup3_i2c_apps_clk_src,2},
    {20,"blsp2_qup3_spi_apps_clk_src",0x2a00c,8,rates_blsp2_qup3_spi_apps_clk_src,7},
    {21,"blsp2_qup4_i2c_apps_clk_src",0x2c020,0,rates_blsp2_qup4_i2c_apps_clk_src,2},
    {22,"blsp2_qup4_spi_apps_clk_src",0x2c00c,8,rates_blsp2_qup4_spi_apps_clk_src,7},
    {23,"blsp2_qup5_i2c_apps_clk_src",0x2e020,0,rates_blsp2_qup5_i2c_apps_clk_src,2},
    {24,"blsp2_qup5_spi_apps_clk_src",0x2e00c,8,rates_blsp2_qup5_spi_apps_clk_src,7},
    {25,"blsp2_qup6_i2c_apps_clk_src",0x30020,0,rates_blsp2_qup6_i2c_apps_clk_src,2},
    {26,"blsp2_qup6_spi_apps_clk_src",0x3000c,8,rates_blsp2_qup6_spi_apps_clk_src,7},
    {27,"blsp2_uart1_apps_clk_src",0x2700c,16,rates_blsp2_uart1_apps_clk_src,15},
    {28,"blsp2_uart2_apps_clk_src",0x2900c,16,rates_blsp2_uart2_apps_clk_src,15},
    {29,"blsp2_uart3_apps_clk_src",0x2b00c,16,rates_blsp2_uart3_apps_clk_src,15},
    {157,"pcie_aux_clk_src",0x6c000,16,rates_pcie_aux_clk_src,1},
    {158,"pdm2_clk_src",0x33010,0,rates_pdm2_clk_src,1},
    {159,"sdcc2_apps_clk_src",0x14010,8,rates_sdcc2_apps_clk_src,7},
    {160,"sdcc4_apps_clk_src",0x16010,8,rates_sdcc4_apps_clk_src,6},
    {161,"tsif_ref_clk_src",0x36010,8,rates_tsif_ref_clk_src,1},
    {162,"ufs_axi_clk_src",0x75018,8,rates_ufs_axi_clk_src,3},
    {177,"ufs_unipro_core_clk_src",0x76028,8,rates_ufs_unipro_core_clk_src,3},
    {163,"usb30_master_clk_src",0xf014,8,rates_usb30_master_clk_src,4},
    {164,"usb30_mock_utmi_clk_src",0xf028,0,rates_usb30_mock_utmi_clk_src,1},
    {165,"usb3_phy_aux_clk_src",0x5000c,0,rates_usb3_phy_aux_clk_src,1},
};
static constexpr Branch branches[] = {
    {36,"gcc_bimc_mss_q6_axi_clk",0x4401c,1,0x4401c,NoClock,0,-1},
    {171,"gcc_mss_cfg_ahb_clk",0x8a000,1,0x8a000,NoClock,0,-1},
    {172,"gcc_boot_rom_ahb_clk",0x52004,1u<<10,0x38004,NoClock,1,-1},
    // No inferred PLL rate or implicit write grant: unlike the MMSS/GPU
    // outputs below, upstream provides no parent relationship for this vote.
    {173,"gcc_mss_gpll0_div_clk_src",0x5200c,1u<<2,0,NoClock,3,-1},
    {174,"gcc_mss_snoc_axi_clk",0x8a03c,1,0x8a03c,NoClock,0,-1},
    {175,"gcc_mss_mnoc_bimc_axi_clk",0x8a004,1,0x8a004,NoClock,0,-1},
    {75,"gcc_gpu_bimc_gfx_clk",0x71010,1,0x71010,NoClock,2,-1},
    {76,"gcc_gpu_bimc_gfx_src_clk",0x7100c,1,0x7100c,NoClock,0,-1},
    // Secure SMMU contexts depend on this leaf. It must never be gated.
    {77,"gcc_gpu_cfg_ahb_clk",0x71004,1,0x71004,NoClock,2,-1,0,true},
    {78,"gcc_gpu_snoc_dvm_gfx_clk",0x71018,1,0x71018,NoClock,0,-1},
    {176,"gcc_bimc_gfx_clk",0x46040,1,0x46040,NoClock,2,-1},
    {178,"gcc_mmss_gpll0_clk",0x5200c,0x2,0,NoClock,3,-1,1},
    {184,"gcc_mmss_gpll0_div_clk",0x5200c,0x1,0,NoClock,3,-1,2},
    {185,"gcc_gpu_gpll0_div_clk",0x5200c,0x8,0,NoClock,3,-1,2},
    {186,"gcc_gpu_gpll0_clk",0x5200c,0x10,0,NoClock,3,-1,1},
    {31,"gcc_aggre1_ufs_axi_clk",0x82028,0x1,0x82028,162,0,1},
    {32,"gcc_aggre1_usb3_axi_clk",0x82024,0x1,0x82024,163,0,2},
    {37,"gcc_blsp1_ahb_clk",0x52004,0x20000,0x17004,NoClock,1,-1},
    {38,"gcc_blsp1_qup1_i2c_apps_clk",0x19008,0x1,0x19008,0,0,-1},
    {39,"gcc_blsp1_qup1_spi_apps_clk",0x19004,0x1,0x19004,1,0,-1},
    {40,"gcc_blsp1_qup2_i2c_apps_clk",0x1b008,0x1,0x1b008,2,0,-1},
    {41,"gcc_blsp1_qup2_spi_apps_clk",0x1b004,0x1,0x1b004,3,0,-1},
    {42,"gcc_blsp1_qup3_i2c_apps_clk",0x1d008,0x1,0x1d008,4,0,-1},
    {43,"gcc_blsp1_qup3_spi_apps_clk",0x1d004,0x1,0x1d004,5,0,-1},
    {44,"gcc_blsp1_qup4_i2c_apps_clk",0x1f008,0x1,0x1f008,6,0,-1},
    {45,"gcc_blsp1_qup4_spi_apps_clk",0x1f004,0x1,0x1f004,7,0,-1},
    {46,"gcc_blsp1_qup5_i2c_apps_clk",0x21008,0x1,0x21008,8,0,-1},
    {47,"gcc_blsp1_qup5_spi_apps_clk",0x21004,0x1,0x21004,9,0,-1},
    {48,"gcc_blsp1_qup6_i2c_apps_clk",0x23008,0x1,0x23008,10,0,-1},
    {49,"gcc_blsp1_qup6_spi_apps_clk",0x23004,0x1,0x23004,11,0,-1},
    {50,"gcc_blsp1_sleep_clk",0x52004,0x10000,0x17008,NoClock,1,-1},
    {51,"gcc_blsp1_uart1_apps_clk",0x1a004,0x1,0x1a004,12,0,-1},
    {52,"gcc_blsp1_uart2_apps_clk",0x1c004,0x1,0x1c004,13,0,-1},
    {53,"gcc_blsp1_uart3_apps_clk",0x1e004,0x1,0x1e004,14,0,-1},
    {54,"gcc_blsp2_ahb_clk",0x52004,0x8000,0x25004,NoClock,1,-1},
    {55,"gcc_blsp2_qup1_i2c_apps_clk",0x26008,0x1,0x26008,15,0,-1},
    {56,"gcc_blsp2_qup1_spi_apps_clk",0x26004,0x1,0x26004,16,0,-1},
    {57,"gcc_blsp2_qup2_i2c_apps_clk",0x28008,0x1,0x28008,17,0,-1},
    {58,"gcc_blsp2_qup2_spi_apps_clk",0x28004,0x1,0x28004,18,0,-1},
    {59,"gcc_blsp2_qup3_i2c_apps_clk",0x2a008,0x1,0x2a008,19,0,-1},
    {60,"gcc_blsp2_qup3_spi_apps_clk",0x2a004,0x1,0x2a004,20,0,-1},
    {61,"gcc_blsp2_qup4_i2c_apps_clk",0x2c008,0x1,0x2c008,21,0,-1},
    {62,"gcc_blsp2_qup4_spi_apps_clk",0x2c004,0x1,0x2c004,22,0,-1},
    {63,"gcc_blsp2_qup5_i2c_apps_clk",0x2e008,0x1,0x2e008,23,0,-1},
    {64,"gcc_blsp2_qup5_spi_apps_clk",0x2e004,0x1,0x2e004,24,0,-1},
    {65,"gcc_blsp2_qup6_i2c_apps_clk",0x30008,0x1,0x30008,25,0,-1},
    {66,"gcc_blsp2_qup6_spi_apps_clk",0x30004,0x1,0x30004,26,0,-1},
    {67,"gcc_blsp2_sleep_clk",0x52004,0x4000,0x25008,NoClock,1,-1},
    {68,"gcc_blsp2_uart1_apps_clk",0x27004,0x1,0x27004,27,0,-1},
    {69,"gcc_blsp2_uart2_apps_clk",0x29004,0x1,0x29004,28,0,-1},
    {70,"gcc_blsp2_uart3_apps_clk",0x2b004,0x1,0x2b004,29,0,-1},
    {71,"gcc_cfg_noc_usb3_axi_clk",0x5018,0x1,0x5018,163,0,2},
    {91,"gcc_pcie_0_aux_clk",0x6b014,0x1,0x6b014,157,0,0},
    {92,"gcc_pcie_0_cfg_ahb_clk",0x6b010,0x1,0x6b010,NoClock,0,0},
    {93,"gcc_pcie_0_mstr_axi_clk",0x6b00c,0x1,0x6b00c,NoClock,0,0},
    {94,"gcc_pcie_0_pipe_clk",0x6b018,0x1,0x6b018,NoClock,2,0},
    {95,"gcc_pcie_0_slv_axi_clk",0x6b008,0x1,0x6b008,NoClock,0,0},
    {96,"gcc_pcie_phy_aux_clk",0x6f004,0x1,0x6f004,157,0,-1},
    {97,"gcc_pdm2_clk",0x3300c,0x1,0x3300c,158,0,-1},
    {98,"gcc_pdm_ahb_clk",0x33004,0x1,0x33004,NoClock,0,-1},
    {99,"gcc_pdm_xo4_clk",0x33008,0x1,0x33008,NoClock,0,-1},
    {101,"gcc_sdcc2_ahb_clk",0x14008,0x1,0x14008,NoClock,0,-1},
    {102,"gcc_sdcc2_apps_clk",0x14004,0x1,0x14004,159,0,-1},
    {103,"gcc_sdcc4_ahb_clk",0x16008,0x1,0x16008,NoClock,0,-1},
    {104,"gcc_sdcc4_apps_clk",0x16004,0x1,0x16004,160,0,-1},
    {105,"gcc_tsif_ahb_clk",0x36004,0x1,0x36004,NoClock,0,-1},
    {106,"gcc_tsif_inactivity_timers_clk",0x3600c,0x1,0x3600c,NoClock,0,-1},
    {107,"gcc_tsif_ref_clk",0x36008,0x1,0x36008,161,0,-1},
    {108,"gcc_ufs_ahb_clk",0x7500c,0x1,0x7500c,NoClock,0,1},
    {109,"gcc_ufs_axi_clk",0x75008,0x1,0x75008,162,0,1},
    {110,"gcc_ufs_ice_core_clk",0x7600c,0x1,0x7600c,NoClock,0,1},
    {111,"gcc_ufs_phy_aux_clk",0x76040,0x1,0x76040,NoClock,0,1},
    {112,"gcc_ufs_rx_symbol_0_clk",0x75014,0x1,0x75014,NoClock,2,1},
    {113,"gcc_ufs_rx_symbol_1_clk",0x7605c,0x1,0x7605c,NoClock,2,1},
    {114,"gcc_ufs_tx_symbol_0_clk",0x75010,0x1,0x75010,NoClock,2,1},
    {115,"gcc_ufs_unipro_core_clk",0x76008,0x1,0x76008,177,0,1},
    {116,"gcc_usb30_master_clk",0xf008,0x1,0xf008,163,0,2},
    {117,"gcc_usb30_mock_utmi_clk",0xf010,0x1,0xf010,164,0,2},
    {118,"gcc_usb30_sleep_clk",0xf00c,0x1,0xf00c,NoClock,0,2},
    {119,"gcc_usb3_phy_aux_clk",0x50000,0x1,0x50000,165,0,-1},
    {120,"gcc_usb3_phy_pipe_clk",0x50004,0x1,0x50004,NoClock,2,-1},
    {121,"gcc_usb_phy_cfg_ahb2phy_clk",0x6a004,0x1,0x6a004,NoClock,0,-1},
    {168,"gcc_ufs_clkref_clk",0x88004,0x1,0x88004,NoClock,0,1},
    {166,"gcc_usb3_clkref_clk",0x88008,0x1,0x88008,NoClock,0,-1},
    {169,"gcc_pcie_clkref_clk",0x8800c,0x1,0x8800c,NoClock,0,-1},
    {170,"gcc_rx1_usb2_clkref_clk",0x88014,0x1,0x88014,NoClock,2,-1},
};
static constexpr Reset resets[] = {
    {0,"gcc_blsp1_qup1_bcr",0x19000},
    {1,"gcc_blsp1_qup2_bcr",0x1b000},
    {2,"gcc_blsp1_qup3_bcr",0x1d000},
    {3,"gcc_blsp1_qup4_bcr",0x1f000},
    {4,"gcc_blsp1_qup5_bcr",0x21000},
    {5,"gcc_blsp1_qup6_bcr",0x23000},
    {6,"gcc_blsp2_qup1_bcr",0x26000},
    {7,"gcc_blsp2_qup2_bcr",0x28000},
    {8,"gcc_blsp2_qup3_bcr",0x2a000},
    {9,"gcc_blsp2_qup4_bcr",0x2c000},
    {10,"gcc_blsp2_qup5_bcr",0x2e000},
    {11,"gcc_blsp2_qup6_bcr",0x30000},
    {12,"gcc_pcie_0_bcr",0x6b000},
    {13,"gcc_pdm_bcr",0x33000},
    {14,"gcc_sdcc2_bcr",0x14000},
    {15,"gcc_sdcc4_bcr",0x16000},
    {16,"gcc_tsif_bcr",0x36000},
    {17,"gcc_ufs_bcr",0x75000},
    {18,"gcc_usb_30_bcr",0xf000},
    {106,"gcc_qusb2phy_prim_bcr",0x12000},
    {107,"gcc_qusb2phy_sec_bcr",0x12004},
    {108,"gcc_mss_restart",0x79000},
    {27,"gcc_blsp1_bcr",0x17000},
    {28,"gcc_blsp1_uart1_bcr",0x1a000},
    {29,"gcc_blsp1_uart2_bcr",0x1c000},
    {30,"gcc_blsp1_uart3_bcr",0x1e000},
    {33,"gcc_blsp2_bcr",0x25000},
    {34,"gcc_blsp2_uart1_bcr",0x27000},
    {35,"gcc_blsp2_uart2_bcr",0x29000},
    {36,"gcc_blsp2_uart3_bcr",0x2b000},
    {39,"gcc_tsif_0_reset",0x36024},
    {40,"gcc_tsif_1_reset",0x36028},
    {48,"gcc_spdm_bcr",0x40000},
    {69,"gcc_usb3_phy_bcr",0x50020},
    {70,"gcc_usb3phy_phy_bcr",0x50024},
    {71,"gcc_usb3_dp_phy_bcr",0x50028},
    {74,"gcc_usb_phy_cfg_ahb2phy_bcr",0x6a000},
    {75,"gcc_pcie_0_link_down_bcr",0x6c014},
    {76,"gcc_pcie_0_phy_bcr",0x6c01c},
    {77,"gcc_pcie_0_nocsr_com_phy_bcr",0x6c020},
    {78,"gcc_pcie_phy_bcr",0x6f000},
    {79,"gcc_pcie_phy_nocsr_com_phy_bcr",0x6f00c},
    {80,"gcc_pcie_phy_cfg_ahb_bcr",0x6f010},
    {81,"gcc_pcie_phy_com_bcr",0x6f014},
};
static constexpr Domain domains[] = {
    {0,"pcie_0_gdsc",0x6b004,false,false},
    {1,"ufs_gdsc",0x75004,false,false},
    {2,"usb_30_gdsc",0xf004,true,false},
    {3,"lpass_adsp_gdsc",0x7d034,false,false},
    {4,"lpass_core_gdsc",0x7d038,false,true},
};
constexpr unsigned RootCount = sizeof(roots)/sizeof(roots[0]);
constexpr unsigned BranchCount = sizeof(branches)/sizeof(branches[0]);
constexpr unsigned ResetCount = sizeof(resets)/sizeof(resets[0]);
constexpr unsigned DomainCount = sizeof(domains)/sizeof(domains[0]);
}
