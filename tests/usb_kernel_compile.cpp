// SPDX-License-Identifier: BSD-2-Clause
// Compile every hardware operation using the real XNU SDK, arm64 compiler
// and Apple Kext code generation. This is a compile check, not a loadable Kext.
#include <IOKit/IOService.h>
#include "USBMMIO.h"
#include "DWC3Core.h"
#include "DWC3Transfer.h"
#include "DWC3Control.h"
#include "XHCICore.h"
#include "XHCIContexts.h"
#include "XHCICommands.h"
#include "XHCITransfers.h"
#include "XHCIEvents.h"
#include "XHCISlots.h"
#include "XHCIPipes.h"
#include "XHCIRequests.h"
#include "USBRole.h"
#include "USBIODMA.h"
#include "USBDeviceRequests.h"
#include "USBHostDMA.h"
#include "USBPhy.h"
#include "USBPhyResources.h"
#include "USBDirectDMA.h"
#include "DWC3Isochronous.h"
#include "USBDeviceIsochronous.h"
#include "QcomUSB.h"
#include "../drivers/bus/QcomSMMU/IORT.h"

SMMU::IORTResult USBIORTCompile(const void *data,size_t bytes,SMMU::IORTTopology &topology) {
    return SMMU::parseIORT(data,bytes,"\\_SB_.USB0",0,topology);
}

static_assert(__is_same(decltype(&QcomUSBController::runHardwareAction),
    IOReturn (QcomUSBController::*)(IOService *, UInt64, QcomUSBController::HardwareAction, void *)),
    "native Host uses the role-nub gate bridge without replacing its Family loop");
IOReturn USBHardwareGateCompile(QcomUSBController *nub, IOService *client, UInt64 epoch,
    QcomUSBController::HardwareAction action, void *context) {
    return nub->runHardwareAction(client, epoch, action, context);
}

template class DWC3::Core<USB::MMIO>;
template class DWC3::Transfer<DWC3::Core<USB::MMIO>, USB::MMIO>;
template class DWC3::Control<DWC3::Core<USB::MMIO>, USB::MMIO>;
template class XHCI::Core<USB::MMIO>;
template class XHCI::Port<XHCI::Core<USB::MMIO>>;
template USB::Result XHCI::PortProtocols::initialize(USB::MMIO &,unsigned);
template USB::Result XHCI::DeviceContext::slot(USB::MMIO &,XHCI::SlotSnapshot &) const;
template USB::Result XHCI::DeviceContext::endpoint(USB::MMIO &,unsigned,unsigned &,uint64_t &) const;
struct XHCICommandCompileBackend {
    XHCI::ProducerRing<USB::MMIO> &ring();
    bool admitted(uint64_t);
    USB::Result doorbell();
    void retain(uintptr_t);
    USB::Result isolation(uint64_t);
};
template class XHCI::CommandQueue<XHCICommandCompileBackend>;
template class XHCI::SlotMemory<USB::IOMemoryAPI,USB::DMAOrdering>;
template class XHCI::PipeMemory<USB::IOMemoryAPI,USB::DMAOrdering>;
template USB::Result XHCI::PipeMemory<USB::IOMemoryAPI,USB::DMAOrdering>::configured(
    USB::MMIO &,const XHCI::CommandRecord &,const XHCI::InputContext &,const XHCI::DeviceContext &,
    const XHCI::EndpointConfiguration &,bool);
template USB::Result XHCI::PipeMemory<USB::IOMemoryAPI,USB::DMAOrdering>::removed(
    USB::MMIO &,const XHCI::CommandRecord &,const XHCI::InputContext &,const XHCI::DeviceContext &,uint64_t);
template USB::Result XHCI::SlotMemory<USB::IOMemoryAPI,USB::DMAOrdering>::evaluatedControl(
    USB::MMIO &,const XHCI::CommandRecord &,unsigned);
template USB::Result XHCI::SlotMemory<USB::IOMemoryAPI,USB::DMAOrdering>::addressed(
    USB::MMIO &,const XHCI::CommandRecord &,unsigned,XHCI::SlotSnapshot &) const;
struct XHCIEventCompileSink {
    void command(const XHCI::CommandRecord &);
    USB::Result port(unsigned);
    USB::Result transfer(const XHCI::TRB &,uint64_t);
    USB::Result frameRollover(uint32_t);
    USB::Result prepareEventAcknowledgement();
};
template USB::Result XHCI::drainEvents(XHCI::Core<USB::MMIO> &,XHCI::EventRing<USB::MMIO> &,
    XHCI::CommandQueue<XHCICommandCompileBackend> &,XHCIEventCompileSink &,uint64_t,unsigned,XHCI::EventBatch &);
struct XHCITransferCompileBackend {
    XHCI::ProducerRing<USB::MMIO> &ring();
    bool admitted(uint64_t);
    USB::Result doorbell(unsigned,unsigned);
    void retain(uintptr_t);
    USB::Result isolation(uint64_t);
};
template class XHCI::TransferDescriptor<64>;
template class XHCI::TransferQueue<XHCITransferCompileBackend,XHCI::CommandQueue<XHCICommandCompileBackend>>;
using USBHostCompileQueue=XHCI::TransferQueue<XHCITransferCompileBackend,XHCI::CommandQueue<XHCICommandCompileBackend>>;
template class XHCI::RequestMemory<USB::IOMemoryAPI>;
template USB::Result XHCI::RequestMemory<USB::IOMemoryAPI>::submit(USBHostCompileQueue &,
    const XHCI::ProducerRing<USB::MMIO> &,uintptr_t,uint64_t,uint64_t,uint64_t);
template USB::Result XHCI::RequestMemory<USB::IOMemoryAPI>::retire(USBHostCompileQueue &,uintptr_t &);
template USB::Result XHCI::TransferQueue<XHCITransferCompileBackend,XHCI::CommandQueue<XHCICommandCompileBackend>>::submit<64>(
    const XHCI::TransferDescriptor<64> &,uintptr_t,uint64_t,uint64_t,uint64_t &);
template USB::Result XHCI::InputContext::submit(XHCI::CommandQueue<XHCICommandCompileBackend> &,
    XHCI::Type,unsigned,bool,uintptr_t,uint64_t,uint64_t,uint64_t &);
template USB::Result XHCI::InputContext::finish(XHCI::CommandQueue<XHCICommandCompileBackend> &,uintptr_t,uintptr_t &);
USB::Result USBXHCIInputContextCompile(XHCI::InputContext &context,void *memory,unsigned length,
    uint64_t dma,unsigned stride,unsigned bits,const XHCI::SlotConfiguration &slot,
    const XHCI::PortProtocol &protocol,const XHCI::EndpointConfiguration &endpoint,uint64_t ticket) {
    auto result=context.bind(memory,length,dma,stride,bits,32,2);
    if (result==USB::Result::Success) result=context.slot(slot,protocol);
    if (result==USB::Result::Success) result=context.endpoint(endpoint);
    if (result==USB::Result::Success) result=context.drop(2);
    if (result==USB::Result::Success) result=context.freeze(ticket);
    if (result==USB::Result::Success) result=context.retire(ticket);
    if (result==USB::Result::Success) result=context.clear();
    return result;
}
template class XHCI::ProducerRing<USB::MMIO>;
template class XHCI::EventRing<USB::MMIO>;
template class USB::DMARegion<USB::IOMemoryAPI>;
template class USB::DeviceBounce<USB::IOMemoryAPI>;
template class USB::HostBounce<USB::IOMemoryAPI>;
template class USB::DeviceRequests<USB::IOMemoryAPI>;
template class DWC3::IsochronousRing<DWC3::Core<USB::MMIO>,USB::MMIO>;
static_assert(IODMACommand::kUnmapped==3,"audited iOS18 direct DMA mapping type");
struct USBDirectCompileBackend {
    USB::Result readDMAWindow(USB::DirectDMAWindow &);
    USB::Result dmaAccessible();
    USB::Result dmaRoute();
    USB::Result dmaIsolated();
};
template class USB::DirectDMA<USBDirectCompileBackend>;

struct USBPhyCompileBackend {
    USB::Result request(ClockController::Controller, ClockController::Operation, uint32_t, uint64_t &);
    USB::Result clamp(bool);
};
template class USBPHY::Resources<USBPhyCompileBackend>;
template class USBPHY::USB2<USB::MMIO, USBPHY::Resources<USBPhyCompileBackend>>;
template class USBPHY::USB3<USB::MMIO, USBPHY::Resources<USBPhyCompileBackend>>;

struct USBCompileHandler {
    void operator()(DWC3::Event) {}
};
template USB::Result DWC3::Core<USB::MMIO>::drainEvents(
    const volatile uint32_t *, USBCompileHandler &);
template USB::Result DWC3::setVbusOverride(USB::MMIO &, bool);

struct USBCompileBackend {
    USB::Result source(bool);
    bool stop(USB::Role, uint64_t);
    USB::Result safePower(USB::Role, const USB::ConnectorState &);
    USB::Result configure(USB::Role, const USB::ConnectorState &);
    USB::Result start(USB::Role, uint64_t, const USB::ConnectorState &);
};
template class USB::RoleSwitch<USBCompileBackend>;
