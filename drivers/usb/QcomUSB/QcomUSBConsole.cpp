// SPDX-License-Identifier: BSD-2-Clause
#include "QcomUSBConsole.h"
#include "USBConsoleBridge.h"
#include "USBConsoleFunction.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSString.h>
#include <pexpert/pexpert.h>

OSDefineMetaClassAndStructors(QcomUSBConsole, IOService);
namespace {
constexpr UInt32 Activated=0xe3ff8110, Deactivated=0xe3ff8111, Alternate=0xe3ff8112;
constexpr unsigned BufferBytes=4096;
// Mi 6 has one device controller. Separate function instances share only port
// state, never descriptors, DMA, callbacks or controller ownership.
UInt32 portState=0;
QcomUSBConsole *dataOwner=nullptr, *controlOwner=nullptr;
bool dtr() { return (__atomic_load_n(&portState,__ATOMIC_ACQUIRE)&1)!=0; }
void terminal(bool open) {
    const auto old=__atomic_load_n(&portState,__ATOMIC_RELAXED);
    if ((old&1)!=unsigned(open)) __atomic_store_n(&portState,((old+2)&~1u)|unsigned(open),__ATOMIC_RELEASE);
}
}
struct QcomUSBConsole::State {
    QcomUSBConsole *owner;
    IOUSBDeviceInterface *interface;
    IOWorkLoop *loop;
    IOCommandGate *gate=nullptr;
    IOTimerEventSource *timer=nullptr;
    struct Channel { IOUSBDevicePipe *pipe=nullptr; IOBufferMemoryDescriptor *buffer=nullptr; bool busy=false, cancelling=false; UInt32 session=0; unsigned length=0; } channels[2];
    bool communication, gateAdded=false, timerAdded=false, handler=false, descriptorHandler=false;
    bool active=false, stopping=false, online=false, notificationPending=true, lastDTR=false;
    unsigned interfaceNumber=0, rxBytes=0, rxOffset=0, pendingTX=0;
    UInt32 rxSession=0;
    UInt32 polls=0, received=0, rejected=0;
    UInt8 lineCoding[7]={0,0xc2,1,0,0,0,8}; // 115200 8N1, host metadata only
    State(QcomUSBConsole *o,IOUSBDeviceInterface *i,IOWorkLoop *l,bool c):owner(o),interface(i),loop(l),communication(c){loop->retain();interface->service()->retain();}
    template<class F> IOReturn gated(F f) {
        return gate->runAction([](OSObject *,void *arg,void *,void *,void *)->IOReturn { return (*static_cast<F *>(arg))(); },&f);
    }
    bool submit(unsigned n,unsigned bytes) {
        auto &c=channels[n];
        if (!active || stopping || c.busy || !c.pipe || !c.buffer) return false;
        IOUSBDeviceCompletion completion{owner,QcomUSBConsole::completed,reinterpret_cast<void *>(uintptr_t(n))};
        c.busy=true; c.session=__atomic_load_n(&portState,__ATOMIC_ACQUIRE);c.length=bytes;owner->retain();
        const auto status=c.pipe->nativeIO(c.buffer,bytes,&completion,nullptr,0);
        if (status!=kIOReturnSuccess) {if(c.busy){c.busy=false;owner->release();}return false;}
        return true;
    }
    void retireReceive() {
        auto &c=channels[0];
        if(c.busy && c.session!=rxSession && !c.cancelling) {
            c.cancelling=true;
            if(c.pipe->nativeAbortGated()!=kIOReturnSuccess)c.cancelling=false;
        }
    }
    void sessionChanged() {
        const auto session=__atomic_load_n(&portState,__ATOMIC_ACQUIRE);
        if(rxSession==session) return;
        rxBytes=rxOffset=0;rxSession=session;
        const bool enabled=active && dtr();
        if(enabled!=online){ml_usb_console_set_enabled(enabled);online=enabled;}
        retireReceive();
    }
    void disconnect() {
        active=false; rxBytes=rxOffset=0;
        if (communication) {
            terminal(false);
            QcomUSBConsole::peerSessionChanged();
        }
        else { ml_usb_console_set_enabled(false); online=false; }
        for (auto &c:channels) if(c.pipe && c.busy) c.pipe->nativeAbortGated();
        notificationPending=true;
    }
    void poll() {
        if (stopping) return;
        if((++polls%100)==1) {
            UInt32 state[]={polls,UInt32(active),UInt32(dtr()),UInt32(online),rxSession,
                UInt32(channels[0].busy),channels[0].session,UInt32(channels[1].busy),
                rxBytes,rxOffset,pendingTX,received,rejected};
            owner->setProperty("USBConsoleTransportState",state,sizeof(state));
        }
        if (active) {
            const bool open=dtr();
            if (communication) {
                if(open!=lastDTR) {lastDTR=open;notificationPending=true;}
                if(notificationPending && !channels[0].busy) {
                    USBConsoleFunction::notification(static_cast<UInt8 *>(channels[0].buffer->getBytesNoCopy()),interfaceNumber,open);
                    if(submit(0,10)) notificationPending=false;
                }
            } else {
                sessionChanged();retireReceive();
                if(open!=online) {ml_usb_console_set_enabled(open);online=open;}
                if(open) {
                    if(rxOffset<rxBytes) {
                        auto *p=static_cast<UInt8 *>(channels[0].buffer->getBytesNoCopy());
                        rxOffset+=ml_usb_console_write_rx(p+rxOffset,rxBytes-rxOffset);
                    }
                    if(rxOffset==rxBytes && !channels[0].busy) {rxOffset=rxBytes=0;submit(0,BufferBytes);}
                    if(!channels[1].busy) {
                        if(!pendingTX) pendingTX=ml_usb_console_read_tx(channels[1].buffer->getBytesNoCopy(),BufferBytes);
                        if(pendingTX && submit(1,pendingTX)) pendingTX=0;
                    }
                }
            }
        }
        timer->setTimeoutMS(5);
    }
};
// Native interfaces and pipes inherit the controller's work loop. Publish and
// retire this peer only under that shared gate. The control handler cancels an
// old OUT before ACKing a DTR transition, so the next session's first command
// cannot complete into a receive request tagged with the previous generation.
QcomUSBConsole::State *QcomUSBConsole::sData=nullptr;
void QcomUSBConsole::peerSessionChanged() {
    auto *peer=sData;
    if(!peer)return;
    // Native abort may commandSleep with the recursive gate released.
    // Keep the peer alive if termination runs while its DMA drains.
    auto *owner=peer->owner;owner->retain();peer->sessionChanged();owner->release();
}
UInt32 QcomUSBConsole::descriptors(void *,UInt8 *out,UInt32 capacity,UInt32 number,
    UInt32 alt,UInt32 pipe,UInt32,void *context) {
    auto &s=*static_cast<State *>(context);
    if(alt || pipe!=UINT32_MAX) return 0;
    s.interfaceNumber=number;
    return s.communication?USBConsoleFunction::descriptors(out,capacity,number):0;
}
bool QcomUSBConsole::control(OSObject *target,const IOUSBDeviceFunctionSetup *setup,IOReturn *status,
    IOMemoryDescriptor **buffer,UInt64 *length,IOUSBDeviceControlCompletion *completion) {
    auto *self=static_cast<QcomUSBConsole *>(target);
    if(!self->fState)return false;
    bool handled=false;
    self->fState->gated([&]()->IOReturn {
        handled=controlGated(target,setup,status,buffer,length,completion);
        return kIOReturnSuccess;
    });
    return handled;
}
bool QcomUSBConsole::controlGated(OSObject *target,const IOUSBDeviceFunctionSetup *setup,IOReturn *status,
    IOMemoryDescriptor **buffer,UInt64 *length,IOUSBDeviceControlCompletion *completion) {
    auto *self=static_cast<QcomUSBConsole *>(target);
    auto *s=self->fState;
    if(!s || !s->communication || s->stopping || (setup->type&0x7f)!=0x21 || setup->index!=s->interfaceNumber) return false;
    *status=kIOReturnUnsupported; *buffer=nullptr; *length=0; *completion={};
    const bool in=(setup->type&0x80)!=0;
    if(setup->request==0x22 && !in && !setup->length && !(setup->value&~3u)) {
        self->setProperty("USBConsoleLineState",setup->value,32);
        terminal(setup->value&1);
        peerSessionChanged();
        *status=kIOReturnSuccess; return true;
    }
    if(setup->request==0x23 && !in && !setup->length) { *status=kIOReturnSuccess; return true; }
    if((setup->request!=0x20 || in) && (setup->request!=0x21 || !in)) return true;
    if(setup->value || setup->length!=7) return true;
    auto *data=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,kIODirectionInOut,7,0);
    if(!data) {*status=kIOReturnNoMemory;return true;}
    memcpy(data->getBytesNoCopy(),s->lineCoding,7);
    *buffer=data; *length=7; *status=kIOReturnSuccess;
    *completion={self,controlCompleted,reinterpret_cast<void *>(uintptr_t(setup->request))};
    return true;
}
void QcomUSBConsole::controlCompleted(OSObject *target,void *parameter,IOReturn status,UInt64 bytes,IOMemoryDescriptor *data) {
    auto *self=static_cast<QcomUSBConsole *>(target);
    if(!self->fState || status || uintptr_t(parameter)!=0x20 || bytes!=7 || !data) return;
    UInt8 line[7];
    if(data->readBytes(0,line,7)==7 && USBConsoleFunction::validLineCoding(line,7)) {
        self->fState->gated([&]()->IOReturn {memcpy(self->fState->lineCoding,line,7);return kIOReturnSuccess;});
    }
}
void QcomUSBConsole::completed(void *target,void *parameter,IOReturn status,UInt32 bytes) {
    auto *self=static_cast<QcomUSBConsole *>(target);
    auto &s=*self->fState;
    const unsigned n=unsigned(uintptr_t(parameter));
    s.gated([&]()->IOReturn {
        s.channels[n].busy=s.channels[n].cancelling=false;
        if(!s.communication && !n) {
            UInt32 state[]={UInt32(status),bytes,s.channels[n].session,
                __atomic_load_n(&portState,__ATOMIC_ACQUIRE),s.rxSession};
            self->setProperty("USBConsoleLastReceive",state,sizeof(state));
            if(!status) {
                if(s.active && bytes<=BufferBytes && s.channels[n].session==__atomic_load_n(&portState,__ATOMIC_ACQUIRE))s.received+=bytes;
                else s.rejected+=bytes;
            }
        }
        if(!s.communication && !n && !status && s.active && bytes<=BufferBytes &&
            s.channels[n].session==__atomic_load_n(&portState,__ATOMIC_ACQUIRE)) {s.rxBytes=bytes;s.rxOffset=0;}
        if(!s.communication && n && bytes<s.channels[n].length) {
            auto *p=static_cast<UInt8 *>(s.channels[n].buffer->getBytesNoCopy());
            s.pendingTX=s.channels[n].length-bytes;memmove(p,p+bytes,s.pendingTX);
        }
        if(status && status!=kIOReturnAborted && s.active && !s.stopping) {
            self->setProperty("USBConsoleLastError",UInt32(status),32);
        }
        return kIOReturnSuccess;
    });
    self->release();
}
void QcomUSBConsole::timer(OSObject *owner,IOTimerEventSource *) {
    auto *self=static_cast<QcomUSBConsole *>(owner);
    self->fState->gated([&]()->IOReturn {self->fState->poll();return kIOReturnSuccess;});
}
IOWorkLoop *QcomUSBConsole::getWorkLoop() const {return fState?fState->loop:IOService::getWorkLoop();}
bool QcomUSBConsole::start(IOService *provider) {
    int mode=0;PE_parse_boot_argn("qcom.usb-cdc",&mode,sizeof(mode));
    auto *interface=IOUSBDeviceInterface::fromObject(provider);
    auto *function=OSDynamicCast(OSString,provider->getProperty("USBDeviceFunction"));
    const bool communication=function && function->isEqualTo("Mi6CDCControl");
    if(mode!=2 || !interface || !function || (!communication && !function->isEqualTo("Mi6CDCData")) || fState) return false;
    auto **slot=communication?&controlOwner:&dataOwner;
    QcomUSBConsole *expected=nullptr;
    if(!__atomic_compare_exchange_n(slot,&expected,this,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) return false;
    if(!IOService::start(provider)) {__atomic_store_n(slot,static_cast<QcomUSBConsole *>(nullptr),__ATOMIC_RELEASE);return false;}
    fStarted=true;
    auto *loop=provider->getWorkLoop();
    if(!loop) goto fail;
    fState=new State(this,interface,loop,communication);
    if(!fState) goto fail;
    {
        auto &s=*fState;
        s.gate=IOCommandGate::commandGate(this);s.timer=IOTimerEventSource::timerEventSource(this,timer);
        if(!s.gate || !s.timer || loop->addEventSource(s.gate)!=kIOReturnSuccess) goto fail;
        s.gateAdded=true;
        if(loop->addEventSource(s.timer)!=kIOReturnSuccess) goto fail;
        s.timerAdded=true;
        for(unsigned n=0;n<(communication?1u:2u);++n) {
            s.channels[n].buffer=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,kIODirectionInOut,BufferBytes,64);
            if(!s.channels[n].buffer) goto fail;
        }
        if(interface->nativeAddAlternateSetting(communication?2:10,communication?2:0,communication?1:0)!=0) goto fail;
        s.channels[0].pipe=interface->nativeCreatePipe(communication?3:2,communication?1:0,communication?16:-1,communication?16:0,0,0);
        if(!s.channels[0].pipe) goto fail;
        if(!communication) {s.channels[1].pipe=interface->nativeCreatePipe(2,1,-1,0,0,0);if(!s.channels[1].pipe)goto fail;}
        if(interface->nativeSetDescription(communication?"Mi 6 USB console control":"Mi 6 USB console data")) goto fail;
        if(interface->nativeSetDescriptorHandler(descriptors,&s)) goto fail;
        s.descriptorHandler=true;
        if(communication) {if(interface->nativeSetControlHandler(this,control))goto fail;s.handler=true;}
        if(s.gated([&]()->IOReturn {
            if(!communication)sData=&s;
            return interface->nativeFinalize();
        })) goto fail;
        s.timer->setTimeoutMS(5);
        setProperty("USBConsoleComposite",true);
        IOLog("QcomUSBConsole: native CDC %s function ready\n",communication?"control":"data");
        registerService(); return true;
    }
fail:
    stop(provider);
    // stop can run before State was allocated.
    if(!fState) __atomic_store_n(slot,static_cast<QcomUSBConsole *>(nullptr),__ATOMIC_RELEASE);
    return false;
}
IOReturn QcomUSBConsole::message(UInt32 type,IOService *provider,void *argument) {
    if(!fState || (type!=Activated && type!=Deactivated && type!=Alternate)) return IOService::message(type,provider,argument);
    auto &s=*fState;
    return s.gated([&]()->IOReturn {
        if(type==Deactivated) s.disconnect();
        else if(type==Activated && !s.stopping) {s.active=true;s.notificationPending=true;}
        else if(type==Alternate && (!argument || *static_cast<UInt32 *>(argument))) return kIOReturnUnsupported;
        return kIOReturnSuccess;
    });
}
void QcomUSBConsole::stop(IOService *provider) {
    if(fState && !fState->stopping) {
        auto &s=*fState;
        auto shutdown=[&]()->IOReturn {
            if(sData==&s)sData=nullptr;
            s.stopping=true;s.disconnect();if(s.timer)s.timer->cancelTimeout();
            if(s.handler){s.interface->nativeClearControlHandler(this);s.handler=false;}
            if(s.descriptorHandler){s.interface->nativeSetDescriptorHandler(nullptr,nullptr);s.descriptorHandler=false;}
            return kIOReturnSuccess;
        };
        if(s.gateAdded)s.gated(shutdown);else shutdown();
        if(s.timerAdded){s.loop->removeEventSource(s.timer);s.timerAdded=false;}
        // Keep the gate until free: aborted native callbacks may still arrive.
        auto **slot=s.communication?&controlOwner:&dataOwner;
        __atomic_store_n(slot,static_cast<QcomUSBConsole *>(nullptr),__ATOMIC_RELEASE);
    }
    if(fStarted){fStarted=false;IOService::stop(provider);}
}
void QcomUSBConsole::free() {
    if(fState) {
        auto &s=*fState;
        for(auto &c:s.channels) OSSafeReleaseNULL(c.buffer);
        if(s.gateAdded)s.loop->removeEventSource(s.gate);
        OSSafeReleaseNULL(s.timer);OSSafeReleaseNULL(s.gate);s.loop->release();
        s.interface->service()->release();delete fState;fState=nullptr;
    }
    IOService::free();
}
