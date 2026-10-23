// SPDX-License-Identifier: BSD-2-Clause
#include "QcomBluetooth.h"
#include "QcomBluetoothUserClient.h"
#include "BluetoothFirmware.h"
#include "BluetoothBoot.h"
static_assert(Bluetooth::MaxH4Frame == 4101, "bounded H4 receive storage");
static_assert(Bluetooth::FirmwareSegmentBytes + 2 <= 255, "HCI command parameter length");
static_assert(Bluetooth::UARTBase != 0x0c1b0000, "Bluetooth must not claim the console UART");
