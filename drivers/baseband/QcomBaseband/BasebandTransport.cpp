// SPDX-License-Identifier: GPL-2.0-only
#include "BasebandLink.h"
#include "BasebandQRTR.h"
#include "BasebandQRTRService.h"
#include "BasebandSysmon.h"
#include "BasebandSSR.h"
#include "BasebandTFTP.h"
// Compile the concrete uncached-SMEM transport into the arm64 KEXT. Runtime
// construction is gated by the boot coordinator's real memory/resource lease.
template class Baseband::GLINK::Link<Baseband::GLINK::MemoryRing>;
template class Baseband::QRTR::Router<Baseband::GLINK::Link<Baseband::GLINK::MemoryRing>>;
template class Baseband::QRTR::ServiceHost<Baseband::GLINK::Link<Baseband::GLINK::MemoryRing>>;
template class Baseband::Sysmon::Client<Baseband::GLINK::Link<Baseband::GLINK::MemoryRing>>;
template class Baseband::SSR::Client<Baseband::GLINK::Link<Baseband::GLINK::MemoryRing>>;
template class Baseband::TFTP::Server<Baseband::GLINK::Link<Baseband::GLINK::MemoryRing>>;
