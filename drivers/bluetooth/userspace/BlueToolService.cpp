// SPDX-License-Identifier: BSD-2-Clause
#include "BlueToolBootstrap.h"
#include "IOKitBluetoothTransport.h"
#include "BluetoothControl.h"
#include <dispatch/dispatch.h>
#include <xpc/xpc.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <termios.h>

// Private C SPI exported by the pinned iPhoneOS SDK's libxpc. The unified
// builder verifies this import. XPC evaluates the peer's audited identity;
// no PID-to-path lookup or client-supplied entitlement is trusted.
extern "C" xpc_object_t xpc_connection_copy_entitlement_value(xpc_connection_t, const char*);
// The public SDK annotates launchd Mach listeners unavailable to applications.
// This internal daemon uses the same exported C entry as the native BlueTool.
extern "C" xpc_connection_t NativeXPCMachService(const char*,dispatch_queue_t,uint64_t)
    __asm("_xpc_connection_create_mach_service");
using namespace Bluetooth;
using namespace Bluetooth::BlueTool;
static_assert(VMIN == 16 && sizeof(termios) == sizeof(NativeTermios), "native UART configuration ABI");

class NativeBackend {
    IOKitBluetoothTransport control;
    int descriptor = -1;
    uint64_t generation = 0;
    bool readBytes(uint8_t* data, size_t bytes, uint64_t deadline) {
        while (bytes) {
            if (!wait(POLLIN,deadline)) return false;
            const auto n=::read(descriptor,data,bytes);
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (n <= 0) return false;
            data+=n;bytes-=size_t(n);
        }
        return true;
    }
    bool wait(short events, uint64_t deadline) {
        for (;;) {
            const auto time=now();
            if (time >= deadline) return false;
            pollfd fd {descriptor,events,0};
            const auto delta=deadline-time;
            const int result=poll(&fd,1,int(delta > 100 ? 100 : delta));
            if (result < 0 && errno != EINTR) return false;
            if (result > 0) {
                if (fd.revents & (POLLERR|POLLHUP|POLLNVAL)) return false;
                if (fd.revents & events) return true;
            }
        }
    }
public:
    ~NativeBackend() { if (descriptor >= 0) ::close(descriptor); }
    static uint64_t now() {
        timespec time {};
        if (clock_gettime(CLOCK_MONOTONIC,&time)) return UINT64_MAX;
        return uint64_t(time.tv_sec)*1000+uint64_t(time.tv_nsec)/1000000;
    }
    static void pause() { usleep(10000); }
    bool connect() { return control.open(BootstrapClient) == KERN_SUCCESS; }
    bool snapshot(Snapshot& s) { return control.bootstrap(s.v) == KERN_SUCCESS && s.valid(); }
    bool power(bool on, uint64_t expected, uint64_t& accepted) {
        const bool success=control.bootstrapPower(on,expected,accepted) == KERN_SUCCESS;
        if (success) generation=accepted;
        return success;
    }
    bool open() {
        Snapshot state;
        if (!snapshot(state) || !state.ready() || (state.v[BootstrapFlags]&BootstrapPortOpen)) return false;
        generation=state.v[BootstrapGeneration];
        descriptor=socket(PF_SYSTEM,SOCK_STREAM,NativeControlProtocol);
        if (descriptor < 0) return false;
        NativeControlInfo info;
        strlcpy(info.name,NativeControlName,sizeof(info.name));
        NativeControlAddress address;
        if (ioctl(descriptor,NativeControlGetInfo,&info)) return false;
        address.id=info.id;
        if (::connect(descriptor,reinterpret_cast<sockaddr*>(&address),sizeof(address))) return false;
        NativeTermios options;
        socklen_t size=sizeof(options);
        if (getsockopt(descriptor,NativeControlProtocol,NativeGetTermios,&options,&size) || size != sizeof(options)) return false;
        options={};options.characters[16]=1; // Darwin VMIN, compile-checked below
        if (setsockopt(descriptor,NativeControlProtocol,NativeSetTermios,&options,sizeof(options))) return false;
        const int flags=fcntl(descriptor,F_GETFL,0);
        if (flags < 0 || fcntl(descriptor,F_SETFL,flags|O_NONBLOCK)) return false;
        return snapshot(state) && state.ready() && state.v[BootstrapGeneration] == generation;
    }
    bool send(const uint8_t* data, size_t bytes, uint64_t deadline) {
        while (bytes) {
            if (!wait(POLLOUT,deadline)) return false;
            const auto n=::write(descriptor,data,bytes);
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == ENOBUFS)) continue;
            if (n <= 0) return false;
            data+=n;bytes-=size_t(n);
        }
        return true;
    }
    Error receive(uint8_t* data, size_t capacity, size_t& bytes, uint64_t deadline) {
        bytes=0;
        if (capacity < 258 || !readBytes(data,3,deadline))
            return now() >= deadline ? Error::Timeout : Error::Failed;
        if (data[0] != Event || !readBytes(data+3,data[2],deadline))
            return now() >= deadline ? Error::Timeout : Error::Failed;
        bytes=3+data[2];return Error::Success;
    }
    bool close(bool success) {
        bool drained=false;
        const auto start=now();
        if (success) for (;;) {
            Snapshot state;
            if (!snapshot(state) || !state.ready() || state.v[BootstrapGeneration] != generation) break;
            if (state.v[BootstrapFlags]&BootstrapDrained) { drained=true;break; }
            if (now()-start >= 100) break;
            pause();
        }
        if (drained && setsockopt(descriptor,NativeControlProtocol,BootstrapHandoff,&generation,sizeof(generation)))
            drained=false;
        const int fd=descriptor;descriptor=-1;
        const bool closed=fd >= 0 && !::close(fd);
        if (!success || !drained) {
            uint64_t ignored=0;
            power(false,generation,ignored); // quarantine this generation only
        }
        return closed && drained;
    }
};

static bool authorized(xpc_connection_t peer) {
    auto value=xpc_connection_copy_entitlement_value(peer,"application-identifier");
    const bool identity=value && xpc_get_type(value) == XPC_TYPE_STRING &&
        !strcmp(xpc_string_get_string_ptr(value),"com.apple.bluetoothd");
    if (value) xpc_release(value);
    value=xpc_connection_copy_entitlement_value(peer,"com.apple.bluetooth.internal");
    const bool entitled=value && xpc_get_type(value) == XPC_TYPE_BOOL && xpc_bool_get_value(value);
    if (value) xpc_release(value);
    return identity && entitled;
}
static void request(xpc_connection_t peer, xpc_object_t message) {
    if (xpc_get_type(message) != XPC_TYPE_DICTIONARY) return;
    auto reply=xpc_dictionary_create_reply(message);
    if (!reply) return;
    Error result=Error::Invalid;
    Proof proof;
    if (authorized(peer)) {
        auto id=xpc_dictionary_get_value(message,MessageKey);
        auto arguments=xpc_dictionary_get_value(message,ArgumentsKey);
        if (id && xpc_get_type(id) == XPC_TYPE_STRING && !strcmp(xpc_string_get_string_ptr(id),RunBuiltin) &&
            arguments && xpc_get_type(arguments) == XPC_TYPE_DICTIONARY) {
            auto name=xpc_dictionary_get_value(arguments,"script");
            auto mac=xpc_dictionary_get_value(arguments,"btaddress");
            if (name && xpc_get_type(name) == XPC_TYPE_STRING &&
                (!mac || xpc_get_type(mac) == XPC_TYPE_STRING)) {
                NativeBackend backend;
                if (backend.connect()) {
                    Bootstrap<NativeBackend> bootstrap(backend);
                    result=bootstrap.run(script(xpc_string_get_string_ptr(name)),
                        mac ? xpc_string_get_string_ptr(mac) : nullptr);
                    proof=bootstrap.evidence();
                } else result=Error::Failed;
            }
        }
    } else result=Error::Failed;
    char firmware[128] {};
    if (proof.complete) snprintf(firmware,sizeof(firmware),"WCN3990/%08x/%08x/%04x/%04x",
        proof.soc.product,proof.soc.soc,proof.soc.rom,proof.soc.patch);
    xpc_dictionary_set_int64(reply,"result",int64_t(result));
    xpc_dictionary_set_int64(reply,"manufacturer",Vendor);
    xpc_dictionary_set_string(reply,"firmware",firmware);
    if (proof.complete) {
        xpc_dictionary_set_uint64(reply,"generation",proof.generation);
        xpc_dictionary_set_uint64(reply,"companyID",proof.company);
    }
    xpc_connection_send_message(peer,reply);
    xpc_release(reply);
}
int main(int argc, char** argv) {
    if (argc != 2 || strcmp(argv[1],"-R")) return 64;
    auto queue=dispatch_queue_create("org.darwin.bluetooth.bootstrap",DISPATCH_QUEUE_SERIAL);
    if (!queue) return 70;
    auto listener=NativeXPCMachService(Service,queue,XPC_CONNECTION_MACH_SERVICE_LISTENER);
    if (!listener) return 70;
    xpc_connection_set_event_handler(listener,^(xpc_object_t peer) {
        if (xpc_get_type(peer) != XPC_TYPE_CONNECTION) return;
        auto connection=static_cast<xpc_connection_t>(peer);
        if (!authorized(connection)) { xpc_connection_cancel(connection);return; }
        xpc_connection_set_target_queue(connection,queue);
        xpc_connection_set_event_handler(connection,^(xpc_object_t message) { request(connection,message); });
        xpc_connection_resume(connection);
    });
    xpc_connection_resume(listener);
    dispatch_main();
}
