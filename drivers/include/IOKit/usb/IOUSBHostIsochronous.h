// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>
#include <stddef.h>

// Native iOS18 shared-memory records, not USB wire descriptors. The concrete
// request's prepare and legacy conversion use strides 28 and 24 respectively.
// int32_t is IOReturn and uint64_t is AbsoluteTime on the pinned arm64 ABI.
struct IOUSBHostIsochronousFrame {
    int32_t status;
    uint32_t requestCount,completeCount,reserved;
    uint64_t timeStamp;
} __attribute__((packed));
struct IOUSBHostIsochronousTransaction {
    int32_t status;
    uint32_t requestCount,offset,completeCount;
    uint64_t timeStamp;
    uint32_t options;
} __attribute__((packed));
enum tIsochronousTransactionOptions : uint32_t {
    kIsochronousTransactionOptionsNone=0,
    kIsochronousTransactionOptionsWrap=1u<<0
};
static_assert(sizeof(IOUSBHostIsochronousFrame)==24,"Native legacy ISO frame stride");
static_assert(sizeof(IOUSBHostIsochronousTransaction)==28,"Native ISO transaction stride");
static_assert(offsetof(IOUSBHostIsochronousFrame,completeCount)==8 &&
    offsetof(IOUSBHostIsochronousFrame,timeStamp)==16,"Native legacy result offsets");
static_assert(offsetof(IOUSBHostIsochronousTransaction,requestCount)==4 &&
    offsetof(IOUSBHostIsochronousTransaction,offset)==8 &&
    offsetof(IOUSBHostIsochronousTransaction,completeCount)==12 &&
    offsetof(IOUSBHostIsochronousTransaction,timeStamp)==16 &&
    offsetof(IOUSBHostIsochronousTransaction,options)==24,"Native ISO transaction fields");
