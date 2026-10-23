// SPDX-License-Identifier: GPL-2.0-only
#include "firmware_rpm.h"
#ifdef BRIDGE_RPM_COLD_HANDOFF
#include "firmware_rpm_cold.h"
#endif
extern "C" {
#include "bridge.h"
extern const unsigned char usb_npa_pin_blob[],usb_npa_pin_end[];
#ifdef BRIDGE_RPM_COLD_HANDOFF
extern const unsigned char usb_glink_pin_blob[],usb_glink_pin_end[];
#endif
}
namespace { BridgeFirmware::RPMRequests requests; }
#ifdef BRIDGE_GPU_PROBE
namespace {
const QcomPower::FirmwareRequests *observed = nullptr;
void dumpRPM() {
    if (!observed || observed->version != 2 || observed->bytes != sizeof(*observed) || observed->count > 160) return;
    bridge_puts("[GPU RPM] observation-only count="); bridge_hex(observed->count); bridge_puts("\n");
    for (unsigned i = 0; i < observed->count; ++i) {
        const auto &row = observed->rows[i];
        bridge_puts("[GPU RPM] type="); bridge_hex(row.type);
        bridge_puts(" resource="); bridge_hex(row.resource);
        bridge_puts(" key="); bridge_hex(row.key);
        bridge_puts(" presence="); bridge_hex(row.presence);
        const QcomPower::FirmwareRequestValue *values[] = {
            &row.activeOrNextSubmitted, &row.activeRequested, &row.sleepSubmitted,
            &row.sleepRequested, &row.nextRequested
        };
        for (const auto *value : values) {
            bridge_puts(" "); bridge_hex(value->present); bridge_puts("/"); bridge_hex(value->value);
        }
        bridge_puts("\n");
    }
    bridge_puts("[GPU RPM] observation-only end\n");
}
}
#endif
#ifdef BRIDGE_RPM_COLD_HANDOFF
namespace {
struct RPMRead {
    static uint32_t read(unsigned offset) {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        return *reinterpret_cast<volatile uint32_t *>(uintptr_t(0x778000)+offset);
    }
};
BridgeFirmware::RPMCold<RPMRead> cold;
const void *stopRecord=nullptr;
}
#endif
extern "C" int bridge_usb_rpm_prepare(EFI_BOOT_SERVICES *services,void *tree) {
    uint32_t bytes=0;
    auto *out=dt_property(tree,"chosen",QcomPower::FirmwareRequestsProperty,&bytes);
    const auto pinBytes=uintptr_t(usb_npa_pin_end)-uintptr_t(usb_npa_pin_blob);
    if(pinBytes>0x100000 || !requests.prepare(services,usb_npa_pin_blob,unsigned(pinBytes),out,bytes))return -1;
#ifdef BRIDGE_GPU_PROBE
    observed = static_cast<const QcomPower::FirmwareRequests *>(out);
    dumpRPM();
#endif
#ifdef BRIDGE_RPM_COLD_HANDOFF
    auto *transport=dt_property(tree,"power-controller@778000",BridgeFirmware::RPMColdProperty,&bytes);
    uint32_t stopBytes=0;stopRecord=dt_property(tree,"chosen",USB::FirmwareStopProperty,&stopBytes);
    const auto glinkBytes=uintptr_t(usb_glink_pin_end)-uintptr_t(usb_glink_pin_blob);
    if(stopBytes!=sizeof(USB::FirmwareStop) || glinkBytes>0x100000 ||
        !cold.prepare(services,usb_glink_pin_blob,unsigned(glinkBytes),requests.nextMessageID(),transport,bytes))return -2;
#endif
    return 0;
}
extern "C" int bridge_usb_rpm_finish(void) {
    if(!requests.finish())return -1;
#ifdef BRIDGE_RPM_COLD_HANDOFF
    USB::FirmwareStop stop{};
    const auto *p=static_cast<const uint8_t *>(stopRecord);auto *q=reinterpret_cast<uint8_t *>(&stop);
    for(unsigned i=0;i<sizeof(stop);++i)q[i]=p[i];
    if(!cold.finish(stop))return -2;
#endif
    return 0;
}
