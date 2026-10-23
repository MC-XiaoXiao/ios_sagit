#ifndef XNU_IIG_KERNEL_RPC_H
#define XNU_IIG_KERNEL_RPC_H
#include <DriverKit/IORPC.h>
#include <stddef.h>
#include <stdint.h>
/* Older IIG emits the user-space parser name in kernel dispatchers. Match
 * IOUserServer.cpp's descriptor walk and additionally honor rpc.sendSize.
 */
static inline IORPCMessage *
xnu_iig_message_from_mach(IORPCMessageMach *msg, bool reply, size_t available)
{
    const size_t header_size = offsetof(IORPCMessageMach, objects);
    if (!msg || available < header_size) return NULL;
    size_t size = msg->msgh.msgh_size;
    if (size < header_size || size > available ||
        msg->msgh.msgh_id != (reply ? kIORPCVersionCurrentReply : kIORPCVersionCurrent))
        return NULL;
    size_t offset = header_size;
    for (uint32_t i = 0; i < msg->msgh_body.msgh_descriptor_count; ++i) {
        if (size - offset < sizeof(mach_msg_type_descriptor_t)) return NULL;
        const mach_msg_type_descriptor_t *d =
            (const mach_msg_type_descriptor_t *)((const uint8_t *)msg + offset);
        size_t length;
        switch (d->type) {
        case MACH_MSG_PORT_DESCRIPTOR: length = sizeof(mach_msg_port_descriptor_t); break;
        case MACH_MSG_OOL_DESCRIPTOR: length = sizeof(mach_msg_ool_descriptor_t); break;
        default: return NULL;
        }
        if (length > size - offset) return NULL;
        offset += length;
    }
    if (size - offset < sizeof(IORPCMessage)) return NULL;
    return (IORPCMessage *)((uint8_t *)msg + offset);
}
#endif
