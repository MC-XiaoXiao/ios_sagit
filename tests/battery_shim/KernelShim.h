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
using IOReturn=int;using IOOptionBits=unsigned;
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
    unsigned registrations=0;
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
class IOPMPowerSource:public IOService {
protected:
    OSDictionary *properties=nullptr;
    bool settingsChangedSinceUpdate=false;
    void setPSProperty(const OSSymbol *k,OSObject *v){
        auto *old=properties->getObject(k->value.c_str());
        if(!old || !v->isEqualTo(old))settingsChangedSinceUpdate=true;
        properties->setObject(k->value.c_str(),v);
    }
    void put(const char *k,OSObject *v){auto *s=OSSymbol::withCString(k);setPSProperty(s,v);s->release();}
    void num(const char *k,uint32_t v){auto *n=OSNumber::withNumber(v,32);put(k,n);n->release();}
    void setBatteryInstalled(bool v){put("BatteryInstalled",v?kOSBooleanTrue:kOSBooleanFalse);}
    void setExternalConnected(bool v){put("ExternalConnected",v?kOSBooleanTrue:kOSBooleanFalse);}
    void setIsCharging(bool v){put("IsCharging",v?kOSBooleanTrue:kOSBooleanFalse);}
    void setCurrentCapacity(unsigned v){num("CurrentCapacity",v);}
    void setMaxCapacity(unsigned v){num("MaxCapacity",v);}
    void setVoltage(unsigned v){num("Voltage",v);}
    void setAmperage(int v){num("Amperage",uint32_t(v));}
    void setTimeRemaining(int v){num("TimeRemaining",uint32_t(v));}
    void setErrorCondition(OSSymbol *s){put("ErrorCondition",s);}
public:
    unsigned notifications=0;
    bool init() override {properties=new OSDictionary;return true;}
    virtual void updateStatus(){
        if(!settingsChangedSinceUpdate)return;
        for(auto &i:properties->values)setProperty(i.first.c_str(),i.second);
        settingsChangedSinceUpdate=false;++notifications;
    }
    void free() override {OSSafeReleaseNULL(properties);IOService::free();}
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
    int setTimeoutMS(unsigned){pending=true;return 0;}
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
        std::lock_guard<std::recursive_mutex> g(mutex);++testTime;
        if(timer && timer->pending){timer->pending=false;timer->action(timer->owner,timer);}
    }
};
inline IOReturn IOCommandGate::runAction(Action a,void *p1,void *p2,void *p3,void *p4){
    std::lock_guard<std::recursive_mutex> g(loop->mutex);return a(owner,p1,p2,p3,p4);
}
class QcomSPMI:public IOService {
public:
    std::function<IOReturn(UInt8,UInt16,UInt8 *,unsigned,bool)> transport;
    IOReturn transfer(UInt8 sid,UInt16 address,UInt8 *p,unsigned n,bool write){return transport(sid,address,p,n,write);}
};
namespace QcomPower {
inline uint32_t le32(const uint8_t *p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
}
inline void clock_get_uptime(UInt64 *p){*p=testTime;}
inline void absolutetime_to_nanoseconds(UInt64 v,UInt64 *p){*p=v;}
inline void IOLog(const char *,...){}
