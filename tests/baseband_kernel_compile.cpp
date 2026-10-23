// SPDX-License-Identifier: GPL-2.0-only
#include "BasebandFirmware.h"
#include "BasebandQ6.h"
#include "BasebandSMEM.h"
#include "BasebandSMEMPool.h"
#include <SMEMProtocol.h>
#include "BasebandFIFO.h"
#include "BasebandLink.h"
#include "BasebandBoot.h"
#include "BasebandSecure.h"
#include "BasebandQRTR.h"
#include "BasebandRegisters.h"
#include "BasebandHaltIO.h"
#include "BasebandMemory.h"
#include "BasebandMemoryAccess.h"
#include "BasebandNative.h"
#include "BasebandSSR.h"
#include "BasebandTFTP.h"
#include <IOKit/IOLib.h>
static_assert(sizeof(Baseband::SMP2P::Observation) <= 32, "bounded status");
struct BasebandCompileTX {
    Baseband::Result send(const void *, size_t, bool) { return Baseband::Result::NotReady; }
};
template class Baseband::GLINK::Channel<BasebandCompileTX>;
template class Baseband::GLINK::Link<Baseband::GLINK::MemoryRing>;
template class Baseband::QRTR::Router<Baseband::GLINK::Link<Baseband::GLINK::MemoryRing>>;
template class Baseband::TFTP::Server<Baseband::GLINK::Link<Baseband::GLINK::MemoryRing>>;
template class Baseband::AXI::Controller<Baseband::HaltIO>;
// Declaration-only stop-owner witness checks the generic sender in ARM64.
// No permissive stop proof or success-returning adapter is linked into KEXTs.
struct BasebandCompileStopOwner {
    Baseband::Result confirmedStopped(Baseband::SSR::Edge, uint64_t);
};
Baseband::Result basebandKernelCleanup(
    Baseband::SSR::Client<Baseband::GLINK::Link<Baseband::GLINK::MemoryRing>> &client,
    BasebandCompileStopOwner &owner, uint64_t epoch, uint64_t stoppedEpoch, uint64_t now) {
    return client.cleanup(epoch, Baseband::SSR::Edge::ADSP, stoppedEpoch, owner, now);
}
// Declaration-only Board forces ARM64 instantiation of the complete lifecycle.
// No success-returning stand-in is linked into the native driver.
struct BasebandCompileBoard {
    Baseband::Result admit(const Baseband::BootLayout &);
    Baseband::Result acquire();
    Baseband::Result assign(const Baseband::Region &, Baseband::Ownership, Baseband::Ownership);
    bool copy(uint64_t, const uint8_t *, size_t);
    bool zero(uint64_t, size_t);
    bool publish(uint64_t, size_t);
    Baseband::Result deassertReset();
    Baseband::Result stageMetadata(const Baseband::FirmwareImage &, const Baseband::Region &);
    Baseband::Result waitReady(uint64_t, unsigned);
    Baseband::Result requestStop(uint64_t, unsigned);
    Baseband::Result quiesceAndReset();
    Baseband::Result release();
};
template class Baseband::Boot<BasebandCompileBoard, Baseband::RegisterIO>;
Baseband::Result basebandKernelParse(const void *data, size_t bytes) {
    // FirmwareImage lives on the heap, not the small XNU kernel stack.
    auto *image = new Baseband::FirmwareImage;
    if (!image) return Baseband::Result::NoMemory;
    const auto status = image->parse(data, bytes);
    delete image; return status;
}
