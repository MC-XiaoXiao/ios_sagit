/* iig(DriverKit-286 Mar 29 2024 10:27:09) generated from IOExtensiblePaniclog.iig */

#undef	IIG_IMPLEMENTATION
#define	IIG_IMPLEMENTATION 	IOExtensiblePaniclog.iig

#if KERNEL
#include <libkern/c++/OSString.h>
#else
#include <DriverKit/DriverKit.h>
#endif /* KERNEL */
#include <DriverKit/IOReturn.h>
#include <DriverKit/IORPC.h>
#include <DriverKit/IOExtensiblePaniclog.h>


#if __has_builtin(__builtin_load_member_function_pointer)
#define SimpleMemberFunctionCast(cfnty, self, func) (cfnty)__builtin_load_member_function_pointer(self, func)
#else
#define SimpleMemberFunctionCast(cfnty, self, func) ({ union { typeof(func) memfun; cfnty cfun; } pair; pair.memfun = func; pair.cfun; })
#endif


struct IOExtensiblePaniclog_Create_Msg_Content
{
    IORPCMessage __hdr;
    OSObjectRef  __object;
    OSData * uuid;
#if !defined(__LP64__)
    uint32_t __uuidPad;
#endif /* !defined(__LP64__) */
    OSString * data_id;
#if !defined(__LP64__)
    uint32_t __data_idPad;
#endif /* !defined(__LP64__) */
    uint32_t  max_len;
    uint32_t  options;
};
#pragma pack(4)
struct IOExtensiblePaniclog_Create_Msg
{
    IORPCMessageMach           mach;
    mach_msg_port_descriptor_t __object__descriptor;
    mach_msg_ool_descriptor_t  uuid__descriptor;
    mach_msg_ool_descriptor_t  data_id__descriptor;
    IOExtensiblePaniclog_Create_Msg_Content content;
};
#pragma pack()
#define IOExtensiblePaniclog_Create_Msg_ObjRefs (3)

struct IOExtensiblePaniclog_Create_Rpl_Content
{
    IORPCMessage __hdr;
    OSObjectRef  out;
};
#pragma pack(4)
struct IOExtensiblePaniclog_Create_Rpl
{
    IORPCMessageMach           mach;
    mach_msg_port_descriptor_t out__descriptor;
    IOExtensiblePaniclog_Create_Rpl_Content content;
};
#pragma pack()
#define IOExtensiblePaniclog_Create_Rpl_ObjRefs (1)


typedef union
{
    const IORPC rpc;
    struct
    {
        const struct IOExtensiblePaniclog_Create_Msg * message;
        struct IOExtensiblePaniclog_Create_Rpl       * reply;
        uint32_t sendSize;
        uint32_t replySize;
    };
}
IOExtensiblePaniclog_Create_Invocation;
struct IOExtensiblePaniclog_SetActive_Msg_Content
{
    IORPCMessage __hdr;
    OSObjectRef  __object;
};
#pragma pack(4)
struct IOExtensiblePaniclog_SetActive_Msg
{
    IORPCMessageMach           mach;
    mach_msg_port_descriptor_t __object__descriptor;
    IOExtensiblePaniclog_SetActive_Msg_Content content;
};
#pragma pack()
#define IOExtensiblePaniclog_SetActive_Msg_ObjRefs (1)

struct IOExtensiblePaniclog_SetActive_Rpl_Content
{
    IORPCMessage __hdr;
};
#pragma pack(4)
struct IOExtensiblePaniclog_SetActive_Rpl
{
    IORPCMessageMach           mach;
    IOExtensiblePaniclog_SetActive_Rpl_Content content;
};
#pragma pack()
#define IOExtensiblePaniclog_SetActive_Rpl_ObjRefs (0)


typedef union
{
    const IORPC rpc;
    struct
    {
        const struct IOExtensiblePaniclog_SetActive_Msg * message;
        struct IOExtensiblePaniclog_SetActive_Rpl       * reply;
        uint32_t sendSize;
        uint32_t replySize;
    };
}
IOExtensiblePaniclog_SetActive_Invocation;
struct IOExtensiblePaniclog_SetInactive_Msg_Content
{
    IORPCMessage __hdr;
    OSObjectRef  __object;
};
#pragma pack(4)
struct IOExtensiblePaniclog_SetInactive_Msg
{
    IORPCMessageMach           mach;
    mach_msg_port_descriptor_t __object__descriptor;
    IOExtensiblePaniclog_SetInactive_Msg_Content content;
};
#pragma pack()
#define IOExtensiblePaniclog_SetInactive_Msg_ObjRefs (1)

struct IOExtensiblePaniclog_SetInactive_Rpl_Content
{
    IORPCMessage __hdr;
};
#pragma pack(4)
struct IOExtensiblePaniclog_SetInactive_Rpl
{
    IORPCMessageMach           mach;
    IOExtensiblePaniclog_SetInactive_Rpl_Content content;
};
#pragma pack()
#define IOExtensiblePaniclog_SetInactive_Rpl_ObjRefs (0)


typedef union
{
    const IORPC rpc;
    struct
    {
        const struct IOExtensiblePaniclog_SetInactive_Msg * message;
        struct IOExtensiblePaniclog_SetInactive_Rpl       * reply;
        uint32_t sendSize;
        uint32_t replySize;
    };
}
IOExtensiblePaniclog_SetInactive_Invocation;
struct IOExtensiblePaniclog_InsertData_Msg_Content
{
    IORPCMessage __hdr;
    OSObjectRef  __object;
    OSData * data;
#if !defined(__LP64__)
    uint32_t __dataPad;
#endif /* !defined(__LP64__) */
};
#pragma pack(4)
struct IOExtensiblePaniclog_InsertData_Msg
{
    IORPCMessageMach           mach;
    mach_msg_port_descriptor_t __object__descriptor;
    mach_msg_ool_descriptor_t  data__descriptor;
    IOExtensiblePaniclog_InsertData_Msg_Content content;
};
#pragma pack()
#define IOExtensiblePaniclog_InsertData_Msg_ObjRefs (2)

struct IOExtensiblePaniclog_InsertData_Rpl_Content
{
    IORPCMessage __hdr;
};
#pragma pack(4)
struct IOExtensiblePaniclog_InsertData_Rpl
{
    IORPCMessageMach           mach;
    IOExtensiblePaniclog_InsertData_Rpl_Content content;
};
#pragma pack()
#define IOExtensiblePaniclog_InsertData_Rpl_ObjRefs (0)


typedef union
{
    const IORPC rpc;
    struct
    {
        const struct IOExtensiblePaniclog_InsertData_Msg * message;
        struct IOExtensiblePaniclog_InsertData_Rpl       * reply;
        uint32_t sendSize;
        uint32_t replySize;
    };
}
IOExtensiblePaniclog_InsertData_Invocation;
struct IOExtensiblePaniclog_AppendData_Msg_Content
{
    IORPCMessage __hdr;
    OSObjectRef  __object;
    OSData * data;
#if !defined(__LP64__)
    uint32_t __dataPad;
#endif /* !defined(__LP64__) */
};
#pragma pack(4)
struct IOExtensiblePaniclog_AppendData_Msg
{
    IORPCMessageMach           mach;
    mach_msg_port_descriptor_t __object__descriptor;
    mach_msg_ool_descriptor_t  data__descriptor;
    IOExtensiblePaniclog_AppendData_Msg_Content content;
};
#pragma pack()
#define IOExtensiblePaniclog_AppendData_Msg_ObjRefs (2)

struct IOExtensiblePaniclog_AppendData_Rpl_Content
{
    IORPCMessage __hdr;
};
#pragma pack(4)
struct IOExtensiblePaniclog_AppendData_Rpl
{
    IORPCMessageMach           mach;
    IOExtensiblePaniclog_AppendData_Rpl_Content content;
};
#pragma pack()
#define IOExtensiblePaniclog_AppendData_Rpl_ObjRefs (0)


typedef union
{
    const IORPC rpc;
    struct
    {
        const struct IOExtensiblePaniclog_AppendData_Msg * message;
        struct IOExtensiblePaniclog_AppendData_Rpl       * reply;
        uint32_t sendSize;
        uint32_t replySize;
    };
}
IOExtensiblePaniclog_AppendData_Invocation;
struct IOExtensiblePaniclog_CopyMemoryDescriptor_Msg_Content
{
    IORPCMessage __hdr;
    OSObjectRef  __object;
};
#pragma pack(4)
struct IOExtensiblePaniclog_CopyMemoryDescriptor_Msg
{
    IORPCMessageMach           mach;
    mach_msg_port_descriptor_t __object__descriptor;
    IOExtensiblePaniclog_CopyMemoryDescriptor_Msg_Content content;
};
#pragma pack()
#define IOExtensiblePaniclog_CopyMemoryDescriptor_Msg_ObjRefs (1)

struct IOExtensiblePaniclog_CopyMemoryDescriptor_Rpl_Content
{
    IORPCMessage __hdr;
    OSObjectRef  mem;
};
#pragma pack(4)
struct IOExtensiblePaniclog_CopyMemoryDescriptor_Rpl
{
    IORPCMessageMach           mach;
    mach_msg_port_descriptor_t mem__descriptor;
    IOExtensiblePaniclog_CopyMemoryDescriptor_Rpl_Content content;
};
#pragma pack()
#define IOExtensiblePaniclog_CopyMemoryDescriptor_Rpl_ObjRefs (1)


typedef union
{
    const IORPC rpc;
    struct
    {
        const struct IOExtensiblePaniclog_CopyMemoryDescriptor_Msg * message;
        struct IOExtensiblePaniclog_CopyMemoryDescriptor_Rpl       * reply;
        uint32_t sendSize;
        uint32_t replySize;
    };
}
IOExtensiblePaniclog_CopyMemoryDescriptor_Invocation;
struct IOExtensiblePaniclog_SetUsedLen_Msg_Content
{
    IORPCMessage __hdr;
    OSObjectRef  __object;
    uint32_t  used_len;
};
#pragma pack(4)
struct IOExtensiblePaniclog_SetUsedLen_Msg
{
    IORPCMessageMach           mach;
    mach_msg_port_descriptor_t __object__descriptor;
    IOExtensiblePaniclog_SetUsedLen_Msg_Content content;
};
#pragma pack()
#define IOExtensiblePaniclog_SetUsedLen_Msg_ObjRefs (1)

struct IOExtensiblePaniclog_SetUsedLen_Rpl_Content
{
    IORPCMessage __hdr;
};
#pragma pack(4)
struct IOExtensiblePaniclog_SetUsedLen_Rpl
{
    IORPCMessageMach           mach;
    IOExtensiblePaniclog_SetUsedLen_Rpl_Content content;
};
#pragma pack()
#define IOExtensiblePaniclog_SetUsedLen_Rpl_ObjRefs (0)


typedef union
{
    const IORPC rpc;
    struct
    {
        const struct IOExtensiblePaniclog_SetUsedLen_Msg * message;
        struct IOExtensiblePaniclog_SetUsedLen_Rpl       * reply;
        uint32_t sendSize;
        uint32_t replySize;
    };
}
IOExtensiblePaniclog_SetUsedLen_Invocation;
#if !KERNEL
extern OSMetaClass * gOSContainerMetaClass;
extern OSMetaClass * gOSDataMetaClass;
extern OSMetaClass * gOSNumberMetaClass;
extern OSMetaClass * gOSStringMetaClass;
extern OSMetaClass * gOSBooleanMetaClass;
extern OSMetaClass * gOSDictionaryMetaClass;
extern OSMetaClass * gOSArrayMetaClass;
extern OSMetaClass * gOSSetMetaClass;
extern OSMetaClass * gOSOrderedSetMetaClass;
extern OSMetaClass * gIODispatchQueueMetaClass;
extern OSMetaClass * gIOServiceMetaClass;
extern OSMetaClass * gIOMemoryMapMetaClass;
#endif /* !KERNEL */

#if !KERNEL

#define IOExtensiblePaniclog_QueueNames  ""

#define IOExtensiblePaniclog_MethodNames  ""

#define IOExtensiblePaniclogMetaClass_MethodNames  ""

struct OSClassDescription_IOExtensiblePaniclog_t
{
    OSClassDescription base;
    uint64_t           methodOptions[2 * 0];
    uint64_t           metaMethodOptions[2 * 0];
    char               queueNames[sizeof(IOExtensiblePaniclog_QueueNames)];
    char               methodNames[sizeof(IOExtensiblePaniclog_MethodNames)];
    char               metaMethodNames[sizeof(IOExtensiblePaniclogMetaClass_MethodNames)];
};

const struct OSClassDescription_IOExtensiblePaniclog_t
OSClassDescription_IOExtensiblePaniclog =
{
    .base =
    {
        .descriptionSize         = sizeof(OSClassDescription_IOExtensiblePaniclog_t),
        .name                    = "IOExtensiblePaniclog",
        .superName               = "OSObject",
        .methodOptionsSize       = 2 * sizeof(uint64_t) * 0,
        .methodOptionsOffset     = __builtin_offsetof(struct OSClassDescription_IOExtensiblePaniclog_t, methodOptions),
        .metaMethodOptionsSize   = 2 * sizeof(uint64_t) * 0,
        .metaMethodOptionsOffset = __builtin_offsetof(struct OSClassDescription_IOExtensiblePaniclog_t, metaMethodOptions),
        .queueNamesSize       = sizeof(IOExtensiblePaniclog_QueueNames),
        .queueNamesOffset     = __builtin_offsetof(struct OSClassDescription_IOExtensiblePaniclog_t, queueNames),
        .methodNamesSize         = sizeof(IOExtensiblePaniclog_MethodNames),
        .methodNamesOffset       = __builtin_offsetof(struct OSClassDescription_IOExtensiblePaniclog_t, methodNames),
        .metaMethodNamesSize     = sizeof(IOExtensiblePaniclogMetaClass_MethodNames),
        .metaMethodNamesOffset   = __builtin_offsetof(struct OSClassDescription_IOExtensiblePaniclog_t, metaMethodNames),
        .flags                   = 1*kOSClassCanRemote,
    },
    .methodOptions =
    {
    },
    .metaMethodOptions =
    {
    },
    .queueNames      = IOExtensiblePaniclog_QueueNames,
    .methodNames     = IOExtensiblePaniclog_MethodNames,
    .metaMethodNames = IOExtensiblePaniclogMetaClass_MethodNames,
};

OSMetaClass * gIOExtensiblePaniclogMetaClass;

static kern_return_t
IOExtensiblePaniclog_New(OSMetaClass * instance);

const OSClassLoadInformation
IOExtensiblePaniclog_Class = 
{
    .description       = &OSClassDescription_IOExtensiblePaniclog.base,
    .metaPointer       = &gIOExtensiblePaniclogMetaClass,
    .version           = 1,
    .instanceSize      = sizeof(IOExtensiblePaniclog),

    .New               = &IOExtensiblePaniclog_New,
};

extern const void * const
gIOExtensiblePaniclog_Declaration;
const void * const
gIOExtensiblePaniclog_Declaration
__attribute__((visibility("hidden"),section("__DATA_CONST,__osclassinfo,regular,no_dead_strip"),no_sanitize("address")))
    = &IOExtensiblePaniclog_Class;

static kern_return_t
IOExtensiblePaniclog_New(OSMetaClass * instance)
{
    if (!new(instance) IOExtensiblePaniclogMetaClass) return (kIOReturnNoMemory);
    return (kIOReturnSuccess);
}

kern_return_t
IOExtensiblePaniclogMetaClass::New(OSObject * instance)
{
    if (!new(instance) IOExtensiblePaniclog) return (kIOReturnNoMemory);
    return (kIOReturnSuccess);
}

#endif /* !KERNEL */

kern_return_t
IOExtensiblePaniclog::Dispatch(const IORPC rpc)
{
    return _Dispatch(this, rpc);
}

kern_return_t
IOExtensiblePaniclog::_Dispatch(IOExtensiblePaniclog * self, const IORPC rpc)
{
    kern_return_t ret = kIOReturnUnsupported;
    IORPCMessage * msg = IORPCMessageFromMach(rpc.message, false);

    switch (msg->msgid)
    {
#if KERNEL
        case IOExtensiblePaniclog_SetActive_ID:
        {
            ret = IOExtensiblePaniclog::SetActive_Invoke(rpc, self, SimpleMemberFunctionCast(IOExtensiblePaniclog::SetActive_Handler, *self, &IOExtensiblePaniclog::SetActive_Impl));
            break;
        }
#endif /* !KERNEL */
#if KERNEL
        case IOExtensiblePaniclog_SetInactive_ID:
        {
            ret = IOExtensiblePaniclog::SetInactive_Invoke(rpc, self, SimpleMemberFunctionCast(IOExtensiblePaniclog::SetInactive_Handler, *self, &IOExtensiblePaniclog::SetInactive_Impl));
            break;
        }
#endif /* !KERNEL */
#if KERNEL
        case IOExtensiblePaniclog_InsertData_ID:
        {
            ret = IOExtensiblePaniclog::InsertData_Invoke(rpc, self, SimpleMemberFunctionCast(IOExtensiblePaniclog::InsertData_Handler, *self, &IOExtensiblePaniclog::InsertData_Impl));
            break;
        }
#endif /* !KERNEL */
#if KERNEL
        case IOExtensiblePaniclog_AppendData_ID:
        {
            ret = IOExtensiblePaniclog::AppendData_Invoke(rpc, self, SimpleMemberFunctionCast(IOExtensiblePaniclog::AppendData_Handler, *self, &IOExtensiblePaniclog::AppendData_Impl));
            break;
        }
#endif /* !KERNEL */
#if KERNEL
        case IOExtensiblePaniclog_CopyMemoryDescriptor_ID:
        {
            ret = IOExtensiblePaniclog::CopyMemoryDescriptor_Invoke(rpc, self, SimpleMemberFunctionCast(IOExtensiblePaniclog::CopyMemoryDescriptor_Handler, *self, &IOExtensiblePaniclog::CopyMemoryDescriptor_Impl));
            break;
        }
#endif /* !KERNEL */
#if KERNEL
        case IOExtensiblePaniclog_SetUsedLen_ID:
        {
            ret = IOExtensiblePaniclog::SetUsedLen_Invoke(rpc, self, SimpleMemberFunctionCast(IOExtensiblePaniclog::SetUsedLen_Handler, *self, &IOExtensiblePaniclog::SetUsedLen_Impl));
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
IOExtensiblePaniclog::MetaClass::Dispatch(const IORPC rpc)
{
#else /* KERNEL */
kern_return_t
IOExtensiblePaniclogMetaClass::Dispatch(const IORPC rpc)
{
#endif /* !KERNEL */

    kern_return_t ret = kIOReturnUnsupported;
    IORPCMessage * msg = IORPCMessageFromMach(rpc.message, false);

    switch (msg->msgid)
    {
#if KERNEL
        case IOExtensiblePaniclog_Create_ID:
            ret = IOExtensiblePaniclog::Create_Invoke(rpc, &IOExtensiblePaniclog::Create_Impl);
            break;
#endif /* !KERNEL */

        default:
            ret = OSMetaClassBase::Dispatch(rpc);
            break;
    }

    return (ret);
}

kern_return_t
IOExtensiblePaniclog::Create(
        OSData * uuid,
        OSString * data_id,
        uint32_t max_len,
        uint32_t options,
        IOExtensiblePaniclog ** out)
{
    kern_return_t ret;
    union
    {
        IOExtensiblePaniclog_Create_Msg msg;
        struct
        {
            IOExtensiblePaniclog_Create_Rpl rpl;
            mach_msg_max_trailer_t trailer;
        } rpl;
    } buf;
    struct IOExtensiblePaniclog_Create_Msg * msg = &buf.msg;
    struct IOExtensiblePaniclog_Create_Rpl * rpl = &buf.rpl.rpl;

    memset(msg, 0, sizeof(struct IOExtensiblePaniclog_Create_Msg));
    msg->mach.msgh.msgh_id   = kIORPCVersion190615;
    msg->mach.msgh.msgh_size = sizeof(*msg);
    msg->content.__hdr.flags = 0*kIORPCMessageOneway
                             | 0*kIORPCMessageSimpleReply
                             | 0*kIORPCMessageLocalHost
                             | 0*kIORPCMessageOnqueue;
    msg->content.__hdr.msgid = IOExtensiblePaniclog_Create_ID;
    msg->content.__object = (OSObjectRef) OSTypeID(IOExtensiblePaniclog);
    msg->content.__hdr.objectRefs = IOExtensiblePaniclog_Create_Msg_ObjRefs;
    msg->mach.msgh_body.msgh_descriptor_count = 3;

    msg->__object__descriptor.type = MACH_MSG_PORT_DESCRIPTOR;

    msg->uuid__descriptor.type = MACH_MSG_OOL_DESCRIPTOR;
    msg->uuid__descriptor.copy = MACH_MSG_VIRTUAL_COPY;
    msg->uuid__descriptor.address = (void *) __builtin_offsetof(IOExtensiblePaniclog_Create_Msg_Content, uuid);
    msg->content.uuid = uuid;

    msg->data_id__descriptor.type = MACH_MSG_OOL_DESCRIPTOR;
    msg->data_id__descriptor.copy = MACH_MSG_VIRTUAL_COPY;
    msg->data_id__descriptor.address = (void *) __builtin_offsetof(IOExtensiblePaniclog_Create_Msg_Content, data_id);
    msg->content.data_id = data_id;

    msg->content.max_len = max_len;

    msg->content.options = options;

    IORPC rpc = { .message = &buf.msg.mach, .reply = &buf.rpl.rpl.mach, .sendSize = sizeof(buf.msg), .replySize = sizeof(buf.rpl) };
    ret = OSMTypeID(IOExtensiblePaniclog)->Invoke(rpc);

    if (kIOReturnSuccess == ret)
    do {
        {
            if (rpl->mach.msgh.msgh_size                  != sizeof(*rpl)) { ret = kIOReturnIPCError; break; };
            if (rpl->content.__hdr.msgid                  != IOExtensiblePaniclog_Create_ID) { ret = kIOReturnIPCError; break; };
            if (rpl->mach.msgh_body.msgh_descriptor_count != 1) { ret = kIOReturnIPCError; break; };
            if (IOExtensiblePaniclog_Create_Rpl_ObjRefs   != rpl->content.__hdr.objectRefs) { ret = kIOReturnIPCError; break; };
        }
    }
    while (false);
    if (kIOReturnSuccess == ret)
    {
        *out = OSDynamicCast(IOExtensiblePaniclog, (OSObject *) rpl->content.out);
        if (rpl->content.out && !*out) ret = kIOReturnBadArgument;
    }


    return (ret);
}

kern_return_t
IOExtensiblePaniclog::SetActive(        OSDispatchMethod supermethod)
{
    kern_return_t ret;
    union
    {
        IOExtensiblePaniclog_SetActive_Msg msg;
        struct
        {
            IOExtensiblePaniclog_SetActive_Rpl rpl;
            mach_msg_max_trailer_t trailer;
        } rpl;
    } buf;
    struct IOExtensiblePaniclog_SetActive_Msg * msg = &buf.msg;
    struct IOExtensiblePaniclog_SetActive_Rpl * rpl = &buf.rpl.rpl;

    memset(msg, 0, sizeof(struct IOExtensiblePaniclog_SetActive_Msg));
    msg->mach.msgh.msgh_id   = kIORPCVersion190615;
    msg->mach.msgh.msgh_size = sizeof(*msg);
    msg->content.__hdr.flags = 0*kIORPCMessageOneway
                             | 1*kIORPCMessageSimpleReply
                             | 0*kIORPCMessageLocalHost
                             | 0*kIORPCMessageOnqueue;
    msg->content.__hdr.msgid = IOExtensiblePaniclog_SetActive_ID;
    msg->content.__object = (OSObjectRef) this;
    msg->content.__hdr.objectRefs = IOExtensiblePaniclog_SetActive_Msg_ObjRefs;
    msg->mach.msgh_body.msgh_descriptor_count = 1;

    msg->__object__descriptor.type = MACH_MSG_PORT_DESCRIPTOR;

    IORPC rpc = { .message = &buf.msg.mach, .reply = &buf.rpl.rpl.mach, .sendSize = sizeof(buf.msg), .replySize = sizeof(buf.rpl) };
    if (supermethod) ret = supermethod((OSObject *)this, rpc);
    else             ret = ((OSObject *)this)->Invoke(rpc);

    if (kIOReturnSuccess == ret)
    do {
        {
            if (rpl->mach.msgh.msgh_size                  != sizeof(*rpl)) { ret = kIOReturnIPCError; break; };
            if (rpl->content.__hdr.msgid                  != IOExtensiblePaniclog_SetActive_ID) { ret = kIOReturnIPCError; break; };
            if (rpl->mach.msgh_body.msgh_descriptor_count != 0) { ret = kIOReturnIPCError; break; };
            if (IOExtensiblePaniclog_SetActive_Rpl_ObjRefs   != rpl->content.__hdr.objectRefs) { ret = kIOReturnIPCError; break; };
        }
    }
    while (false);
    if (kIOReturnSuccess == ret)
    {
    }


    return (ret);
}

kern_return_t
IOExtensiblePaniclog::SetInactive(        OSDispatchMethod supermethod)
{
    kern_return_t ret;
    union
    {
        IOExtensiblePaniclog_SetInactive_Msg msg;
        struct
        {
            IOExtensiblePaniclog_SetInactive_Rpl rpl;
            mach_msg_max_trailer_t trailer;
        } rpl;
    } buf;
    struct IOExtensiblePaniclog_SetInactive_Msg * msg = &buf.msg;
    struct IOExtensiblePaniclog_SetInactive_Rpl * rpl = &buf.rpl.rpl;

    memset(msg, 0, sizeof(struct IOExtensiblePaniclog_SetInactive_Msg));
    msg->mach.msgh.msgh_id   = kIORPCVersion190615;
    msg->mach.msgh.msgh_size = sizeof(*msg);
    msg->content.__hdr.flags = 0*kIORPCMessageOneway
                             | 1*kIORPCMessageSimpleReply
                             | 0*kIORPCMessageLocalHost
                             | 0*kIORPCMessageOnqueue;
    msg->content.__hdr.msgid = IOExtensiblePaniclog_SetInactive_ID;
    msg->content.__object = (OSObjectRef) this;
    msg->content.__hdr.objectRefs = IOExtensiblePaniclog_SetInactive_Msg_ObjRefs;
    msg->mach.msgh_body.msgh_descriptor_count = 1;

    msg->__object__descriptor.type = MACH_MSG_PORT_DESCRIPTOR;

    IORPC rpc = { .message = &buf.msg.mach, .reply = &buf.rpl.rpl.mach, .sendSize = sizeof(buf.msg), .replySize = sizeof(buf.rpl) };
    if (supermethod) ret = supermethod((OSObject *)this, rpc);
    else             ret = ((OSObject *)this)->Invoke(rpc);

    if (kIOReturnSuccess == ret)
    do {
        {
            if (rpl->mach.msgh.msgh_size                  != sizeof(*rpl)) { ret = kIOReturnIPCError; break; };
            if (rpl->content.__hdr.msgid                  != IOExtensiblePaniclog_SetInactive_ID) { ret = kIOReturnIPCError; break; };
            if (rpl->mach.msgh_body.msgh_descriptor_count != 0) { ret = kIOReturnIPCError; break; };
            if (IOExtensiblePaniclog_SetInactive_Rpl_ObjRefs   != rpl->content.__hdr.objectRefs) { ret = kIOReturnIPCError; break; };
        }
    }
    while (false);
    if (kIOReturnSuccess == ret)
    {
    }


    return (ret);
}

kern_return_t
IOExtensiblePaniclog::InsertData(
        OSData * data,
        OSDispatchMethod supermethod)
{
    kern_return_t ret;
    union
    {
        IOExtensiblePaniclog_InsertData_Msg msg;
        struct
        {
            IOExtensiblePaniclog_InsertData_Rpl rpl;
            mach_msg_max_trailer_t trailer;
        } rpl;
    } buf;
    struct IOExtensiblePaniclog_InsertData_Msg * msg = &buf.msg;
    struct IOExtensiblePaniclog_InsertData_Rpl * rpl = &buf.rpl.rpl;

    memset(msg, 0, sizeof(struct IOExtensiblePaniclog_InsertData_Msg));
    msg->mach.msgh.msgh_id   = kIORPCVersion190615;
    msg->mach.msgh.msgh_size = sizeof(*msg);
    msg->content.__hdr.flags = 0*kIORPCMessageOneway
                             | 0*kIORPCMessageSimpleReply
                             | 0*kIORPCMessageLocalHost
                             | 0*kIORPCMessageOnqueue;
    msg->content.__hdr.msgid = IOExtensiblePaniclog_InsertData_ID;
    msg->content.__object = (OSObjectRef) this;
    msg->content.__hdr.objectRefs = IOExtensiblePaniclog_InsertData_Msg_ObjRefs;
    msg->mach.msgh_body.msgh_descriptor_count = 2;

    msg->__object__descriptor.type = MACH_MSG_PORT_DESCRIPTOR;

    msg->data__descriptor.type = MACH_MSG_OOL_DESCRIPTOR;
    msg->data__descriptor.copy = MACH_MSG_VIRTUAL_COPY;
    msg->data__descriptor.address = (void *) __builtin_offsetof(IOExtensiblePaniclog_InsertData_Msg_Content, data);
    msg->content.data = data;

    IORPC rpc = { .message = &buf.msg.mach, .reply = &buf.rpl.rpl.mach, .sendSize = sizeof(buf.msg), .replySize = sizeof(buf.rpl) };
    if (supermethod) ret = supermethod((OSObject *)this, rpc);
    else             ret = ((OSObject *)this)->Invoke(rpc);

    if (kIOReturnSuccess == ret)
    do {
        {
            if (rpl->mach.msgh.msgh_size                  != sizeof(*rpl)) { ret = kIOReturnIPCError; break; };
            if (rpl->content.__hdr.msgid                  != IOExtensiblePaniclog_InsertData_ID) { ret = kIOReturnIPCError; break; };
            if (rpl->mach.msgh_body.msgh_descriptor_count != 0) { ret = kIOReturnIPCError; break; };
            if (IOExtensiblePaniclog_InsertData_Rpl_ObjRefs   != rpl->content.__hdr.objectRefs) { ret = kIOReturnIPCError; break; };
        }
    }
    while (false);
    if (kIOReturnSuccess == ret)
    {
    }


    return (ret);
}

kern_return_t
IOExtensiblePaniclog::AppendData(
        OSData * data,
        OSDispatchMethod supermethod)
{
    kern_return_t ret;
    union
    {
        IOExtensiblePaniclog_AppendData_Msg msg;
        struct
        {
            IOExtensiblePaniclog_AppendData_Rpl rpl;
            mach_msg_max_trailer_t trailer;
        } rpl;
    } buf;
    struct IOExtensiblePaniclog_AppendData_Msg * msg = &buf.msg;
    struct IOExtensiblePaniclog_AppendData_Rpl * rpl = &buf.rpl.rpl;

    memset(msg, 0, sizeof(struct IOExtensiblePaniclog_AppendData_Msg));
    msg->mach.msgh.msgh_id   = kIORPCVersion190615;
    msg->mach.msgh.msgh_size = sizeof(*msg);
    msg->content.__hdr.flags = 0*kIORPCMessageOneway
                             | 0*kIORPCMessageSimpleReply
                             | 0*kIORPCMessageLocalHost
                             | 0*kIORPCMessageOnqueue;
    msg->content.__hdr.msgid = IOExtensiblePaniclog_AppendData_ID;
    msg->content.__object = (OSObjectRef) this;
    msg->content.__hdr.objectRefs = IOExtensiblePaniclog_AppendData_Msg_ObjRefs;
    msg->mach.msgh_body.msgh_descriptor_count = 2;

    msg->__object__descriptor.type = MACH_MSG_PORT_DESCRIPTOR;

    msg->data__descriptor.type = MACH_MSG_OOL_DESCRIPTOR;
    msg->data__descriptor.copy = MACH_MSG_VIRTUAL_COPY;
    msg->data__descriptor.address = (void *) __builtin_offsetof(IOExtensiblePaniclog_AppendData_Msg_Content, data);
    msg->content.data = data;

    IORPC rpc = { .message = &buf.msg.mach, .reply = &buf.rpl.rpl.mach, .sendSize = sizeof(buf.msg), .replySize = sizeof(buf.rpl) };
    if (supermethod) ret = supermethod((OSObject *)this, rpc);
    else             ret = ((OSObject *)this)->Invoke(rpc);

    if (kIOReturnSuccess == ret)
    do {
        {
            if (rpl->mach.msgh.msgh_size                  != sizeof(*rpl)) { ret = kIOReturnIPCError; break; };
            if (rpl->content.__hdr.msgid                  != IOExtensiblePaniclog_AppendData_ID) { ret = kIOReturnIPCError; break; };
            if (rpl->mach.msgh_body.msgh_descriptor_count != 0) { ret = kIOReturnIPCError; break; };
            if (IOExtensiblePaniclog_AppendData_Rpl_ObjRefs   != rpl->content.__hdr.objectRefs) { ret = kIOReturnIPCError; break; };
        }
    }
    while (false);
    if (kIOReturnSuccess == ret)
    {
    }


    return (ret);
}

kern_return_t
IOExtensiblePaniclog::CopyMemoryDescriptor(
        IOBufferMemoryDescriptor ** mem,
        OSDispatchMethod supermethod)
{
    kern_return_t ret;
    union
    {
        IOExtensiblePaniclog_CopyMemoryDescriptor_Msg msg;
        struct
        {
            IOExtensiblePaniclog_CopyMemoryDescriptor_Rpl rpl;
            mach_msg_max_trailer_t trailer;
        } rpl;
    } buf;
    struct IOExtensiblePaniclog_CopyMemoryDescriptor_Msg * msg = &buf.msg;
    struct IOExtensiblePaniclog_CopyMemoryDescriptor_Rpl * rpl = &buf.rpl.rpl;

    memset(msg, 0, sizeof(struct IOExtensiblePaniclog_CopyMemoryDescriptor_Msg));
    msg->mach.msgh.msgh_id   = kIORPCVersion190615;
    msg->mach.msgh.msgh_size = sizeof(*msg);
    msg->content.__hdr.flags = 0*kIORPCMessageOneway
                             | 0*kIORPCMessageSimpleReply
                             | 0*kIORPCMessageLocalHost
                             | 0*kIORPCMessageOnqueue;
    msg->content.__hdr.msgid = IOExtensiblePaniclog_CopyMemoryDescriptor_ID;
    msg->content.__object = (OSObjectRef) this;
    msg->content.__hdr.objectRefs = IOExtensiblePaniclog_CopyMemoryDescriptor_Msg_ObjRefs;
    msg->mach.msgh_body.msgh_descriptor_count = 1;

    msg->__object__descriptor.type = MACH_MSG_PORT_DESCRIPTOR;

    IORPC rpc = { .message = &buf.msg.mach, .reply = &buf.rpl.rpl.mach, .sendSize = sizeof(buf.msg), .replySize = sizeof(buf.rpl) };
    if (supermethod) ret = supermethod((OSObject *)this, rpc);
    else             ret = ((OSObject *)this)->Invoke(rpc);

    if (kIOReturnSuccess == ret)
    do {
        {
            if (rpl->mach.msgh.msgh_size                  != sizeof(*rpl)) { ret = kIOReturnIPCError; break; };
            if (rpl->content.__hdr.msgid                  != IOExtensiblePaniclog_CopyMemoryDescriptor_ID) { ret = kIOReturnIPCError; break; };
            if (rpl->mach.msgh_body.msgh_descriptor_count != 1) { ret = kIOReturnIPCError; break; };
            if (IOExtensiblePaniclog_CopyMemoryDescriptor_Rpl_ObjRefs   != rpl->content.__hdr.objectRefs) { ret = kIOReturnIPCError; break; };
        }
    }
    while (false);
    if (kIOReturnSuccess == ret)
    {
        *mem = OSDynamicCast(IOBufferMemoryDescriptor, (OSObject *) rpl->content.mem);
        if (rpl->content.mem && !*mem) ret = kIOReturnBadArgument;
    }


    return (ret);
}

kern_return_t
IOExtensiblePaniclog::SetUsedLen(
        uint32_t used_len,
        OSDispatchMethod supermethod)
{
    kern_return_t ret;
    union
    {
        IOExtensiblePaniclog_SetUsedLen_Msg msg;
        struct
        {
            IOExtensiblePaniclog_SetUsedLen_Rpl rpl;
            mach_msg_max_trailer_t trailer;
        } rpl;
    } buf;
    struct IOExtensiblePaniclog_SetUsedLen_Msg * msg = &buf.msg;
    struct IOExtensiblePaniclog_SetUsedLen_Rpl * rpl = &buf.rpl.rpl;

    memset(msg, 0, sizeof(struct IOExtensiblePaniclog_SetUsedLen_Msg));
    msg->mach.msgh.msgh_id   = kIORPCVersion190615;
    msg->mach.msgh.msgh_size = sizeof(*msg);
    msg->content.__hdr.flags = 0*kIORPCMessageOneway
                             | 1*kIORPCMessageSimpleReply
                             | 0*kIORPCMessageLocalHost
                             | 0*kIORPCMessageOnqueue;
    msg->content.__hdr.msgid = IOExtensiblePaniclog_SetUsedLen_ID;
    msg->content.__object = (OSObjectRef) this;
    msg->content.__hdr.objectRefs = IOExtensiblePaniclog_SetUsedLen_Msg_ObjRefs;
    msg->mach.msgh_body.msgh_descriptor_count = 1;

    msg->__object__descriptor.type = MACH_MSG_PORT_DESCRIPTOR;

    msg->content.used_len = used_len;

    IORPC rpc = { .message = &buf.msg.mach, .reply = &buf.rpl.rpl.mach, .sendSize = sizeof(buf.msg), .replySize = sizeof(buf.rpl) };
    if (supermethod) ret = supermethod((OSObject *)this, rpc);
    else             ret = ((OSObject *)this)->Invoke(rpc);

    if (kIOReturnSuccess == ret)
    do {
        {
            if (rpl->mach.msgh.msgh_size                  != sizeof(*rpl)) { ret = kIOReturnIPCError; break; };
            if (rpl->content.__hdr.msgid                  != IOExtensiblePaniclog_SetUsedLen_ID) { ret = kIOReturnIPCError; break; };
            if (rpl->mach.msgh_body.msgh_descriptor_count != 0) { ret = kIOReturnIPCError; break; };
            if (IOExtensiblePaniclog_SetUsedLen_Rpl_ObjRefs   != rpl->content.__hdr.objectRefs) { ret = kIOReturnIPCError; break; };
        }
    }
    while (false);
    if (kIOReturnSuccess == ret)
    {
    }


    return (ret);
}

kern_return_t
IOExtensiblePaniclog::Create_Invoke(const IORPC _rpc,
        Create_Handler func)
{
    IOExtensiblePaniclog_Create_Invocation rpc = { _rpc };
    kern_return_t ret;

    if (3 != rpc.message->mach.msgh_body.msgh_descriptor_count) return (kIOReturnIPCError);
    if (IOExtensiblePaniclog_Create_Msg_ObjRefs != rpc.message->content.__hdr.objectRefs) return (kIOReturnIPCError);
    if (rpc.message != NULL && rpc.sendSize < sizeof(IOExtensiblePaniclog_Create_Msg)) return (kIOReturnIPCError);
    if (rpc.reply != NULL && rpc.replySize < sizeof(IOExtensiblePaniclog_Create_Rpl)) return (kIOReturnIPCError);
    if (((OSObject *) rpc.message->content.uuid) != NULL && OSDynamicCast(OSData, (OSObject *) rpc.message->content.uuid) == NULL) { return kIOReturnBadArgument; } 
    if (((OSObject *) rpc.message->content.data_id) != NULL && OSDynamicCast(OSString, (OSObject *) rpc.message->content.data_id) == NULL) { return kIOReturnBadArgument; } 

    ret = (*func)(        rpc.message->content.uuid,
        rpc.message->content.data_id,
        rpc.message->content.max_len,
        rpc.message->content.options,
        (IOExtensiblePaniclog **)&rpc.reply->content.out);

    if (kIOReturnSuccess != ret) return (ret);

    rpc.reply->content.__hdr.msgid = IOExtensiblePaniclog_Create_ID;
    rpc.reply->content.__hdr.flags = kIORPCMessageOneway;
    rpc.reply->mach.msgh.msgh_id   = kIORPCVersion190615Reply;
    rpc.reply->mach.msgh.msgh_size = sizeof(*rpc.reply);
    rpc.reply->mach.msgh_body.msgh_descriptor_count = 1;
    rpc.reply->content.__hdr.objectRefs = IOExtensiblePaniclog_Create_Rpl_ObjRefs;
    rpc.reply->out__descriptor.type = MACH_MSG_PORT_DESCRIPTOR;

    return (ret);
}

kern_return_t
IOExtensiblePaniclog::SetActive_Invoke(const IORPC _rpc,
        OSMetaClassBase * target,
        SetActive_Handler func)
{
    IOExtensiblePaniclog_SetActive_Invocation rpc = { _rpc };
    kern_return_t ret;

    if (1 != rpc.message->mach.msgh_body.msgh_descriptor_count) return (kIOReturnIPCError);
    if (IOExtensiblePaniclog_SetActive_Msg_ObjRefs != rpc.message->content.__hdr.objectRefs) return (kIOReturnIPCError);
    if (rpc.message != NULL && rpc.sendSize < sizeof(IOExtensiblePaniclog_SetActive_Msg)) return (kIOReturnIPCError);
    if (rpc.reply != NULL && rpc.replySize < sizeof(IOExtensiblePaniclog_SetActive_Rpl)) return (kIOReturnIPCError);

    ret = (*func)(target);

    if (kIOReturnSuccess != ret) return (ret);

    rpc.reply->content.__hdr.msgid = IOExtensiblePaniclog_SetActive_ID;
    rpc.reply->content.__hdr.flags = kIORPCMessageOneway;
    rpc.reply->mach.msgh.msgh_id   = kIORPCVersion190615Reply;
    rpc.reply->mach.msgh.msgh_size = sizeof(*rpc.reply);
    rpc.reply->mach.msgh_body.msgh_descriptor_count = 0;
    rpc.reply->content.__hdr.objectRefs = IOExtensiblePaniclog_SetActive_Rpl_ObjRefs;

    return (ret);
}

kern_return_t
IOExtensiblePaniclog::SetInactive_Invoke(const IORPC _rpc,
        OSMetaClassBase * target,
        SetInactive_Handler func)
{
    IOExtensiblePaniclog_SetInactive_Invocation rpc = { _rpc };
    kern_return_t ret;

    if (1 != rpc.message->mach.msgh_body.msgh_descriptor_count) return (kIOReturnIPCError);
    if (IOExtensiblePaniclog_SetInactive_Msg_ObjRefs != rpc.message->content.__hdr.objectRefs) return (kIOReturnIPCError);
    if (rpc.message != NULL && rpc.sendSize < sizeof(IOExtensiblePaniclog_SetInactive_Msg)) return (kIOReturnIPCError);
    if (rpc.reply != NULL && rpc.replySize < sizeof(IOExtensiblePaniclog_SetInactive_Rpl)) return (kIOReturnIPCError);

    ret = (*func)(target);

    if (kIOReturnSuccess != ret) return (ret);

    rpc.reply->content.__hdr.msgid = IOExtensiblePaniclog_SetInactive_ID;
    rpc.reply->content.__hdr.flags = kIORPCMessageOneway;
    rpc.reply->mach.msgh.msgh_id   = kIORPCVersion190615Reply;
    rpc.reply->mach.msgh.msgh_size = sizeof(*rpc.reply);
    rpc.reply->mach.msgh_body.msgh_descriptor_count = 0;
    rpc.reply->content.__hdr.objectRefs = IOExtensiblePaniclog_SetInactive_Rpl_ObjRefs;

    return (ret);
}

kern_return_t
IOExtensiblePaniclog::InsertData_Invoke(const IORPC _rpc,
        OSMetaClassBase * target,
        InsertData_Handler func)
{
    IOExtensiblePaniclog_InsertData_Invocation rpc = { _rpc };
    kern_return_t ret;

    if (2 != rpc.message->mach.msgh_body.msgh_descriptor_count) return (kIOReturnIPCError);
    if (IOExtensiblePaniclog_InsertData_Msg_ObjRefs != rpc.message->content.__hdr.objectRefs) return (kIOReturnIPCError);
    if (rpc.message != NULL && rpc.sendSize < sizeof(IOExtensiblePaniclog_InsertData_Msg)) return (kIOReturnIPCError);
    if (rpc.reply != NULL && rpc.replySize < sizeof(IOExtensiblePaniclog_InsertData_Rpl)) return (kIOReturnIPCError);
    if (((OSObject *) rpc.message->content.data) != NULL && OSDynamicCast(OSData, (OSObject *) rpc.message->content.data) == NULL) { return kIOReturnBadArgument; } 

    ret = (*func)(target,
        rpc.message->content.data);

    if (kIOReturnSuccess != ret) return (ret);

    rpc.reply->content.__hdr.msgid = IOExtensiblePaniclog_InsertData_ID;
    rpc.reply->content.__hdr.flags = kIORPCMessageOneway;
    rpc.reply->mach.msgh.msgh_id   = kIORPCVersion190615Reply;
    rpc.reply->mach.msgh.msgh_size = sizeof(*rpc.reply);
    rpc.reply->mach.msgh_body.msgh_descriptor_count = 0;
    rpc.reply->content.__hdr.objectRefs = IOExtensiblePaniclog_InsertData_Rpl_ObjRefs;

    return (ret);
}

kern_return_t
IOExtensiblePaniclog::AppendData_Invoke(const IORPC _rpc,
        OSMetaClassBase * target,
        AppendData_Handler func)
{
    IOExtensiblePaniclog_AppendData_Invocation rpc = { _rpc };
    kern_return_t ret;

    if (2 != rpc.message->mach.msgh_body.msgh_descriptor_count) return (kIOReturnIPCError);
    if (IOExtensiblePaniclog_AppendData_Msg_ObjRefs != rpc.message->content.__hdr.objectRefs) return (kIOReturnIPCError);
    if (rpc.message != NULL && rpc.sendSize < sizeof(IOExtensiblePaniclog_AppendData_Msg)) return (kIOReturnIPCError);
    if (rpc.reply != NULL && rpc.replySize < sizeof(IOExtensiblePaniclog_AppendData_Rpl)) return (kIOReturnIPCError);
    if (((OSObject *) rpc.message->content.data) != NULL && OSDynamicCast(OSData, (OSObject *) rpc.message->content.data) == NULL) { return kIOReturnBadArgument; } 

    ret = (*func)(target,
        rpc.message->content.data);

    if (kIOReturnSuccess != ret) return (ret);

    rpc.reply->content.__hdr.msgid = IOExtensiblePaniclog_AppendData_ID;
    rpc.reply->content.__hdr.flags = kIORPCMessageOneway;
    rpc.reply->mach.msgh.msgh_id   = kIORPCVersion190615Reply;
    rpc.reply->mach.msgh.msgh_size = sizeof(*rpc.reply);
    rpc.reply->mach.msgh_body.msgh_descriptor_count = 0;
    rpc.reply->content.__hdr.objectRefs = IOExtensiblePaniclog_AppendData_Rpl_ObjRefs;

    return (ret);
}

kern_return_t
IOExtensiblePaniclog::CopyMemoryDescriptor_Invoke(const IORPC _rpc,
        OSMetaClassBase * target,
        CopyMemoryDescriptor_Handler func)
{
    IOExtensiblePaniclog_CopyMemoryDescriptor_Invocation rpc = { _rpc };
    kern_return_t ret;

    if (1 != rpc.message->mach.msgh_body.msgh_descriptor_count) return (kIOReturnIPCError);
    if (IOExtensiblePaniclog_CopyMemoryDescriptor_Msg_ObjRefs != rpc.message->content.__hdr.objectRefs) return (kIOReturnIPCError);
    if (rpc.message != NULL && rpc.sendSize < sizeof(IOExtensiblePaniclog_CopyMemoryDescriptor_Msg)) return (kIOReturnIPCError);
    if (rpc.reply != NULL && rpc.replySize < sizeof(IOExtensiblePaniclog_CopyMemoryDescriptor_Rpl)) return (kIOReturnIPCError);

    ret = (*func)(target,
        (IOBufferMemoryDescriptor **)&rpc.reply->content.mem);

    if (kIOReturnSuccess != ret) return (ret);

    rpc.reply->content.__hdr.msgid = IOExtensiblePaniclog_CopyMemoryDescriptor_ID;
    rpc.reply->content.__hdr.flags = kIORPCMessageOneway;
    rpc.reply->mach.msgh.msgh_id   = kIORPCVersion190615Reply;
    rpc.reply->mach.msgh.msgh_size = sizeof(*rpc.reply);
    rpc.reply->mach.msgh_body.msgh_descriptor_count = 1;
    rpc.reply->content.__hdr.objectRefs = IOExtensiblePaniclog_CopyMemoryDescriptor_Rpl_ObjRefs;
    rpc.reply->mem__descriptor.type = MACH_MSG_PORT_DESCRIPTOR;

    return (ret);
}

kern_return_t
IOExtensiblePaniclog::SetUsedLen_Invoke(const IORPC _rpc,
        OSMetaClassBase * target,
        SetUsedLen_Handler func)
{
    IOExtensiblePaniclog_SetUsedLen_Invocation rpc = { _rpc };
    kern_return_t ret;

    if (1 != rpc.message->mach.msgh_body.msgh_descriptor_count) return (kIOReturnIPCError);
    if (IOExtensiblePaniclog_SetUsedLen_Msg_ObjRefs != rpc.message->content.__hdr.objectRefs) return (kIOReturnIPCError);
    if (rpc.message != NULL && rpc.sendSize < sizeof(IOExtensiblePaniclog_SetUsedLen_Msg)) return (kIOReturnIPCError);
    if (rpc.reply != NULL && rpc.replySize < sizeof(IOExtensiblePaniclog_SetUsedLen_Rpl)) return (kIOReturnIPCError);

    ret = (*func)(target,
        rpc.message->content.used_len);

    if (kIOReturnSuccess != ret) return (ret);

    rpc.reply->content.__hdr.msgid = IOExtensiblePaniclog_SetUsedLen_ID;
    rpc.reply->content.__hdr.flags = kIORPCMessageOneway;
    rpc.reply->mach.msgh.msgh_id   = kIORPCVersion190615Reply;
    rpc.reply->mach.msgh.msgh_size = sizeof(*rpc.reply);
    rpc.reply->mach.msgh_body.msgh_descriptor_count = 0;
    rpc.reply->content.__hdr.objectRefs = IOExtensiblePaniclog_SetUsedLen_Rpl_ObjRefs;

    return (ret);
}




