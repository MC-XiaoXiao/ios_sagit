/* iig(DriverKit-286 Mar 29 2024 10:27:09) generated from IOEventLink.iig */

#undef	IIG_IMPLEMENTATION
#define	IIG_IMPLEMENTATION 	IOEventLink.iig

#if KERNEL
#include <libkern/c++/OSString.h>
#else
#include <DriverKit/DriverKit.h>
#endif /* KERNEL */
#include <DriverKit/IOReturn.h>
#include <DriverKit/IORPC.h>
#include <DriverKit/IOEventLink.h>


#if __has_builtin(__builtin_load_member_function_pointer)
#define SimpleMemberFunctionCast(cfnty, self, func) (cfnty)__builtin_load_member_function_pointer(self, func)
#else
#define SimpleMemberFunctionCast(cfnty, self, func) ({ union { typeof(func) memfun; cfnty cfun; } pair; pair.memfun = func; pair.cfun; })
#endif


struct IOEventLink_Create_Msg_Content
{
    IORPCMessage __hdr;
    OSObjectRef  __object;
    OSString * name;
#if !defined(__LP64__)
    uint32_t __namePad;
#endif /* !defined(__LP64__) */
    OSObjectRef  userClient;
};
#pragma pack(4)
struct IOEventLink_Create_Msg
{
    IORPCMessageMach           mach;
    mach_msg_port_descriptor_t __object__descriptor;
    mach_msg_ool_descriptor_t  name__descriptor;
    mach_msg_port_descriptor_t userClient__descriptor;
    IOEventLink_Create_Msg_Content content;
};
#pragma pack()
#define IOEventLink_Create_Msg_ObjRefs (3)

struct IOEventLink_Create_Rpl_Content
{
    IORPCMessage __hdr;
    OSObjectRef  eventLink;
};
#pragma pack(4)
struct IOEventLink_Create_Rpl
{
    IORPCMessageMach           mach;
    mach_msg_port_descriptor_t eventLink__descriptor;
    IOEventLink_Create_Rpl_Content content;
};
#pragma pack()
#define IOEventLink_Create_Rpl_ObjRefs (1)


typedef union
{
    const IORPC rpc;
    struct
    {
        const struct IOEventLink_Create_Msg * message;
        struct IOEventLink_Create_Rpl       * reply;
        uint32_t sendSize;
        uint32_t replySize;
    };
}
IOEventLink_Create_Invocation;
struct IOEventLink_SetEventlinkPort_Msg_Content
{
    IORPCMessage __hdr;
    OSObjectRef  __object;
};
#pragma pack(4)
struct IOEventLink_SetEventlinkPort_Msg
{
    IORPCMessageMach           mach;
    mach_msg_port_descriptor_t __object__descriptor;
    mach_msg_port_descriptor_t port__descriptor;
    IOEventLink_SetEventlinkPort_Msg_Content content;
};
#pragma pack()
#define IOEventLink_SetEventlinkPort_Msg_ObjRefs (1)

struct IOEventLink_SetEventlinkPort_Rpl_Content
{
    IORPCMessage __hdr;
};
#pragma pack(4)
struct IOEventLink_SetEventlinkPort_Rpl
{
    IORPCMessageMach           mach;
    IOEventLink_SetEventlinkPort_Rpl_Content content;
};
#pragma pack()
#define IOEventLink_SetEventlinkPort_Rpl_ObjRefs (0)


typedef union
{
    const IORPC rpc;
    struct
    {
        const struct IOEventLink_SetEventlinkPort_Msg * message;
        struct IOEventLink_SetEventlinkPort_Rpl       * reply;
        uint32_t sendSize;
        uint32_t replySize;
    };
}
IOEventLink_SetEventlinkPort_Invocation;
struct IOEventLink_InvalidateKernel_Msg_Content
{
    IORPCMessage __hdr;
    OSObjectRef  __object;
    OSObjectRef  client;
};
#pragma pack(4)
struct IOEventLink_InvalidateKernel_Msg
{
    IORPCMessageMach           mach;
    mach_msg_port_descriptor_t __object__descriptor;
    mach_msg_port_descriptor_t client__descriptor;
    IOEventLink_InvalidateKernel_Msg_Content content;
};
#pragma pack()
#define IOEventLink_InvalidateKernel_Msg_ObjRefs (2)

struct IOEventLink_InvalidateKernel_Rpl_Content
{
    IORPCMessage __hdr;
};
#pragma pack(4)
struct IOEventLink_InvalidateKernel_Rpl
{
    IORPCMessageMach           mach;
    IOEventLink_InvalidateKernel_Rpl_Content content;
};
#pragma pack()
#define IOEventLink_InvalidateKernel_Rpl_ObjRefs (0)


typedef union
{
    const IORPC rpc;
    struct
    {
        const struct IOEventLink_InvalidateKernel_Msg * message;
        struct IOEventLink_InvalidateKernel_Rpl       * reply;
        uint32_t sendSize;
        uint32_t replySize;
    };
}
IOEventLink_InvalidateKernel_Invocation;
#if !KERNEL
extern OSMetaClass * gOSContainerMetaClass;
extern OSMetaClass * gOSDataMetaClass;
extern OSMetaClass * gOSNumberMetaClass;
extern OSMetaClass * gOSBooleanMetaClass;
extern OSMetaClass * gOSDictionaryMetaClass;
extern OSMetaClass * gOSArrayMetaClass;
extern OSMetaClass * gOSSetMetaClass;
extern OSMetaClass * gOSOrderedSetMetaClass;
extern OSMetaClass * gOSStringMetaClass;
extern OSMetaClass * gIOServiceStateNotificationDispatchSourceMetaClass;
extern OSMetaClass * gIOMemoryMapMetaClass;
extern OSMetaClass * gOSAction_IOUserClient_KernelCompletionMetaClass;
#endif /* !KERNEL */

#if KERNEL
OSDefineMetaClassAndStructors(IOEventLink, OSObject);
#endif /* KERNEL */

#if !KERNEL

#define IOEventLink_QueueNames  ""

#define IOEventLink_MethodNames  ""

#define IOEventLinkMetaClass_MethodNames  ""

struct OSClassDescription_IOEventLink_t
{
    OSClassDescription base;
    uint64_t           methodOptions[2 * 0];
    uint64_t           metaMethodOptions[2 * 0];
    char               queueNames[sizeof(IOEventLink_QueueNames)];
    char               methodNames[sizeof(IOEventLink_MethodNames)];
    char               metaMethodNames[sizeof(IOEventLinkMetaClass_MethodNames)];
};

const struct OSClassDescription_IOEventLink_t
OSClassDescription_IOEventLink =
{
    .base =
    {
        .descriptionSize         = sizeof(OSClassDescription_IOEventLink_t),
        .name                    = "IOEventLink",
        .superName               = "OSObject",
        .methodOptionsSize       = 2 * sizeof(uint64_t) * 0,
        .methodOptionsOffset     = __builtin_offsetof(struct OSClassDescription_IOEventLink_t, methodOptions),
        .metaMethodOptionsSize   = 2 * sizeof(uint64_t) * 0,
        .metaMethodOptionsOffset = __builtin_offsetof(struct OSClassDescription_IOEventLink_t, metaMethodOptions),
        .queueNamesSize       = sizeof(IOEventLink_QueueNames),
        .queueNamesOffset     = __builtin_offsetof(struct OSClassDescription_IOEventLink_t, queueNames),
        .methodNamesSize         = sizeof(IOEventLink_MethodNames),
        .methodNamesOffset       = __builtin_offsetof(struct OSClassDescription_IOEventLink_t, methodNames),
        .metaMethodNamesSize     = sizeof(IOEventLinkMetaClass_MethodNames),
        .metaMethodNamesOffset   = __builtin_offsetof(struct OSClassDescription_IOEventLink_t, metaMethodNames),
        .flags                   = 1*kOSClassCanRemote,
    },
    .methodOptions =
    {
    },
    .metaMethodOptions =
    {
    },
    .queueNames      = IOEventLink_QueueNames,
    .methodNames     = IOEventLink_MethodNames,
    .metaMethodNames = IOEventLinkMetaClass_MethodNames,
};

OSMetaClass * gIOEventLinkMetaClass;

static kern_return_t
IOEventLink_New(OSMetaClass * instance);

const OSClassLoadInformation
IOEventLink_Class = 
{
    .description       = &OSClassDescription_IOEventLink.base,
    .metaPointer       = &gIOEventLinkMetaClass,
    .version           = 1,
    .instanceSize      = sizeof(IOEventLink),

    .New               = &IOEventLink_New,
};

extern const void * const
gIOEventLink_Declaration;
const void * const
gIOEventLink_Declaration
__attribute__((visibility("hidden"),section("__DATA_CONST,__osclassinfo,regular,no_dead_strip"),no_sanitize("address")))
    = &IOEventLink_Class;

static kern_return_t
IOEventLink_New(OSMetaClass * instance)
{
    if (!new(instance) IOEventLinkMetaClass) return (kIOReturnNoMemory);
    return (kIOReturnSuccess);
}

kern_return_t
IOEventLinkMetaClass::New(OSObject * instance)
{
    if (!new(instance) IOEventLink) return (kIOReturnNoMemory);
    return (kIOReturnSuccess);
}

#endif /* !KERNEL */

kern_return_t
IOEventLink::Dispatch(const IORPC rpc)
{
    return _Dispatch(this, rpc);
}

kern_return_t
IOEventLink::_Dispatch(IOEventLink * self, const IORPC rpc)
{
    kern_return_t ret = kIOReturnUnsupported;
    IORPCMessage * msg = IORPCMessageFromMach(rpc.message, false);

    switch (msg->msgid)
    {
        case IOEventLink_SetEventlinkPort_ID:
        {
            ret = IOEventLink::SetEventlinkPort_Invoke(rpc, self, SimpleMemberFunctionCast(IOEventLink::SetEventlinkPort_Handler, *self, &IOEventLink::SetEventlinkPort_Impl));
            break;
        }
#if KERNEL
        case IOEventLink_InvalidateKernel_ID:
        {
            ret = IOEventLink::InvalidateKernel_Invoke(rpc, self, SimpleMemberFunctionCast(IOEventLink::InvalidateKernel_Handler, *self, &IOEventLink::InvalidateKernel_Impl));
            break;
        }
#endif /* !KERNEL */

        default:
            ret = OSObject::_Dispatch(self, rpc);
            break;
    }

    return (ret);
}

#if KERNEL
kern_return_t
IOEventLink::MetaClass::Dispatch(const IORPC rpc)
{
#else /* KERNEL */
kern_return_t
IOEventLinkMetaClass::Dispatch(const IORPC rpc)
{
#endif /* !KERNEL */

    kern_return_t ret = kIOReturnUnsupported;
    IORPCMessage * msg = IORPCMessageFromMach(rpc.message, false);

    switch (msg->msgid)
    {
#if KERNEL
        case IOEventLink_Create_ID:
            ret = IOEventLink::Create_Invoke(rpc, &IOEventLink::Create_Impl);
            break;
#endif /* !KERNEL */

        default:
            ret = OSMetaClassBase::Dispatch(rpc);
            break;
    }

    return (ret);
}

kern_return_t
IOEventLink::Create_Call(
        OSString * name,
        IOUserClient * userClient,
        IOEventLink ** eventLink)
{
    kern_return_t ret;
    union
    {
        IOEventLink_Create_Msg msg;
        struct
        {
            IOEventLink_Create_Rpl rpl;
            mach_msg_max_trailer_t trailer;
        } rpl;
    } buf;
    struct IOEventLink_Create_Msg * msg = &buf.msg;
    struct IOEventLink_Create_Rpl * rpl = &buf.rpl.rpl;

    memset(msg, 0, sizeof(struct IOEventLink_Create_Msg));
    msg->mach.msgh.msgh_id   = kIORPCVersion190615;
    msg->mach.msgh.msgh_size = sizeof(*msg);
    msg->content.__hdr.flags = 0*kIORPCMessageOneway
                             | 0*kIORPCMessageSimpleReply
                             | 0*kIORPCMessageLocalHost
                             | 0*kIORPCMessageOnqueue;
    msg->content.__hdr.msgid = IOEventLink_Create_ID;
    msg->content.__object = (OSObjectRef) OSTypeID(IOEventLink);
    msg->content.__hdr.objectRefs = IOEventLink_Create_Msg_ObjRefs;
    msg->mach.msgh_body.msgh_descriptor_count = 3;

    msg->__object__descriptor.type = MACH_MSG_PORT_DESCRIPTOR;

    msg->name__descriptor.type = MACH_MSG_OOL_DESCRIPTOR;
    msg->name__descriptor.copy = MACH_MSG_VIRTUAL_COPY;
    msg->name__descriptor.address = (void *) __builtin_offsetof(IOEventLink_Create_Msg_Content, name);
    msg->content.name = name;

    msg->userClient__descriptor.type = MACH_MSG_PORT_DESCRIPTOR;
    msg->content.userClient = (OSObjectRef) userClient;

    IORPC rpc = { .message = &buf.msg.mach, .reply = &buf.rpl.rpl.mach, .sendSize = sizeof(buf.msg), .replySize = sizeof(buf.rpl) };
    ret = OSMTypeID(IOEventLink)->Invoke(rpc);

    if (kIOReturnSuccess == ret)
    do {
        {
            if (rpl->mach.msgh.msgh_size                  != sizeof(*rpl)) { ret = kIOReturnIPCError; break; };
            if (rpl->content.__hdr.msgid                  != IOEventLink_Create_ID) { ret = kIOReturnIPCError; break; };
            if (rpl->mach.msgh_body.msgh_descriptor_count != 1) { ret = kIOReturnIPCError; break; };
            if (IOEventLink_Create_Rpl_ObjRefs   != rpl->content.__hdr.objectRefs) { ret = kIOReturnIPCError; break; };
        }
    }
    while (false);
    if (kIOReturnSuccess == ret)
    {
        *eventLink = OSDynamicCast(IOEventLink, (OSObject *) rpl->content.eventLink);
        if (rpl->content.eventLink && !*eventLink) ret = kIOReturnBadArgument;
    }


    return (ret);
}

kern_return_t
IOEventLink::SetEventlinkPort(
        mach_port_t port,
        OSDispatchMethod supermethod)
{
    kern_return_t ret;
    union
    {
        IOEventLink_SetEventlinkPort_Msg msg;
        struct
        {
            IOEventLink_SetEventlinkPort_Rpl rpl;
            mach_msg_max_trailer_t trailer;
        } rpl;
    } buf;
    struct IOEventLink_SetEventlinkPort_Msg * msg = &buf.msg;
    struct IOEventLink_SetEventlinkPort_Rpl * rpl = &buf.rpl.rpl;

    memset(msg, 0, sizeof(struct IOEventLink_SetEventlinkPort_Msg));
    msg->mach.msgh.msgh_id   = kIORPCVersion190615;
    msg->mach.msgh.msgh_size = sizeof(*msg);
    msg->content.__hdr.flags = 0*kIORPCMessageOneway
                             | 1*kIORPCMessageSimpleReply
                             | 0*kIORPCMessageLocalHost
                             | 0*kIORPCMessageOnqueue;
    msg->content.__hdr.msgid = IOEventLink_SetEventlinkPort_ID;
    msg->content.__object = (OSObjectRef) this;
    msg->content.__hdr.objectRefs = IOEventLink_SetEventlinkPort_Msg_ObjRefs;
    msg->mach.msgh_body.msgh_descriptor_count = 2;

    msg->__object__descriptor.type = MACH_MSG_PORT_DESCRIPTOR;

    msg->port__descriptor.type = MACH_MSG_PORT_DESCRIPTOR;
    msg->port__descriptor.disposition = MACH_MSG_TYPE_COPY_SEND;
    msg->port__descriptor.name = port;
    IORPC rpc = { .message = &buf.msg.mach, .reply = &buf.rpl.rpl.mach, .sendSize = sizeof(buf.msg), .replySize = sizeof(buf.rpl) };
    if (supermethod) ret = supermethod((OSObject *)this, rpc);
    else             ret = ((OSObject *)this)->Invoke(rpc);

    if (kIOReturnSuccess == ret)
    do {
        {
            if (rpl->mach.msgh.msgh_size                  != sizeof(*rpl)) { ret = kIOReturnIPCError; break; };
            if (rpl->content.__hdr.msgid                  != IOEventLink_SetEventlinkPort_ID) { ret = kIOReturnIPCError; break; };
            if (rpl->mach.msgh_body.msgh_descriptor_count != 0) { ret = kIOReturnIPCError; break; };
            if (IOEventLink_SetEventlinkPort_Rpl_ObjRefs   != rpl->content.__hdr.objectRefs) { ret = kIOReturnIPCError; break; };
        }
    }
    while (false);
    if (kIOReturnSuccess == ret)
    {
    }


    return (ret);
}

kern_return_t
IOEventLink::InvalidateKernel(
        IOUserClient * client,
        OSDispatchMethod supermethod)
{
    kern_return_t ret;
    union
    {
        IOEventLink_InvalidateKernel_Msg msg;
        struct
        {
            IOEventLink_InvalidateKernel_Rpl rpl;
            mach_msg_max_trailer_t trailer;
        } rpl;
    } buf;
    struct IOEventLink_InvalidateKernel_Msg * msg = &buf.msg;
    struct IOEventLink_InvalidateKernel_Rpl * rpl = &buf.rpl.rpl;

    memset(msg, 0, sizeof(struct IOEventLink_InvalidateKernel_Msg));
    msg->mach.msgh.msgh_id   = kIORPCVersion190615;
    msg->mach.msgh.msgh_size = sizeof(*msg);
    msg->content.__hdr.flags = 0*kIORPCMessageOneway
                             | 1*kIORPCMessageSimpleReply
                             | 0*kIORPCMessageLocalHost
                             | 0*kIORPCMessageOnqueue;
    msg->content.__hdr.msgid = IOEventLink_InvalidateKernel_ID;
    msg->content.__object = (OSObjectRef) this;
    msg->content.__hdr.objectRefs = IOEventLink_InvalidateKernel_Msg_ObjRefs;
    msg->mach.msgh_body.msgh_descriptor_count = 2;

    msg->__object__descriptor.type = MACH_MSG_PORT_DESCRIPTOR;

    msg->client__descriptor.type = MACH_MSG_PORT_DESCRIPTOR;
    msg->content.client = (OSObjectRef) client;

    IORPC rpc = { .message = &buf.msg.mach, .reply = &buf.rpl.rpl.mach, .sendSize = sizeof(buf.msg), .replySize = sizeof(buf.rpl) };
    if (supermethod) ret = supermethod((OSObject *)this, rpc);
    else             ret = ((OSObject *)this)->Invoke(rpc);

    if (kIOReturnSuccess == ret)
    do {
        {
            if (rpl->mach.msgh.msgh_size                  != sizeof(*rpl)) { ret = kIOReturnIPCError; break; };
            if (rpl->content.__hdr.msgid                  != IOEventLink_InvalidateKernel_ID) { ret = kIOReturnIPCError; break; };
            if (rpl->mach.msgh_body.msgh_descriptor_count != 0) { ret = kIOReturnIPCError; break; };
            if (IOEventLink_InvalidateKernel_Rpl_ObjRefs   != rpl->content.__hdr.objectRefs) { ret = kIOReturnIPCError; break; };
        }
    }
    while (false);
    if (kIOReturnSuccess == ret)
    {
    }


    return (ret);
}

kern_return_t
IOEventLink::Create_Invoke(const IORPC _rpc,
        Create_Handler func)
{
    IOEventLink_Create_Invocation rpc = { _rpc };
    kern_return_t ret;
    IOUserClient * userClient;

    if (3 != rpc.message->mach.msgh_body.msgh_descriptor_count) return (kIOReturnIPCError);
    if (IOEventLink_Create_Msg_ObjRefs != rpc.message->content.__hdr.objectRefs) return (kIOReturnIPCError);
    if (rpc.message != NULL && rpc.sendSize < sizeof(IOEventLink_Create_Msg)) return (kIOReturnIPCError);
    if (rpc.reply != NULL && rpc.replySize < sizeof(IOEventLink_Create_Rpl)) return (kIOReturnIPCError);
    if (((OSObject *) rpc.message->content.name) != NULL && OSDynamicCast(OSString, (OSObject *) rpc.message->content.name) == NULL) { return kIOReturnBadArgument; } 
    userClient = OSDynamicCast(IOUserClient, (OSObject *) rpc.message->content.userClient);
    if (!userClient && rpc.message->content.userClient) return (kIOReturnBadArgument);

    ret = (*func)(        rpc.message->content.name,
        userClient,
        (IOEventLink **)&rpc.reply->content.eventLink);

    if (kIOReturnSuccess != ret) return (ret);

    rpc.reply->content.__hdr.msgid = IOEventLink_Create_ID;
    rpc.reply->content.__hdr.flags = kIORPCMessageOneway;
    rpc.reply->mach.msgh.msgh_id   = kIORPCVersion190615Reply;
    rpc.reply->mach.msgh.msgh_size = sizeof(*rpc.reply);
    rpc.reply->mach.msgh_body.msgh_descriptor_count = 1;
    rpc.reply->content.__hdr.objectRefs = IOEventLink_Create_Rpl_ObjRefs;
    rpc.reply->eventLink__descriptor.type = MACH_MSG_PORT_DESCRIPTOR;

    return (ret);
}

kern_return_t
IOEventLink::SetEventlinkPort_Invoke(const IORPC _rpc,
        OSMetaClassBase * target,
        SetEventlinkPort_Handler func)
{
    IOEventLink_SetEventlinkPort_Invocation rpc = { _rpc };
    kern_return_t ret;

    if (2 != rpc.message->mach.msgh_body.msgh_descriptor_count) return (kIOReturnIPCError);
    if (IOEventLink_SetEventlinkPort_Msg_ObjRefs != rpc.message->content.__hdr.objectRefs) return (kIOReturnIPCError);
    if (rpc.message != NULL && rpc.sendSize < sizeof(IOEventLink_SetEventlinkPort_Msg)) return (kIOReturnIPCError);
    if (rpc.reply != NULL && rpc.replySize < sizeof(IOEventLink_SetEventlinkPort_Rpl)) return (kIOReturnIPCError);

    ret = (*func)(target,
        rpc.message->port__descriptor.name);

    if (kIOReturnSuccess != ret) return (ret);

    rpc.reply->content.__hdr.msgid = IOEventLink_SetEventlinkPort_ID;
    rpc.reply->content.__hdr.flags = kIORPCMessageOneway;
    rpc.reply->mach.msgh.msgh_id   = kIORPCVersion190615Reply;
    rpc.reply->mach.msgh.msgh_size = sizeof(*rpc.reply);
    rpc.reply->mach.msgh_body.msgh_descriptor_count = 0;
    rpc.reply->content.__hdr.objectRefs = IOEventLink_SetEventlinkPort_Rpl_ObjRefs;

    return (ret);
}

kern_return_t
IOEventLink::InvalidateKernel_Invoke(const IORPC _rpc,
        OSMetaClassBase * target,
        InvalidateKernel_Handler func)
{
    IOEventLink_InvalidateKernel_Invocation rpc = { _rpc };
    kern_return_t ret;
    IOUserClient * client;

    if (2 != rpc.message->mach.msgh_body.msgh_descriptor_count) return (kIOReturnIPCError);
    if (IOEventLink_InvalidateKernel_Msg_ObjRefs != rpc.message->content.__hdr.objectRefs) return (kIOReturnIPCError);
    if (rpc.message != NULL && rpc.sendSize < sizeof(IOEventLink_InvalidateKernel_Msg)) return (kIOReturnIPCError);
    if (rpc.reply != NULL && rpc.replySize < sizeof(IOEventLink_InvalidateKernel_Rpl)) return (kIOReturnIPCError);
    client = OSDynamicCast(IOUserClient, (OSObject *) rpc.message->content.client);
    if (!client && rpc.message->content.client) return (kIOReturnBadArgument);

    ret = (*func)(target,
        client);

    if (kIOReturnSuccess != ret) return (ret);

    rpc.reply->content.__hdr.msgid = IOEventLink_InvalidateKernel_ID;
    rpc.reply->content.__hdr.flags = kIORPCMessageOneway;
    rpc.reply->mach.msgh.msgh_id   = kIORPCVersion190615Reply;
    rpc.reply->mach.msgh.msgh_size = sizeof(*rpc.reply);
    rpc.reply->mach.msgh_body.msgh_descriptor_count = 0;
    rpc.reply->content.__hdr.objectRefs = IOEventLink_InvalidateKernel_Rpl_ObjRefs;

    return (ret);
}




