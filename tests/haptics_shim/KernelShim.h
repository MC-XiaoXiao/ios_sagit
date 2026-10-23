// SPDX-License-Identifier: BSD-2-Clause
// Host execution of the actual IOKit adapter. ABI is checked separately by
// compiling the same .cpp against the real XNU SDK and linking its Kext.
#pragma once
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdarg>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>
using UInt8=uint8_t;using UInt16=uint16_t;using UInt32=uint32_t;using UInt64=uint64_t;
using IOReturn=int;using IOOptionBits=unsigned;using task_t=uintptr_t;
class IOUserClient;
inline bool administrator=true, failTimer=false;
inline unsigned gateDepth=0;
constexpr int kIOReturnBadArgument=-3,kIOReturnNotReady=-4,kIOReturnBusy=-5,kIOReturnUnsupported=-6,kIOReturnIOError=-7,kIOReturnOffline=-8,kIOReturnExclusiveAccess=-9,kIOReturnNotPermitted=-10,kIOReturnNotPrivileged=-11,kIOReturnNotAttached=-12;
constexpr uintptr_t MACH_PORT_NULL=0;
constexpr unsigned kIOExternalMethodArgumentsCurrentVersion=2;
constexpr auto kIOClientPrivilegeAdministrator="administrator";
constexpr int kIOReturnSuccess=0,kIOReturnNoMemory=-1,kIOPMNoSuchState=-2,IOPMAckImplied=0;
constexpr unsigned kIOPMDeviceUsable=1,kIOPMPowerOn=2;
inline int liveObjects=0, failEventAdd=0, eventAdds=0;
inline bool failPM=false;
inline UInt64 testTime=100;
#define APPLE_KEXT_OVERRIDE override
#define OSDeclareDefaultStructors(T)
#define OSDefineMetaClassAndStructors(T,B)
#define OSDynamicCast(T,p) dynamic_cast<T *>(p)
#define OSSafeReleaseNULL(p) do { if(p) { (p)->release(); (p)=nullptr; } } while(false)
class OSObject {
    mutable unsigned refs=1;
public:
    OSObject(){++liveObjects;}
    virtual ~OSObject(){--liveObjects;}
    virtual bool init(){return true;}
    virtual void free(){}
    virtual bool isEqualTo(const OSObject *o) const {return this==o;}
    void retain() const {++refs;}
    void release() const {assert(refs);if(!--refs){auto *p=const_cast<OSObject *>(this);p->free();delete p;}}
};
class OSSymbol:public OSObject {
public:
    std::string value;
    static const OSSymbol *withCString(const char *p){auto *s=new OSSymbol;s->value=p;return s;}
    __attribute__((noinline)) bool isEqualTo(const OSObject *o) const override {
        auto *s=dynamic_cast<const OSSymbol *>(o);return s && s->value==value;
    }
};
class OSNumber:public OSObject {
public:
    UInt64 value=0;
    static OSNumber *withNumber(UInt64 v,unsigned){auto *n=new OSNumber;n->value=v;return n;}
    __attribute__((noinline)) bool isEqualTo(const OSObject *o) const override {
        auto *n=dynamic_cast<const OSNumber *>(o);return n && n->value==value;
    }
};
class OSBoolean:public OSObject {};
inline OSBoolean trueObject,falseObject;
inline auto *kOSBooleanTrue=&trueObject,*kOSBooleanFalse=&falseObject;
class OSDictionary:public OSObject {
public:
    std::map<std::string,OSObject *> values;
    OSObject *getObject(const char *k){auto i=values.find(k);return i==values.end()?nullptr:i->second;}
    void setObject(const char *k,OSObject *v){v->retain();removeObject(k);values[k]=v;}
    void removeObject(const char *k){auto i=values.find(k);if(i!=values.end()){i->second->release();values.erase(i);}}
    void free() override {for(auto &i:values)i.second->release();values.clear();}
};
class OSData:public OSObject {
public:
    std::vector<uint8_t> bytes;
    unsigned getLength() const{return bytes.size();}
    const void *getBytesNoCopy() const{return bytes.data();}
};
class IOWorkLoop;
struct IOPMPowerState {unsigned version=0,capabilityFlags=0,outputPowerCharacter=0,inputPowerRequirement=0;};
class IOService:public OSObject {
    IOService *provider=nullptr;
public:
    OSDictionary registry;
    std::vector<IOService *> children;
    unsigned registrations=0;bool inactive=false;
    bool isInactive() const{return inactive;}
    bool terminate(){inactive=true;return true;}
    virtual IOReturn newUserClient(task_t,void *,UInt32,OSDictionary *,IOUserClient **){return kIOReturnUnsupported;}
    virtual bool start(IOService *){return true;}
    virtual void stop(IOService *){}
    virtual bool requestTerminate(IOService *,IOOptionBits){return true;}
    virtual IOWorkLoop *getWorkLoop() const{return nullptr;}
    virtual IOReturn setPowerState(unsigned long,IOService *){return 0;}
    bool attach(IOService *p){provider=p;p->children.push_back(this);return true;}
    void detach(IOService *p){
        for(auto i=p->children.begin();i!=p->children.end();++i)if(*i==this){p->children.erase(i);break;}
        provider=nullptr;
    }
    IOService *getProvider(){return provider;}
    void registerService(){++registrations;}
    OSObject *getProperty(const char *key){return registry.getObject(key);}
    bool setProperty(const char *k,OSObject *v){registry.setObject(k,v);return true;}
    bool setProperty(const char *k,bool v){return setProperty(k,v?kOSBooleanTrue:kOSBooleanFalse);}
    bool setProperty(const char *k,UInt64 v,unsigned bits){
        auto *n=OSNumber::withNumber(v,bits);setProperty(k,n);n->release();return true;
    }
    bool setProperty(const char *k,const char *v){
        auto *s=OSSymbol::withCString(v);setProperty(k,const_cast<OSSymbol *>(s));s->release();return true;
    }
    void removeProperty(const char *k){registry.removeObject(k);}
    void PMinit(){}
    void PMstop(){}
    void joinPMtree(IOService *){}
    int registerPowerDriver(IOService *,IOPMPowerState *,unsigned){return failPM?-1:0;}
    void changePowerStateTo(unsigned n){setPowerState(n,this);}
    void free() override {assert(children.empty());registry.free();}
};
class Event:public OSObject {
public:
    OSObject *owner=nullptr;
    IOWorkLoop *loop=nullptr;
    void free() override {owner->release();}
};
class IOTimerEventSource:public Event {
public:
    using Action=void(*)(OSObject *,IOTimerEventSource *);
    Action action=nullptr;bool pending=false;
    static IOTimerEventSource *timerEventSource(OSObject *o,Action a){
        auto *t=new IOTimerEventSource;t->owner=o;o->retain();t->action=a;return t;
    }
    int setTimeoutMS(unsigned){if(failTimer)return -77;pending=true;return 0;}
    void cancelTimeout(){pending=false;}
};
class IOCommandGate:public Event {
public:
    using Action=IOReturn(*)(OSObject *,void *,void *,void *,void *);
    static IOCommandGate *commandGate(OSObject *o){auto *g=new IOCommandGate;g->owner=o;o->retain();return g;}
    IOReturn runAction(Action,void * =nullptr,void * =nullptr,void * =nullptr,void * =nullptr);
};
class IOWorkLoop:public OSObject {
public:
    std::recursive_mutex mutex;
    IOTimerEventSource *timer=nullptr;
    static IOWorkLoop *workLoop(){return new IOWorkLoop;}
    int addEventSource(Event *e){
        if(++eventAdds==failEventAdd)return -1;
        e->retain();e->loop=this;
        if(auto *t=dynamic_cast<IOTimerEventSource *>(e))timer=t;
        return 0;
    }
    void removeEventSource(Event *e){if(e==timer)timer=nullptr;e->loop=nullptr;e->release();}
    void fire(){
        std::lock_guard<std::recursive_mutex> g(mutex);++gateDepth;
        if(timer && timer->pending){timer->pending=false;timer->action(timer->owner,timer);}
        --gateDepth;
    }
};
inline IOReturn IOCommandGate::runAction(Action a,void *p1,void *p2,void *p3,void *p4){
    std::lock_guard<std::recursive_mutex> g(loop->mutex);++gateDepth;auto r=a(owner,p1,p2,p3,p4);--gateDepth;return r;
}
class QcomSPMI:public IOService {
public:
    IOWorkLoop *busLoop=IOWorkLoop::workLoop();
    IOWorkLoop *getWorkLoop() const override{return busLoop;}
    void free() override{busLoop->release();IOService::free();}
    std::function<IOReturn(UInt8,UInt16,UInt8 *,unsigned,bool)> transport;
    IOReturn transfer(UInt8 sid,UInt16 address,UInt8 *p,unsigned n,bool write){return transport(sid,address,p,n,write);}
};
namespace QcomPower {
inline uint32_t le32(const uint8_t *p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
}
inline void clock_get_uptime(UInt64 *p){*p=testTime;}
inline void absolutetime_to_nanoseconds(UInt64 v,UInt64 *p){*p=v;}
inline void IOLog(const char *,...){}

struct IOExternalMethodDispatch {};
struct IOExternalMethodArguments {
    unsigned version=kIOExternalMethodArgumentsCurrentVersion,selector=0;
    const uint64_t *scalarInput=nullptr;
    uint64_t *scalarOutput=nullptr;
    unsigned scalarInputCount=0,scalarOutputCount=0;
    unsigned structureInputSize=0,structureOutputSize=0,structureOutputDescriptorSize=0;
    void *structureInputDescriptor=nullptr,*structureOutputDescriptor=nullptr,*structureVariableOutputData=nullptr;
    uintptr_t asyncWakePort=0;unsigned asyncReferenceCount=0;
};
class IOUserClient:public IOService {
public:
    static IOReturn clientHasPrivilege(task_t,const char *){return administrator?0:kIOReturnNotPrivileged;}
    virtual bool initWithTask(task_t,void *,UInt32,OSDictionary *){return true;}
    virtual IOReturn clientClose(){return 0;}
    virtual IOReturn externalMethod(UInt32,IOExternalMethodArguments *,IOExternalMethodDispatch *,OSObject *,void *){return kIOReturnUnsupported;}
};
inline bool ml_at_interrupt_context(){return false;}
