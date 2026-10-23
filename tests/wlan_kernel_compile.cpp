// SPDX-License-Identifier: BSD-2-Clause
#include "QcomWLAN.h"
#include "QcomWLANUserClient.h"
#include "WLANFirmware.h"
#include "WLANCopyEngine.h"
#include "WLANIODMA.h"
#include "WLANTarget.h"
#include "WLANQRTR.h"
#include "WLANBoardData.h"
#include <WLANDriverPolicy.h>
static_assert(sizeof(WLAN::CEDescriptor) == 16, "kernel CE wire format");
static_assert(WLAN::DMAAddressBits == 35, "WCN3990 address range");
static_assert(WLAN::CopyEngineCount == 12, "SNOC CE count");

// Emit real IOKit DMA calls, rather than only parsing the template definition.
WLAN::Result compileWLANDMA(WLAN::IOMemoryAPI &api) {
    WLAN::DMABuffer<WLAN::IOMemoryAPI> buffer(api);
    auto result = buffer.allocate(4096,64);
    if (result == WLAN::Result::Success) {
        result = buffer.publish(1);
        if (result == WLAN::Result::Success) result = buffer.isolate(1);
    }
    const auto cleanup = buffer.release();
    return cleanup == WLAN::Result::Success ? result : cleanup;
}
