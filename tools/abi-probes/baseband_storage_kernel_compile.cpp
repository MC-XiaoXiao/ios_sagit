// SPDX-License-Identifier: GPL-2.0-only
#include <IOKit/storage/IOMedia.h>
static_assert(sizeof(IOStorage)==144, "iPad iOS 18 IOStorage ABI");
static_assert(sizeof(IOMedia)==208, "iPad iOS 18 IOMedia ABI");
// Emit the real inherited declaration layout used by the native storage
// client, then compare its used slots with the independently inspected image.
class BasebandMediaABI : public IOMedia { OSDeclareDefaultStructors(BasebandMediaABI); };
OSDefineMetaClassAndStructors(BasebandMediaABI,IOMedia);
