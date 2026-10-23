// Read-only 22H374 UI patch verification. No remote execution or memory writes.
#include <Foundation/Foundation.h>
#include <UIKit/UIKit.h>
#include <CoreGraphics/CoreGraphics.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <mach/mach.h>
#include <mach-o/loader.h>
#include <dlfcn.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

@interface OBDevice : NSObject
+ (id)currentDevice;
- (NSUInteger)type;
- (NSUInteger)templateType;
@end
@interface PUIProgressWindow : NSObject
- (id)initWithOptions:(NSUInteger)options contextLevel:(float)level appearance:(NSInteger)appearance;
- (void)setVisible:(BOOL)visible;
- (NSString *)errorDescription;
@end

using ReadMemory = kern_return_t (*)(mach_port_t, uint64_t, uint64_t, uint64_t, uint64_t *);
static bool readBytes(ReadMemory read, mach_port_t task, uint64_t address, void *bytes, size_t size) {
    uint64_t count = 0;
    return address && read && !read(task, address, size, reinterpret_cast<uint64_t>(bytes), &count) && count == size;
}

static bool frameworkUUID(ReadMemory read, mach_port_t task, uint64_t base) {
    mach_header_64 header{};
    if (!readBytes(read, task, base, &header, sizeof(header)) || header.magic != MH_MAGIC_64 ||
        header.ncmds > 128 || header.sizeofcmds > 16384) return false;
    uint8_t commands[16384]{};
    if (!readBytes(read, task, base+sizeof(header), commands, header.sizeofcmds)) return false;
    const uint8_t expected[]={0xc0,0xc3,0x89,0x2b,0x9f,0x15,0x38,0xc8,0xba,0xed,0x74,0x01,0xcd,0xe5,0xb7,0x67};
    size_t pos=0;
    for (unsigned i=0; i<header.ncmds; ++i) {
        if (pos+sizeof(load_command)>header.sizeofcmds) return false;
        load_command command{};memcpy(&command,commands+pos,sizeof(command));
        if (command.cmdsize<sizeof(command) || pos+command.cmdsize>header.sizeofcmds) return false;
        if (command.cmd==LC_UUID && command.cmdsize==sizeof(uuid_command))
            return !memcmp(commands+pos+8,expected,sizeof(expected));
        pos+=command.cmdsize;
    }
    return false;
}

static ptrdiff_t scalarOffset(Class cls, const char *name) {
    Ivar ivar=class_getInstanceVariable(cls,name);
    return ivar?ivar_getOffset(ivar):-1;
}

static int onboarding() {
    void *library=dlopen("/System/Library/PrivateFrameworks/OnBoardingKit.framework/OnBoardingKit",RTLD_NOW);
    Class cls=NSClassFromString(@"OBDevice");
    Method method=cls?class_getClassMethod(cls,sel_registerName("currentDevice")):nullptr;
    Dl_info info{};
    if (!library || !method || !dladdr(reinterpret_cast<void *>(method_getImplementation(method)),&info)) return 1;
    auto read=reinterpret_cast<ReadMemory>(dlsym(RTLD_DEFAULT,"mach_vm_read_overwrite"));
    uint64_t slide=reinterpret_cast<uint64_t>(info.dli_fbase)-0x19b938000ULL;
    if (!frameworkUUID(read,mach_task_self(),0x19b938000ULL+slide)) return 1;
    ptrdiff_t typeOffset=scalarOffset(cls,"_type"),templateOffset=scalarOffset(cls,"_templateType");
    if (typeOffset<0 || typeOffset>256 || templateOffset<0 || templateOffset>256) return 1;
    OBDevice *device=[cls currentDevice];
    CGRect bounds=[UIScreen mainScreen].bounds;
    printf("UI_PATCH_LOCAL type=%lu template=%lu bounds=%.6gx%.6g scale=%.6g type_offset=%td template_offset=%td isolated_process=1\n",
        (unsigned long)[device type],(unsigned long)[device templateType],bounds.size.width,bounds.size.height,
        [UIScreen mainScreen].scale,typeOffset,templateOffset);
    auto list=reinterpret_cast<int (*)(uint32_t,uint32_t,void *,int)>(dlsym(RTLD_DEFAULT,"proc_listpids"));
    auto name=reinterpret_cast<int (*)(int,void *,uint32_t)>(dlsym(RTLD_DEFAULT,"proc_name"));
    auto readTask=reinterpret_cast<kern_return_t (*)(mach_port_t,int,mach_port_t *)>(dlsym(RTLD_DEFAULT,"task_read_for_pid"));
    if (!list || !name || !readTask) return 1;
    int pids[4096]{};int bytes=list(1,0,pids,sizeof(pids));
    if (bytes<0 || bytes>int(sizeof(pids)) || bytes%sizeof(pids[0])) return 1;
    bool setupVerified=false;
    for (int i=0;i<bytes/int(sizeof(pids[0]));++i) {
        char process[64]{};
        if (pids[i]<=0 || name(pids[i],process,sizeof(process)-1)<=0 || strcmp(process,"Setup")) continue;
        mach_port_t task=MACH_PORT_NULL;
        kern_return_t status=readTask(mach_task_self(),pids[i],&task);
        printf("UI_PATCH_REMOTE name=%s pid=%d task_read_status=0x%x\n",process,pids[i],status);
        if (status || !task) continue;
        uint64_t object=0,type=0,templateType=0;uint32_t hook=0;
        bool valid=frameworkUUID(read,task,0x19b938000ULL+slide) &&
            readBytes(read,task,0x19b94f4f4ULL+slide,&hook,sizeof(hook)) &&
            readBytes(read,task,0x1f2a77158ULL+slide,&object,sizeof(object)) && object &&
            readBytes(read,task,object+typeOffset,&type,sizeof(type)) &&
            readBytes(read,task,object+templateOffset,&templateType,sizeof(templateType));
        double height=0;
        if (valid && templateType>=1 && templateType<=10)
            valid=readBytes(read,task,0x19b9757a8ULL+slide+(templateType-1)*sizeof(double),&height,sizeof(height));
        printf("UI_PATCH_SETUP pid=%d valid=%u native_uuid_verified=%u hook=0x%08x object=0x%llx type=%llu template=%llu icon_height=%.6g optical_verified=0\n",
            pids[i],valid,valid,hook,(unsigned long long)object,(unsigned long long)type,(unsigned long long)templateType,height);
        setupVerified=valid && hook==0x140110e3 && type==1 && templateType==8 && height==80;
        mach_port_deallocate(mach_task_self(),task);
    }
    return setupVerified?0:2;
}

static int logo() {
    void *library=dlopen("/System/Library/PrivateFrameworks/ProgressUI.framework/ProgressUI",RTLD_NOW);
    Class cls=NSClassFromString(@"PUIProgressWindow");
    if (!library || !cls) return 1;
    Ivar logo=class_getInstanceVariable(cls,"_appleLogo");
    if (!logo || ivar_getOffset(logo)!=0x88) return 1;
    // Same options as the backboardd overlay: no progress bar or owned context.
    PUIProgressWindow *window=[[cls alloc] initWithOptions:0 contextLevel:0 appearance:0];
    if (!window) return 1;
    [window setVisible:YES];
    uintptr_t pointer=0;
    memcpy(&pointer,reinterpret_cast<const uint8_t *>((__bridge const void *)window)+ivar_getOffset(logo),sizeof(pointer));
    CGImageRef image=reinterpret_cast<CGImageRef>(pointer);
    size_t width=image?CGImageGetWidth(image):0,height=image?CGImageGetHeight(image):0;
    printf("UI_PATCH_LOGO image_nonnull=%u width=%zu height=%zu error=%s separate_context=0 optical_verified=0\n",
        image!=nullptr,width,height,[window errorDescription].UTF8String?:"(none)");
    return image && width==600 && height==600?0:1;
}

int main(int argc,char **argv) {
    if (argc!=2 || geteuid()!=0) return 2;
    signal(SIGALRM,+[](int){_exit(124);});alarm(20);
    @autoreleasepool {
        @try {
            if (!strcmp(argv[1],"onboarding")) return onboarding();
            if (!strcmp(argv[1],"logo")) return logo();
        } @catch (NSException *exception) {
            printf("UI_PATCH_EXCEPTION name=%s reason=%s\n",exception.name.UTF8String,exception.reason.UTF8String);
            return 1;
        }
    }
    return 2;
}
