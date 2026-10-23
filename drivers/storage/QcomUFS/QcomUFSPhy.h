/* SPDX-License-Identifier: GPL-2.0-only */
/* Calibration tables adapted from Linux v6.12 phy-qcom-qmp-ufs.c.
 * Copyright (c) 2017, The Linux Foundation. All rights reserved.
 * msm8998 uses the QMP v3 configuration; see docs/ufs/reference/sources.json. */
#ifndef _QCOM_UFS_PHY_H
#define _QCOM_UFS_PHY_H
#include <stdint.h>
namespace QcomUFSPhy {
struct Setting { uint16_t offset; uint8_t value; };
constexpr Setting Serdes[] = {
    {0x03c, 0x02}, // QSERDES_V3_COM_SYS_CLK_CTRL
    {0x034, 0x04}, // QSERDES_V3_COM_BIAS_EN_CLKBUFLR_EN
    {0x00c, 0x0a}, // QSERDES_V3_COM_BG_TIMER
    {0x048, 0x07}, // QSERDES_V3_COM_PLL_IVCO
    {0x15c, 0x06}, // QSERDES_V3_COM_CMN_CONFIG
    {0x080, 0xd5}, // QSERDES_V3_COM_SYSCLK_EN_SEL
    {0x088, 0x20}, // QSERDES_V3_COM_RESETSM_CNTRL
    {0x138, 0x30}, // QSERDES_V3_COM_CLK_SELECT
    {0x13c, 0x00}, // QSERDES_V3_COM_HSCLK_SEL
    {0x090, 0x01}, // QSERDES_V3_COM_LOCK_CMP_EN
    {0x0ec, 0x00}, // QSERDES_V3_COM_VCO_TUNE_CTRL
    {0x154, 0x00}, // QSERDES_V3_COM_CORE_CLK_EN
    {0x0f0, 0x04}, // QSERDES_V3_COM_VCO_TUNE_MAP
    {0x164, 0x05}, // QSERDES_V3_COM_SVS_MODE_CLK_SEL
    {0x104, 0xff}, // QSERDES_V3_COM_VCO_TUNE_INITVAL1
    {0x108, 0x00}, // QSERDES_V3_COM_VCO_TUNE_INITVAL2
    {0x0b0, 0x82}, // QSERDES_V3_COM_DEC_START_MODE0
    {0x060, 0x06}, // QSERDES_V3_COM_CP_CTRL_MODE0
    {0x068, 0x16}, // QSERDES_V3_COM_PLL_RCTRL_MODE0
    {0x070, 0x36}, // QSERDES_V3_COM_PLL_CCTRL_MODE0
    {0x0d8, 0x3f}, // QSERDES_V3_COM_INTEGLOOP_GAIN0_MODE0
    {0x0dc, 0x00}, // QSERDES_V3_COM_INTEGLOOP_GAIN1_MODE0
    {0x0f4, 0xda}, // QSERDES_V3_COM_VCO_TUNE1_MODE0
    {0x0f8, 0x01}, // QSERDES_V3_COM_VCO_TUNE2_MODE0
    {0x098, 0xff}, // QSERDES_V3_COM_LOCK_CMP1_MODE0
    {0x09c, 0x0c}, // QSERDES_V3_COM_LOCK_CMP2_MODE0
    {0x0b4, 0x98}, // QSERDES_V3_COM_DEC_START_MODE1
    {0x064, 0x06}, // QSERDES_V3_COM_CP_CTRL_MODE1
    {0x06c, 0x16}, // QSERDES_V3_COM_PLL_RCTRL_MODE1
    {0x074, 0x36}, // QSERDES_V3_COM_PLL_CCTRL_MODE1
    {0x0e0, 0x3f}, // QSERDES_V3_COM_INTEGLOOP_GAIN0_MODE1
    {0x0e4, 0x00}, // QSERDES_V3_COM_INTEGLOOP_GAIN1_MODE1
    {0x0fc, 0xc1}, // QSERDES_V3_COM_VCO_TUNE1_MODE1
    {0x100, 0x00}, // QSERDES_V3_COM_VCO_TUNE2_MODE1
    {0x0a4, 0x32}, // QSERDES_V3_COM_LOCK_CMP1_MODE1
    {0x0a8, 0x0f}, // QSERDES_V3_COM_LOCK_CMP2_MODE1
};
constexpr Setting TX[] = {
    {0x48c, 0x06}, // QSERDES_V3_TX_LANE_MODE_1
    {0x444, 0x04}, // QSERDES_V3_TX_RES_CODE_LANE_OFFSET_TX
    {0x448, 0x07}, // QSERDES_V3_TX_RES_CODE_LANE_OFFSET_RX
    {0x88c, 0x06}, // QSERDES_V3_TX_LANE_MODE_1 lane 1
    {0x844, 0x04}, // QSERDES_V3_TX_RES_CODE_LANE_OFFSET_TX lane 1
    {0x848, 0x07}, // QSERDES_V3_TX_RES_CODE_LANE_OFFSET_RX lane 1
};
constexpr Setting RX[] = {
    {0x708, 0x24}, // QSERDES_V3_RX_SIGDET_LVL
    {0x704, 0x0f}, // QSERDES_V3_RX_SIGDET_CNTRL
    {0x70c, 0x1e}, // QSERDES_V3_RX_SIGDET_DEGLITCH_CNTRL
    {0x71c, 0x40}, // QSERDES_V3_RX_RX_INTERFACE_MODE
    {0x630, 0x0b}, // QSERDES_V3_RX_UCDR_FASTLOCK_FO_GAIN
    {0x67c, 0x5b}, // QSERDES_V3_RX_RX_TERM_BW
    {0x6d4, 0x06}, // QSERDES_V3_RX_RX_EQU_ADAPTOR_CNTRL2
    {0x6d8, 0x04}, // QSERDES_V3_RX_RX_EQU_ADAPTOR_CNTRL3
    {0x6dc, 0x1b}, // QSERDES_V3_RX_RX_EQU_ADAPTOR_CNTRL4
    {0x624, 0x04}, // QSERDES_V3_RX_UCDR_SVS_SO_GAIN_HALF
    {0x628, 0x04}, // QSERDES_V3_RX_UCDR_SVS_SO_GAIN_QUARTER
    {0x62c, 0x04}, // QSERDES_V3_RX_UCDR_SVS_SO_GAIN
    {0x634, 0x4b}, // QSERDES_V3_RX_UCDR_SO_SATURATION_AND_ENABLE
    {0x644, 0x81}, // QSERDES_V3_RX_UCDR_PI_CONTROLS
    {0x63c, 0x80}, // QSERDES_V3_RX_UCDR_FASTLOCK_COUNT_LOW
    {0x764, 0x59}, // QSERDES_V3_RX_RX_MODE_00
    {0xb08, 0x24}, // QSERDES_V3_RX_SIGDET_LVL lane 1
    {0xb04, 0x0f}, // QSERDES_V3_RX_SIGDET_CNTRL lane 1
    {0xb0c, 0x1e}, // QSERDES_V3_RX_SIGDET_DEGLITCH_CNTRL lane 1
    {0xb1c, 0x40}, // QSERDES_V3_RX_RX_INTERFACE_MODE lane 1
    {0xa30, 0x0b}, // QSERDES_V3_RX_UCDR_FASTLOCK_FO_GAIN lane 1
    {0xa7c, 0x5b}, // QSERDES_V3_RX_RX_TERM_BW lane 1
    {0xad4, 0x06}, // QSERDES_V3_RX_RX_EQU_ADAPTOR_CNTRL2 lane 1
    {0xad8, 0x04}, // QSERDES_V3_RX_RX_EQU_ADAPTOR_CNTRL3 lane 1
    {0xadc, 0x1b}, // QSERDES_V3_RX_RX_EQU_ADAPTOR_CNTRL4 lane 1
    {0xa24, 0x04}, // QSERDES_V3_RX_UCDR_SVS_SO_GAIN_HALF lane 1
    {0xa28, 0x04}, // QSERDES_V3_RX_UCDR_SVS_SO_GAIN_QUARTER lane 1
    {0xa2c, 0x04}, // QSERDES_V3_RX_UCDR_SVS_SO_GAIN lane 1
    {0xa34, 0x4b}, // QSERDES_V3_RX_UCDR_SO_SATURATION_AND_ENABLE lane 1
    {0xa44, 0x81}, // QSERDES_V3_RX_UCDR_PI_CONTROLS lane 1
    {0xa3c, 0x80}, // QSERDES_V3_RX_UCDR_FASTLOCK_COUNT_LOW lane 1
    {0xb64, 0x59}, // QSERDES_V3_RX_RX_MODE_00 lane 1
};
constexpr Setting PCS[] = {
    {0xd40, 0x6e}, // QPHY_V3_PCS_UFS_RX_SIGDET_CTRL2
    {0xc2c, 0x0a}, // QPHY_V3_PCS_UFS_TX_LARGE_AMP_DRV_LVL
    {0xc34, 0x02}, // QPHY_V3_PCS_UFS_TX_SMALL_AMP_DRV_LVL
    {0xd34, 0x03}, // QPHY_V3_PCS_UFS_RX_SYM_RESYNC_CTRL
    {0xdbc, 0x43}, // QPHY_V3_PCS_UFS_TX_MID_TERM_CTRL1
    {0xd3c, 0x0f}, // QPHY_V3_PCS_UFS_RX_SIGDET_CTRL1
    {0xd38, 0x9a}, // QPHY_V3_PCS_UFS_RX_MIN_HIBERN8_TIME
    {0xdc4, 0x02}, // QPHY_V3_PCS_UFS_MULTI_LANE_CTRL1
};
constexpr Setting RateB[] = {
    {0x0f0, 0x44}, // QSERDES_V3_COM_VCO_TUNE_MAP
};
constexpr uint32_t Start = 0xc00;
constexpr uint32_t Ready = 0xd60;
constexpr uint32_t Power = 0xc04;
template<class IO, unsigned N> inline void program(IO &io, const Setting (&table)[N]) {
    for (unsigned i = 0; i < N; ++i) io.write(table[i].offset, table[i].value);
}
template<class IO> inline bool start(IO &io) {
    io.write(Power, io.read(Power) | 1);
    program(io, Serdes); program(io, TX); program(io, RX); program(io, PCS); program(io, RateB);
    return true;
}
}
#endif
