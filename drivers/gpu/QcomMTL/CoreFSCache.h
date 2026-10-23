// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>

namespace GPU { namespace CoreFSCache {
// iOS 18 C ABI, checked against 22H374 libCoreFSCache and its Metal caller.
// The payload is opaque to CoreFSCache; QMTLARC1 validation stays with QcomMTL.
struct Options {
    uint64_t capacity;
    uint16_t version=1,keyBytes=32,flags=0,permissions=0;
};
static_assert(sizeof(Options)==16 && offsetof(Options,version)==8 && offsetof(Options,keyBytes)==10,"CoreFSCache options ABI");
constexpr int Busy=206;
class Store {
    void *library_=nullptr,*cache_=nullptr;
    NSLock *lock_=[NSLock new];
    int (*open_)(const char *,const Options *,void **,const void *)=nullptr;
    int (*find_)(void *,const void *,void **)=nullptr;
    int (*insert_)(void *,const void *,const void *,size_t,void **,uintptr_t *)=nullptr;
    int (*data_)(void *,void *,const void **,size_t *)=nullptr;
    int (*sync_)(void *,void *)=nullptr;
    int (*release_)(void *,void *)=nullptr;
    int (*close_)(void *)=nullptr;
    struct Guard {NSLock *lock;Guard(NSLock *value):lock(value){[lock lock];}~Guard(){[lock unlock];}};
    template<class F> bool symbol(F &out,const char *name) {
        out=reinterpret_cast<F>(dlsym(library_,name));return out!=nullptr;
    }
public:
    Store() {
        if(NSProcessInfo.processInfo.operatingSystemVersion.majorVersion!=18)return;
        library_=dlopen("/System/Library/Frameworks/OpenGLES.framework/libCoreFSCache.dylib",RTLD_NOW|RTLD_LOCAL);
        if(library_ && !(symbol(open_,"fscache_open_with_key") && symbol(find_,"fscache_find_and_retain") &&
            symbol(insert_,"fscache_insert_and_retain") && symbol(data_,"fscache_element_get_data") &&
            symbol(sync_,"fscache_sync_element") && symbol(release_,"fscache_release") && symbol(close_,"fscache_close"))) {
            dlclose(library_);library_=nullptr;
        }
    }
    Store(const Store &)=delete;
    Store &operator=(const Store &)=delete;
    ~Store(){close();if(library_)dlclose(library_);}
    bool available() const {return library_!=nullptr;}
    int open(const char *path,const uint8_t identity[32],uint64_t capacity) {
        Guard held(lock_);if(!library_)return -1;
        if(cache_)return Busy;
        Options options{capacity};return open_(path,&options,&cache_,identity);
    }
    void close() {Guard held(lock_);if(cache_){close_(cache_);cache_=nullptr;}}
    NSData *copy(const uint8_t key[32],size_t limit) {
        Guard held(lock_);if(!cache_)return nil;
        void *element=nullptr;if(find_(cache_,key,&element) || !element)return nil;
        const void *bytes=nullptr;size_t size=0;NSData *result=nil;
        if(!data_(cache_,element,&bytes,&size) && bytes && size && size<=limit)
            result=[NSData dataWithBytes:bytes length:size];
        release_(cache_,element);
        // Only the owned copy escapes the retained element and external lock.
        return result;
    }
    bool store(const uint8_t key[32],NSData *data) {
        Guard held(lock_);if(!cache_ || !data.length)return false;
        void *element=nullptr;uintptr_t relocation=0;
        const int status=insert_(cache_,key,data.bytes,data.length,&element,&relocation);
        // An insertion can relocate the native index. No earlier element is
        // retained across calls, so the relocation needs no pointer fixups.
        if(status || !element)return false;
        const bool synced=sync_(cache_,element)==0;
        const bool released=release_(cache_,element)==0;
        return synced && released;
    }
};
} }
