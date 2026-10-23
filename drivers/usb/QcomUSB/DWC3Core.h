// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "USBProtocol.h"
#include "DWC3Registers.h"
#include "DWC3Endpoint.h"

namespace DWC3 {
// Callers serialize this object on the IOKit work loop. IO supplies ordered
// read/write, bounded microsecond delay, and a DMA read barrier. Only the
// hardware core lives here; Apple USB objects belong to the Family adapters.
template<class IO> class Core {
    IO &io;
    unsigned interrupts = 0, endpoints = 0, addressBits = 0;
    uint32_t revision = 0, eventBytes = 0, eventOffset = 0, hostOperational = 0;
    USB::Role port = USB::Role::None;
    bool probed = false, quiesced = false, eventsInstalled = false;
    uint32_t ending = 0;
    uint32_t activeEndpoints = 0, allocatedEndpoints = 0;
    uint8_t activeTypes[32]{};
    bool configurationReady = false;
    bool isoStarting = false;

    USB::Result poll(uint32_t reg, uint32_t mask, uint32_t expected, unsigned us) {
        for (unsigned i = 0; i <= us; ++i) {
            const uint32_t value = io.read(reg);
            if (value == UINT32_MAX) return USB::Result::HardwareError;
            if ((value & mask) == expected) return USB::Result::Success;
            if (i != us) io.delay(1);
        }
        return USB::Result::Timeout;
    }
    bool isDevice() const { return probed && port == USB::Role::Device; }
public:
    explicit Core(IO &access) : io(access) {}
    bool stopped() const { return quiesced; }
    unsigned endpointCount() const { return endpoints; }
    unsigned dmaBits() const { return addressBits; }
    uint32_t version() const { return revision; }
    USB::Role hardwareRole() const { return port; }
    USB::Result verifyStopped() {
        if (!probed || !quiesced) return USB::Result::NotReady;
        const auto ctl=io.read(GCTL);
        if (ctl==UINT32_MAX) return USB::Result::HardwareError;
        if ((ctl&PortMask)!=(port==USB::Role::Host ? PortHost:PortDevice)) return USB::Result::HardwareError;
        if (port==USB::Role::Host) {
            const auto cap=io.read(0),command=io.read(hostOperational),status=io.read(hostOperational+4);
            if (cap==UINT32_MAX || command==UINT32_MAX || status==UINT32_MAX || (cap&255)!=hostOperational)
                return USB::Result::HardwareError;
            return !(command&1) && (status&1) ? USB::Result::Success:USB::Result::Busy;
        }
        const auto command=io.read(DCTL),status=io.read(DSTS);
        if (command==UINT32_MAX || status==UINT32_MAX) return USB::Result::HardwareError;
        return !(command&Run) && (status&Halted) ? USB::Result::Success:USB::Result::Busy;
    }
    bool endpointStopping(unsigned ep) const { return ep < 32 && (ending & (1u << ep)); }
    bool endpointActive(unsigned ep) const { return ep < 32 && (activeEndpoints & (1u << ep)); }

    USB::Result probe(uint64_t length, unsigned platformAddressBits) {
        if (probed) return USB::Result::Busy;
        if (length < Aperture || platformAddressBits < 32 || platformAddressBits > 40)
            return USB::Result::Invalid;
        uint32_t id = io.read(GSNPSID), p0 = io.read(GHWPARAMS0);
        uint32_t p1 = io.read(GHWPARAMS1), p3 = io.read(GHWPARAMS3);
        if (id == UINT32_MAX || p0 == UINT32_MAX || p1 == UINT32_MAX || p3 == UINT32_MAX)
            return USB::Result::HardwareError;
        // Reject DWC2 and the later DWC_usb31/32 programming models.
        if ((id >> 16) != 0x5533 || (id & 0xffff) < 0x194a ||
            (id & 0xffff) > 0x330a || (p0 & 3) != 2 || ((p3 >> 2) & 3) != 1)
            return USB::Result::Unsupported;
        unsigned count = (p1 >> 15) & 63, eps = (p3 >> 12) & 63;
        unsigned bits = p0 >> 24, in = (p3 >> 18) & 31;
        if (!count || count > 32 || eps < 2 || eps > 32 || !in || in >= eps ||
            bits < 32 || bits > 64) return USB::Result::Unsupported;
        uint32_t ctl = io.read(GCTL);
        switch (ctl & PortMask) {
        case PortHost: port = USB::Role::Host; break;
        case PortDevice: port = USB::Role::Device; break;
        default: return USB::Result::Unsupported; // no autonomous HNP/SRP
        }
        revision = id & 0xffff; endpoints = eps; interrupts = count;
        addressBits = bits < platformAddressBits ? bits : platformAddressBits;
        probed = true;
        return USB::Result::Success;
    }

    USB::Result halt() {
        if (!probed) return USB::Result::NotReady;
        quiesced = false;
        USB::Result result;
        if (port == USB::Role::Host) {
            const uint32_t cap = io.read(0);
            hostOperational = cap & 0xff;
            if (cap == UINT32_MAX || hostOperational < 0x20 ||
                hostOperational > 0x80 || (hostOperational & 3))
                return USB::Result::HardwareError;
            uint32_t cmd = io.read(hostOperational);
            if (cmd == UINT32_MAX) return USB::Result::HardwareError;
            // xHCI USBCMD.R/S and INTE; USBSTS.HCH proves DMA has stopped.
            io.write(hostOperational, cmd & ~5u);
            result = poll(hostOperational + 4, 1, 1, 16000);
        } else {
            io.write(DEVTEN, 0);
            uint32_t ctl = io.read(DCTL);
            if (ctl == UINT32_MAX) return USB::Result::HardwareError;
            io.write(DCTL, ctl & ~(Run | (1u << 19)));
            result = poll(DSTS, Halted, Halted, 10000);
        }
        if (result != USB::Result::Success) return result;
        // Pending device event counts are byte counts acknowledged by writing
        // their value, not W1C bitmaps. Mask every implemented interrupter.
        for (unsigned n = 0; n < interrupts; ++n) {
            uint32_t size = io.read(eventSize(n));
            if (size == UINT32_MAX) return USB::Result::HardwareError;
            io.write(eventSize(n), size | EventMask);
        }
        quiesced = true;
        activeEndpoints = ending = 0;
        return USB::Result::Success;
    }

    USB::Result select(USB::Role next) {
        if (!probed || !quiesced) return USB::Result::NotReady;
        if (next != USB::Role::Host && next != USB::Role::Device)
            return USB::Result::Invalid;
        // Drop all old event pointers only after DMA has stopped. No old
        // address may survive into the next controller incarnation.
        for (unsigned n = 0; n < interrupts; ++n) {
            uint32_t count = io.read(eventCount(n));
            if (count == UINT32_MAX) return USB::Result::HardwareError;
            count &= 0xfffc;
            if (count) io.write(eventCount(n), count);
            io.write(eventSize(n), EventMask);
            io.write(eventAddressLo(n), 0); io.write(eventAddressHi(n), 0);
        }
        eventsInstalled = false; eventBytes = eventOffset = 0; ending = 0;
        allocatedEndpoints = activeEndpoints = 0; configurationReady = false;
        uint32_t ctl = io.read(GCTL);
        if (ctl == UINT32_MAX) return USB::Result::HardwareError;
        // Firmware keeps calibrated PHYs and clocks alive. Do not reset the
        // QMP/QUSB2 analog block or invent efuse trim values on a warm handoff.
        ctl &= ~(PortMask | (3u << 4) | (1u << 1));
        ctl |= next == USB::Role::Host ? PortHost : PortDevice;
        uint32_t usb2 = io.read(GUSB2PHYCFG);
        uint32_t pipe = io.read(GUSB3PIPECTL);
        uint32_t uctl = io.read(GUCTL1);
        if (usb2 == UINT32_MAX || pipe == UINT32_MAX || uctl == UINT32_MAX)
            return USB::Result::HardwareError;
        uint32_t deviceControl = 0;
        if (next == USB::Role::Device) {
            deviceControl = io.read(DCTL);
            if (deviceControl == UINT32_MAX) return USB::Result::HardwareError;
            // Prevent a stale device R/S from taking effect immediately when
            // changing PRTCAPDIR, before we can issue the device soft reset.
            io.write(DCTL, deviceControl & ~Run);
        }
        io.write(GCTL, ctl); port = next;
        io.write(GUSB2PHYCFG, usb2 & ~((1u << 6) | (1u << 8)));
        io.write(GUSB3PIPECTL, pipe & ~(1u << 17));
        io.write(GUCTL1, uctl | (1u << 17)); // disable SS park mode
        if (next == USB::Role::Device) {
            io.write(DCTL, (deviceControl & ~Run) | SoftReset);
            const auto result = poll(DCTL, SoftReset, 0, 1000);
            if (result != USB::Result::Success) {
                quiesced = false; // no proof that the failed reset is idle
                return result;
            }
            const auto idle = poll(DSTS, Halted, Halted, 10000);
            if (idle != USB::Result::Success) { quiesced = false; return idle; }
            io.write(DEVTEN, 0); io.write(DALEPENA, 0);
        }
        return USB::Result::Success;
    }

    USB::Result installEvents(uint64_t address, unsigned bytes) {
        if (!isDevice() || !quiesced) return USB::Result::NotReady;
        if (bytes < 256 || bytes > 0xfffc || (bytes & 3) ||
            !USB::dmaRange(address, bytes, addressBits, 64)) return USB::Result::Invalid;
        io.write(eventSize(0), EventMask);
        uint32_t count = io.read(eventCount(0));
        if (count == UINT32_MAX) return USB::Result::HardwareError;
        count &= 0xfffc;
        if (count) io.write(eventCount(0), count);
        io.write(eventAddressLo(0), uint32_t(address));
        io.write(eventAddressHi(0), uint32_t(address >> 32));
        io.write(eventSize(0), EventMask | bytes);
        eventBytes = bytes; eventOffset = 0; eventsInstalled = true;
        return USB::Result::Success;
    }

    USB::Result runDevice(USB::Speed maximum) {
        if (!isDevice() || !quiesced || !eventsInstalled) return USB::Result::NotReady;
        if (maximum != USB::Speed::High && maximum != USB::Speed::Super)
            return USB::Result::Invalid;
        const uint32_t enabled = io.read(DALEPENA);
        uint32_t cfg = io.read(DCFG), ctl = io.read(DCTL);
        if (enabled == UINT32_MAX || cfg == UINT32_MAX || ctl == UINT32_MAX)
            return USB::Result::HardwareError;
        if ((enabled & 3) != 3) return USB::Result::NotReady;
        // DCFG speed values differ from USB::Speed. Disable LPM until the
        // MSM8998 LPM erratum and link power transitions are handled.
        cfg &= ~(7u | (127u << 3) | (1u << 22));
        if (maximum == USB::Speed::Super) cfg |= 4;
        io.write(DCFG, cfg);
        io.write(DEVTEN, (1u << 11) | (1u << 9) | (1u << 6) | (1u << 4) | (1u << 3) | 7u);
        io.write(eventSize(0), eventBytes);
        quiesced = false; // from this write onward DMA owns its descriptors
        // A new incarnation starts with both USB3 low-power policies clear.
        // Only the Native Family's qualified policy may enable them later.
        io.write(DCTL, (ctl & ~USB3LinkEnable) | Run);
        return poll(DSTS, Halted, 0, 10000);
    }

    // Apple Family validates configuration, feature requests and service
    // intervals. Apply its complete current policy, including both enables
    // and SET_SEL PEL values. No control request is parsed in this backend.
    USB::Result setUSB3LinkPolicy(bool u1, bool u2, unsigned u1PEL, unsigned u2PEL) {
        if (!isDevice() || quiesced) return USB::Result::NotReady;
        if (u1PEL>255 || u2PEL>65535) return USB::Result::Invalid;
        const auto status=io.read(DSTS),ctl=io.read(DCTL),command=io.read(DGCMD);
        if (status==UINT32_MAX || ctl==UINT32_MAX || command==UINT32_MAX)
            return USB::Result::HardwareError;
        if (!(ctl&Run)) return USB::Result::NotReady;
        if ((u1 || u2) && (status&7)!=4) return USB::Result::Unsupported;
        if (command&CommandActive) return USB::Result::Busy;
        // Disable transitions before updating their common periodic budget.
        // An unknown command completion leaves both policies disabled; the
        // caller must close admission and recover the controller.
        const auto disabled=ctl&~USB3LinkEnable;
        io.write(DCTL,disabled);
        const auto idle=io.read(DCTL);
        if (idle==UINT32_MAX || (idle&(Run|USB3LinkEnable))!=Run) return USB::Result::HardwareError;
        unsigned parameter=u1 ? u1PEL:u2 ? u2PEL:0;
        if (parameter>125) parameter=0; // DWC3 periodic-parameter encoding
        io.write(DGCMDPAR,parameter);
        io.write(DGCMD,SetPeriodicParameters|CommandActive);
        const auto result=poll(DGCMD,CommandActive,0,500);
        if (result!=USB::Result::Success) return result;
        const auto completed=io.read(DGCMD);
        if (completed==UINT32_MAX || (completed&(15u<<12))) return USB::Result::HardwareError;
        // Neither a role/speed change nor loss of R/S may revive an old
        // policy after the generic command's completion.
        const auto current=io.read(DCTL),state=io.read(DSTS);
        if (current==UINT32_MAX || state==UINT32_MAX) return USB::Result::HardwareError;
        if (!(current&Run) || ((u1 || u2) && (state&7)!=4)) return USB::Result::NotReady;
        const auto enable=(u1 ? AcceptU1|InitiateU1:0u)|(u2 ? AcceptU2|InitiateU2:0u);
        io.write(DCTL,(current&~USB3LinkEnable)|enable);
        const auto actual=io.read(DCTL);
        return actual!=UINT32_MAX && (actual&(Run|USB3LinkEnable))==(Run|enable) ?
            USB::Result::Success:USB::Result::HardwareError;
    }

    USB::Result setAddress(unsigned address) {
        if (!isDevice()) return USB::Result::NotReady;
        if (address > 127) return USB::Result::Invalid;
        const uint32_t cfg = io.read(DCFG);
        if (cfg == UINT32_MAX) return USB::Result::HardwareError;
        io.write(DCFG, (cfg & ~(127u << 3)) | address << 3);
        return USB::Result::Success;
    }

    // The Family has already checked configuration and remote-wake policy.
    // This USB2 operation proves ON/U0 before returning; USB3 function wake
    // also needs a Device Notification command and is a separate operation.
    USB::Result wakeUSB2() {
        if (!isDevice() || quiesced) return USB::Result::NotReady;
        const uint32_t status = io.read(DSTS), ctl = io.read(DCTL);
        if (status == UINT32_MAX || ctl == UINT32_MAX) return USB::Result::HardwareError;
        const unsigned speed = status & 7, link = (status >> 18) & 15;
        if (speed == 4) return USB::Result::Unsupported;
        if (speed != 0 && speed != 1 && speed != 3) return USB::Result::Invalid;
        if (!(ctl & Run)) return USB::Result::NotReady;
        if (!link) return USB::Result::Success;
        if (link != 1 && link != 2 && link != 3 && link != 5 && link != 15) return USB::Result::NotReady;
        io.write(DCTL, (ctl & ~(15u << 5)) | (8u << 5));
        return poll(DSTS, 15u << 18, 0, 10000);
    }

    // The Native Family supplies the selected, wake-enabled function's
    // interface number. A USB3 function can be suspended while the link is
    // already in U0; no controller-wide suspend flag substitutes for it.
    USB::Result wakeUSB3(unsigned interfaceNumber) {
        if (!isDevice() || quiesced) return USB::Result::NotReady;
        if (interfaceNumber > 255) return USB::Result::Invalid;
        uint32_t status = io.read(DSTS), ctl = io.read(DCTL), command = io.read(DGCMD);
        if (status == UINT32_MAX || ctl == UINT32_MAX || command == UINT32_MAX)
            return USB::Result::HardwareError;
        if ((status & 7) != 4) return USB::Result::Unsupported;
        if (!(ctl & Run)) return USB::Result::NotReady;
        if (command & CommandActive) return USB::Result::Busy;
        const unsigned link = (status >> 18) & 15;
        if (link > 3) return USB::Result::NotReady;
        if (link == 3) {
            io.write(DCTL, (ctl & ~(15u << 5)) | (8u << 5));
            const auto result = poll(DSTS, 15u << 18, 0, 10000);
            if (result != USB::Result::Success) return result;
            status = io.read(DSTS); ctl = io.read(DCTL);
            if (status == UINT32_MAX || ctl == UINT32_MAX) return USB::Result::HardwareError;
            if ((status & 7) != 4 || !(ctl & Run)) return USB::Result::NotReady;
        }
        // U1/U2 automatically exit on this notification. U3 must first reach
        // U0. Never overwrite an outstanding generic command or its argument.
        io.write(DGCMDPAR, FunctionWake | interfaceNumber << 4);
        io.write(DGCMD, DeviceNotification | CommandActive);
        const auto result = poll(DGCMD, CommandActive, 0, 500);
        if (result != USB::Result::Success) return result;
        command = io.read(DGCMD);
        if (command == UINT32_MAX) return USB::Result::HardwareError;
        return command & (15u << 12) ? USB::Result::HardwareError : USB::Result::Success;
    }

    USB::Result beginEndpointConfiguration(unsigned firstResource = 0) {
        if (!isDevice()) return USB::Result::NotReady;
        if (firstResource != 0 && firstResource != 2) return USB::Result::Invalid;
        const uint32_t mask = firstResource == 0 ? UINT32_MAX : ~3u;
        if ((activeEndpoints | ending) & mask) return USB::Result::Busy;
        if (!firstResource && !quiesced) return USB::Result::NotReady;
        uint32_t enabled = io.read(DALEPENA);
        if (enabled == UINT32_MAX) return USB::Result::HardwareError;
        io.write(DALEPENA, enabled & ~mask);
        uint32_t reply = 0;
        auto result = command(0, Command::StartConfiguration, 0, 0, 0, firstResource << 16, reply);
        if (result != USB::Result::Success) { configurationReady = false; return result; }
        allocatedEndpoints &= ~mask;
        configurationReady = true;
        return USB::Result::Success;
    }

    USB::Result configureEndpoint(const EndpointConfiguration &config, bool modify = false) {
        if (!isDevice() || !configurationReady) return USB::Result::NotReady;
        const unsigned ep = config.physicalEndpoint;
        if (!config.valid() || ep >= endpoints) return USB::Result::Invalid;
        // CONNECT_DONE can downgrade SuperSpeed to HS/FS while the initial
        // SETUP TD is armed. The programming model requires a Modify command
        // for EP0's negotiated MPS without disconnecting or cancelling SETUP.
        // DATA/STATUS and untyped raw START commands do not get this exception.
        const bool setupModify = modify && ep == 0 && config.type == EndpointType::Control &&
            activeTypes[0] == unsigned(TRBType::Setup);
        if ((endpointActive(ep) && !setupModify) || endpointStopping(ep)) return USB::Result::Busy;
        const uint32_t bit = 1u << ep;
        if (modify && !(allocatedEndpoints & bit)) return USB::Result::NotReady;
        uint32_t reply = 0;
        auto result = command(ep, Command::Configure, config.parameter0(modify),
            config.parameter1(), 0, 0, reply);
        if (result != USB::Result::Success) return result;
        if (!(allocatedEndpoints & bit)) {
            result = command(ep, Command::Resource, 1, 0, 0, 0, reply);
            if (result != USB::Result::Success) return result;
            allocatedEndpoints |= bit;
        }
        const uint32_t enabled = io.read(DALEPENA);
        if (enabled == UINT32_MAX) return USB::Result::HardwareError;
        io.write(DALEPENA, enabled | bit);
        return USB::Result::Success;
    }

    USB::Result disableEndpoint(unsigned ep) {
        if (!isDevice()) return USB::Result::NotReady;
        if (ep >= endpoints) return USB::Result::Invalid;
        if (endpointActive(ep) || endpointStopping(ep)) return USB::Result::Busy;
        uint32_t enabled = io.read(DALEPENA);
        if (enabled == UINT32_MAX) return USB::Result::HardwareError;
        io.write(DALEPENA, enabled & ~(1u << ep));
        return USB::Result::Success;
    }

    USB::Result startTransfer(unsigned ep, uint64_t address, TRBType type, uint32_t &reply) {
        if (ep >= endpoints || unsigned(type) < 1 || unsigned(type) > 5 ||
            !USB::dmaRange(address, sizeof(TRB), addressBits, 16)) return USB::Result::Invalid;
        if (endpointActive(ep) || endpointStopping(ep)) return USB::Result::Busy;
        auto result = command(ep, Command::Start, uint32_t(address >> 32), uint32_t(address), 0, 0, reply);
        if (result == USB::Result::Success) activeTypes[ep] = unsigned(type);
        return result;
    }
    USB::Result currentFrame(uint32_t &frame) {
        if (!isDevice()) return USB::Result::NotReady;
        const auto status=io.read(DSTS);
        if (status==UINT32_MAX) return USB::Result::HardwareError;
        frame=(status>>3)&0x3fff;return USB::Result::Success;
    }
    USB::Result startIsochronous(unsigned ep,uint64_t address,uint32_t frame,uint32_t &reply) {
        if (quiesced || !eventsInstalled) return USB::Result::NotReady;
        if (ep<2 || ep>=endpoints || frame>0xffff ||
            !USB::dmaRange(address,sizeof(TRB),addressBits,16)) return USB::Result::Invalid;
        if (endpointActive(ep) || endpointStopping(ep)) return USB::Result::Busy;
        // probe() accepts DWC_usb3 only. The DWC_usb31 1.70a HS-IN frame
        // erratum has a different IP signature and is not silently enabled.
        isoStarting=true;
        const auto result=command(ep,Command::Start,uint32_t(address>>32),uint32_t(address),0,frame<<16,reply);
        isoStarting=false;
        if (result==USB::Result::Success || result==USB::Result::BusExpired) activeTypes[ep]=unsigned(TRBType::IsoFirst);
        return result;
    }

    USB::Result command(unsigned ep, Command command, uint32_t p0, uint32_t p1,
                        uint32_t p2, uint32_t options, uint32_t &reply) {
        if (!isDevice()) return USB::Result::NotReady;
        unsigned code = unsigned(command);
        if (ep >= endpoints || code < 1 || code > 9 || code == 3 ||
            (options & ~(((command==Command::Start && isoStarting ? 0xffffu : 127u) << 16) | CommandForce | CommandInterrupt)) ||
            (command == Command::End && (!(options & CommandInterrupt) ||
                (ep != 0 && !(options & (127u << 16))))))
            return USB::Result::Invalid;
        if (endpointStopping(ep)) return USB::Result::Busy;
        if (command == Command::Start && endpointActive(ep)) return USB::Result::Busy;
        uint32_t base = endpoint(ep), value = io.read(base + 12);
        if (value == UINT32_MAX) return USB::Result::HardwareError;
        if (value & CommandActive) return USB::Result::Busy;
        // Hardware requires PAR2, PAR1, PAR0 before CMDACT.
        if (command!=Command::Update) {
            io.write(base, p2); io.write(base + 4, p1); io.write(base + 8, p0);
        }
        if (command == Command::End) ending |= 1u << ep;
        if (command == Command::Start) { activeEndpoints |= 1u << ep; activeTypes[ep] = 0; }
        io.write(base + 12, code | options | CommandActive);
        auto result = poll(base + 12, CommandActive, 0, 1000);
        if (result != USB::Result::Success) return result;
        reply = io.read(base + 12);
        if ((reply >> 12) & 15) return command==Command::Start && isoStarting && ((reply>>12)&15)==2 ?
            USB::Result::BusExpired : USB::Result::HardwareError;
        // On older DWC3 revisions CMDACT clearing acknowledges EndTransfer
        // acceptance, not completion of DMA. Keep its buffers mapped until
        // EPCmdCmplt is consumed, or halt() proves the entire device idle.
        if (command == Command::End) return USB::Result::Pending;
        return USB::Result::Success;
    }

    // Callback consumes a copied event on the work loop. It must not free or
    // replace this ring, recursively dispatch, or change the controller role.
    // Acknowledgement follows processing, so the producer cannot reuse unread
    // entries. Invalid counts fail without acknowledging fabricated bytes.
    template<class Handler> USB::Result drainEvents(const volatile uint32_t *memory, Handler &handler) {
        if (!isDevice() || !eventsInstalled || !memory) return USB::Result::NotReady;
        const uint32_t raw = io.read(eventCount(0));
        const uint32_t count = raw & 0xffff;
        if (raw == UINT32_MAX || (count & 3) || count > eventBytes)
            return USB::Result::HardwareError;
        if (!count) return USB::Result::Success;
        io.dmaReadBarrier();
        // Validate the immutable completed-event snapshot before delivering
        // any callbacks. Otherwise a malformed later event could cause
        // already-delivered completions to be replayed on the next drain.
        uint32_t offset = eventOffset;
        for (uint32_t n = 0; n < count; n += 4) {
            Event event{memory[offset / 4]};
            if (!event.device() && event.endpointNumber() >= endpoints)
                return USB::Result::HardwareError;
            offset += 4;
            if (offset == eventBytes) offset = 0;
        }
        for (uint32_t n = 0; n < count; n += 4) {
            Event event{memory[eventOffset / 4]};
            eventOffset += 4;
            if (eventOffset == eventBytes) eventOffset = 0;
            if (!event.device() && event.type() == 7 && !event.status() &&
                ((event.parameters() >> 8) & 15) == unsigned(Command::End)) {
                ending &= ~(1u << event.endpointNumber());
                activeEndpoints &= ~(1u << event.endpointNumber());
            }
            if (!event.device() && event.type() == 1 && !endpointStopping(event.endpointNumber()))
                activeEndpoints &= ~(1u << event.endpointNumber());
            handler(event);
        }
        io.write(eventCount(0), count);
        return USB::Result::Success;
    }
};

// Only immediately after a proven core reset/halt, before installing DMA.
// A USB2-only route needs UTMI as PIPE clock even though the DWC3 IP contains
// an SS interface. The gated switch also restores the real PIPE for SS routes.
template<class IO> USB::Result setPipeClock(IO &scratch, bool useUTMI) {
    const auto reg=QScratch::General;
    auto value=scratch.read(reg);
    if (value==UINT32_MAX) return USB::Result::HardwareError;
    scratch.write(reg,value|QScratch::PipeClockDisable);
    if (!(scratch.read(reg)&QScratch::PipeClockDisable)) return USB::Result::HardwareError;
    scratch.delay(100);
    value=scratch.read(reg);
    if (value==UINT32_MAX) return USB::Result::HardwareError;
    const uint32_t select=QScratch::PipeUTMISelect|QScratch::PipePhyStatusSoftware;
    const uint32_t target=useUTMI ? select:0;
    scratch.write(reg,(value&~select)|target);
    value=scratch.read(reg);
    if (value==UINT32_MAX || (value&select)!=target) return USB::Result::HardwareError;
    scratch.delay(100);
    scratch.write(reg,value&~QScratch::PipeClockDisable);
    value=scratch.read(reg);
    return value!=UINT32_MAX && (value&(select|QScratch::PipeClockDisable))==target ?
        USB::Result::Success : USB::Result::HardwareError;
}
template<class IO> USB::Result setVbusOverride(IO &scratch, bool connected) {
    const uint32_t hs = QScratch::VbusValid | QScratch::SessionSelect;
    const uint32_t usb2 = scratch.read(QScratch::HSControl), usb3 = scratch.read(QScratch::SSControl);
    if (usb2 == UINT32_MAX || usb3 == UINT32_MAX) return USB::Result::HardwareError;
    scratch.write(QScratch::HSControl, connected ? usb2 | hs : usb2 & ~hs);
    scratch.write(QScratch::SSControl, connected ? usb3 | QScratch::PowerPresent : usb3 & ~QScratch::PowerPresent);
    return USB::Result::Success;
}
}
