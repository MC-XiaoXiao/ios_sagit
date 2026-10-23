// SPDX-License-Identifier: BSD-2-Clause
#include "QcomWLANDriver.h"
#include <DriverKit/IORPC.h>

namespace {
struct Description {
    OSClassDescription base;
    char queues[1], methods[1], metaMethods[1];
};
const Description description = {
    .base = {
        .descriptionSize = sizeof(Description),
        .name = "QcomWLANDriver",
        .superName = "IOService",
        .methodOptionsSize = 0, .methodOptionsOffset = sizeof(OSClassDescription),
        .metaMethodOptionsSize = 0, .metaMethodOptionsOffset = sizeof(OSClassDescription),
        .queueNamesSize = 1, .queueNamesOffset = offsetof(Description,queues),
        .methodNamesSize = 1, .methodNamesOffset = offsetof(Description,methods),
        .metaMethodNamesSize = 1, .metaMethodNamesOffset = offsetof(Description,metaMethods),
        .flags = kOSClassCanRemote, .resv1 = {},
        .methodOptions = {}, .metaMethodOptions = {}, .dispatchNames = {},
        .methodNames = {}, .metaMethodNames = {},
    }, .queues = "", .methods = "", .metaMethods = ""
};
OSMetaClass *metaClass;
kern_return_t createMeta(OSMetaClass *instance) {
    new(instance) QcomWLANDriverMetaClass;
    return kIOReturnSuccess;
}
const OSClassLoadInformation load = {
    .description = &description.base, .metaPointer = &metaClass,
    .version = 1, .instanceSize = sizeof(QcomWLANDriver), .resv2 = {},
    .New = createMeta, .resv3 = {}
};
}
extern const void * const QcomWLANDriver_Declaration;
const void * const QcomWLANDriver_Declaration
__attribute__((visibility("hidden"),section("__DATA_CONST,__osclassinfo,regular,no_dead_strip"))) = &load;

kern_return_t QcomWLANDriverMetaClass::New(OSObject *instance) {
    new(instance) QcomWLANDriver;
    return kIOReturnSuccess;
}
kern_return_t QcomWLANDriver::Dispatch(IORPC rpc) { return _Dispatch(this,rpc); }
kern_return_t QcomWLANDriver::_Dispatch(QcomWLANDriver *self,IORPC rpc) {
    // The Apple invoke helpers validate descriptor counts and complete wire
    // sizes. Do not decode an untrusted message before the common envelope.
    if (!rpc.message || rpc.sendSize < sizeof(IORPCMessageMach) + sizeof(IORPCMessage) ||
        rpc.message->msgh.msgh_size > rpc.sendSize ||
        rpc.message->msgh.msgh_size < sizeof(IORPCMessageMach) + sizeof(IORPCMessage) ||
        rpc.message->msgh.msgh_id != kIORPCVersion190615) return kIOReturnIPCError;
    const auto *base = reinterpret_cast<const uint8_t *>(rpc.message);
    size_t offset = sizeof(IORPCMessageMach);
    const auto limit = rpc.message->msgh.msgh_size;
    const auto count = rpc.message->msgh_body.msgh_descriptor_count;
    for (unsigned i = 0; i < count; ++i) {
        if (limit - offset < sizeof(mach_msg_port_descriptor_t) + sizeof(IORPCMessage)) return kIOReturnIPCError;
        const auto *descriptor = reinterpret_cast<const mach_msg_port_descriptor_t *>(base + offset);
        const size_t size = descriptor->type == MACH_MSG_PORT_DESCRIPTOR ? sizeof(mach_msg_port_descriptor_t) :
            descriptor->type == MACH_MSG_OOL_DESCRIPTOR ? sizeof(mach_msg_ool_descriptor_t) : 0;
        if (!size || limit - offset < size + sizeof(IORPCMessage)) return kIOReturnIPCError;
        offset += size;
    }
    auto *message = IORPCMessageFromMach(rpc.message,false);
    if (!message) return kIOReturnIPCError;
    switch (message->msgid) {
    case IOService_Start_ID:
        return IOService::Start_Invoke(rpc,self,[](OSMetaClassBase *object, IOService *provider) {
            return static_cast<QcomWLANDriver *>(object)->Start_Impl(provider);
        });
    case IOService_Stop_ID:
        return IOService::Stop_Invoke(rpc,self,[](OSMetaClassBase *object, IOService *provider) {
            return static_cast<QcomWLANDriver *>(object)->Stop_Impl(provider);
        });
    default: return IOService::_Dispatch(self,rpc);
    }
}
