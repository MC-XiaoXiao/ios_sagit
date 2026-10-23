// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>

namespace StandardUSB {
struct __attribute__((packed)) DeviceRequest {
    uint8_t bmRequestType,bRequest;
    uint16_t wValue,wIndex,wLength;
};
static_assert(sizeof(DeviceRequest)==8,"USB setup packet");
// USB wire descriptors. The Family descriptor collection itself is naturally
// aligned; only these on-wire records are packed.
struct __attribute__((packed)) HubDescriptor {
    uint8_t bLength,bDescriptorType,bNumberPorts;
    uint16_t wHubCharacteristics;
    uint8_t bPowerOnToPowerGood,bHubControllerCurrent;
    uint8_t deviceRemovable[2],reserved[2]; // variable USB2 bitmaps, not read by HCD
};
struct __attribute__((packed)) SuperSpeedHubDescriptor {
    uint8_t bLength,bDescriptorType,bNumberPorts;
    uint16_t wHubCharacteristics;
    uint8_t bPowerOnToPowerGood,bHubControllerCurrent,bHubDecodeLatency;
    uint16_t wHubDelay,deviceRemovable;
};
static_assert(sizeof(HubDescriptor)==11,"Native USB2 hub descriptor view");
static_assert(sizeof(SuperSpeedHubDescriptor)==12,"USB3 hub descriptor");
struct __attribute__((packed)) EndpointDescriptor {
    uint8_t bLength, bDescriptorType, bEndpointAddress, bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
};
struct __attribute__((packed)) SuperSpeedEndpointCompanionDescriptor {
    uint8_t bLength, bDescriptorType, bMaxBurst, bmAttributes;
    uint16_t wBytesPerInterval;
};
struct __attribute__((packed)) SuperSpeedPlusIsochronousEndpointCompanionDescriptor {
    uint8_t bLength, bDescriptorType;
    uint16_t wReserved;
    uint32_t dwBytesPerInterval;
};
static_assert(sizeof(EndpointDescriptor)==7,"USB endpoint descriptor");
static_assert(sizeof(SuperSpeedEndpointCompanionDescriptor)==6,"USB SS endpoint companion");
static_assert(sizeof(SuperSpeedPlusIsochronousEndpointCompanionDescriptor)==8,"USB SSP endpoint companion");
}
