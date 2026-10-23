// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IODataQueueClient.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stddef.h>

// Internal iOS transport projection. The SDK exports these C interfaces but
// marks parts of its public IOKitLib header unavailable to iOS applications.
// The unified builder checks every used symbol against that SDK's IOKit.tbd.
extern "C" {
CFMutableDictionaryRef IOServiceMatching(const char *);
kern_return_t IOServiceGetMatchingServices(mach_port_t, CFDictionaryRef, mach_port_t *);
mach_port_t IOIteratorNext(mach_port_t);
kern_return_t IOObjectRetain(mach_port_t);
kern_return_t IOObjectRelease(mach_port_t);
boolean_t IOObjectConformsTo(mach_port_t, const char *);
kern_return_t IORegistryEntryGetRegistryEntryID(mach_port_t, uint64_t *);
CFTypeRef IORegistryEntryCreateCFProperty(mach_port_t, CFStringRef, CFAllocatorRef, uint32_t);
kern_return_t IORegistryEntrySetCFProperties(mach_port_t, CFTypeRef);
kern_return_t IOServiceOpen(mach_port_t, task_t, uint32_t, mach_port_t *);
kern_return_t IOServiceClose(mach_port_t);
kern_return_t IOConnectSetNotificationPort(mach_port_t, uint32_t, mach_port_t, uintptr_t);
kern_return_t IOConnectCallScalarMethod(mach_port_t, uint32_t, const uint64_t *, uint32_t, uint64_t *, uint32_t *);
kern_return_t IOConnectCallMethod(mach_port_t, uint32_t, const uint64_t *, uint32_t,
    const void *, size_t, uint64_t *, uint32_t *, void *, size_t *);
}
