// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>

// Offsets relative to the complete DWC3 aperture (not globals + 0xc100).
// Register facts cross-checked against Linux v6.12; see docs/usb/reference.
namespace DWC3 {
constexpr uint32_t Aperture = 0xcd00;
constexpr uint32_t GSBUSCFG0 = 0xc100, GCTL = 0xc110, GUCTL1 = 0xc11c;
constexpr uint32_t GSNPSID = 0xc120, GUCTL = 0xc12c;
constexpr uint32_t GHWPARAMS0 = 0xc140, GHWPARAMS1 = 0xc144, GHWPARAMS3 = 0xc14c;
constexpr uint32_t GUSB2PHYCFG = 0xc200, GUSB3PIPECTL = 0xc2c0;
constexpr uint32_t DCFG = 0xc700, DCTL = 0xc704, DEVTEN = 0xc708, DSTS = 0xc70c;
constexpr uint32_t DGCMDPAR = 0xc710, DGCMD = 0xc714;
constexpr uint32_t DALEPENA = 0xc720;
constexpr uint32_t PortMask = 3u << 12, PortHost = 1u << 12, PortDevice = 2u << 12;
constexpr uint32_t Run = 1u << 31, SoftReset = 1u << 30, Halted = 1u << 22;
constexpr uint32_t CommandActive = 1u << 10, CommandForce = 1u << 11;
constexpr uint32_t CommandInterrupt = 1u << 8;
constexpr uint32_t DeviceNotification = 7, FunctionWake = 1;
constexpr uint32_t SetPeriodicParameters = 2;
constexpr uint32_t AcceptU1 = 1u << 9, InitiateU1 = 1u << 10;
constexpr uint32_t AcceptU2 = 1u << 11, InitiateU2 = 1u << 12;
constexpr uint32_t USB3LinkEnable = AcceptU1 | InitiateU1 | AcceptU2 | InitiateU2;
constexpr uint32_t EventMask = 1u << 31;
constexpr uint32_t eventAddressLo(unsigned n) { return 0xc400 + n * 16; }
constexpr uint32_t eventAddressHi(unsigned n) { return 0xc404 + n * 16; }
constexpr uint32_t eventSize(unsigned n) { return 0xc408 + n * 16; }
constexpr uint32_t eventCount(unsigned n) { return 0xc40c + n * 16; }
constexpr uint32_t endpoint(unsigned n) { return 0xc800 + n * 16; }
enum class Command : uint8_t {
    Configure = 1, Resource = 2, SetStall = 4, ClearStall = 5,
    Start = 6, Update = 7, End = 8, StartConfiguration = 9
};

// The Qualcomm QSCRATCH block is a separate MMIO resource.
namespace QScratch {
constexpr uint32_t General = 0x08, HSControl = 0x10, SSControl = 0x30;
constexpr uint32_t VbusValid = 1u << 20, SessionSelect = 1u << 28;
constexpr uint32_t PowerPresent = 1u << 24;
constexpr uint32_t PipeUTMISelect=1u, PipePhyStatusSoftware=1u<<3, PipeClockDisable=1u<<8;
}

struct alignas(16) TRB {
    uint32_t addressLo, addressHi, size, control;
};
static_assert(sizeof(TRB) == 16 && alignof(TRB) == 16, "DWC3 DMA TRB layout");
enum class TRBType : uint8_t {
    Normal = 1, Setup = 2, Status2 = 3, Status3 = 4, ControlData = 5,
    IsoFirst = 6, Iso = 7, Link = 8
};
constexpr uint32_t Owned = 1, Last = 2, Chain = 4, ContinueShort = 8;
constexpr uint32_t InterruptShort = 1u << 10, InterruptComplete = 1u << 11;
constexpr uint32_t TRBLengthMask = 0x00ffffff;

struct Event {
    uint32_t word;
    bool device() const { return word & 1; }
    bool deviceSpecific() const { return (word & 0xff) == 1; }
    unsigned endpointNumber() const { return (word >> 1) & 31; }
    unsigned type() const { return device() ? (word >> 8) & 15 : (word >> 6) & 15; }
    unsigned status() const { return (word >> 12) & 15; }
    unsigned parameters() const { return word >> 16; }
};
}
