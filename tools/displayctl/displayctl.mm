// SPDX-License-Identifier: BSD-2-Clause
// Display/process diagnostics and explicit development Data initialization.
#include <Foundation/Foundation.h>
#include <QuartzCore/CALayer.h>
#include <QuartzCore/CAAnimation.h>
#include <QuartzCore/CADisplayLink.h>
#include <QuartzCore/CARenderer.h>
#include <QuartzCore/CAMetalLayer.h>
#include <QuartzCore/CAMediaTiming.h>
#include <QuartzCore/CATransaction.h>
#include <Metal/Metal.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/CGImageDestination.h>
#include <IOSurface/IOSurfaceRef.h>
#include <IOKitUser.h>
#include <DisplayFrameStatistics.h>
#include <DisplayLeaseHistory.h>
#include <DisplayTimeline.h>
#include <algorithm>
#include <vector>
#include <objc/message.h>
#include <objc/runtime.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/event.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <math.h>

// Original 22H374 ProgressUI declarations; the implementation is loaded from
// the firmware. Keep float arguments in the native SIMD argument registers.
@interface PUIProgressWindow : NSObject
- (instancetype)initWithOptions:(NSUInteger)options contextLevel:(float)level appearance:(NSInteger)appearance;
- (void)setProgressValue:(float)value;
- (void)setVisible:(BOOL)visible;
@property(readonly) NSString *errorDescription;
@property(readonly) id layer;
@end

@interface DisplayClockObserver : NSObject
@property NSUInteger phase;
@property NSUInteger callbacks;
@property NSUInteger invalidTimes;
@property CFTimeInterval firstTimestamp;
@property CFTimeInterval lastTimestamp;
- (void)tick:(CADisplayLink *)link;
@end
@implementation DisplayClockObserver
- (void)tick:(CADisplayLink *)link {
    CFTimeInterval timestamp = link.timestamp, target = link.targetTimestamp, duration = link.duration;
    if (!isfinite(timestamp) || !isfinite(target) || !isfinite(duration) ||
            timestamp <= 0 || target <= timestamp || duration <= 0 ||
            (_callbacks && timestamp <= _lastTimestamp)) ++_invalidTimes;
    if (!_callbacks) _firstTimestamp = timestamp;
    _lastTimestamp = timestamp;
    ++_callbacks;
    if (_callbacks <= 6)
        printf("DISPLAY_LINK_TICK phase=%lu index=%lu timestamp=%.9f target=%.9f duration=%.9f\n",
            (unsigned long)_phase, (unsigned long)_callbacks, timestamp, target, duration);
}
@end

static int displayLinkClock() {
    // Use QuartzCore's native display clock. A software timer would conceal
    // a missing kernel-to-compositor VBL notification path.
    if (!pthread_main_np() || setenv("CA_SYSTEM_SESSION_CLIENT", "1", 1)) return 1;
    struct Deadline {
        void (*previous)(int);
        Deadline():previous(signal(SIGALRM, +[](int) {
            static constexpr char message[] = "DISPLAY_LINK_TIMEOUT seconds=10\n";
            (void)write(STDERR_FILENO, message, sizeof(message) - 1); _exit(124);
        })) { alarm(10); }
        ~Deadline() { alarm(0); if (previous != SIG_ERR) signal(SIGALRM, previous); }
    } bounded;
    int result = 0;
    for (NSUInteger phase = 0; phase < 2; ++phase) {
        DisplayClockObserver *observer = [DisplayClockObserver new]; observer.phase = phase;
        CADisplayLink *link = nil;
        printf("DISPLAY_LINK_BEGIN phase=%lu request=%s duration_seconds=1.5 software_tick_source=0\n",
            (unsigned long)phase, phase ? "hello-range-80-120-preferred120" : "fixed60");
        @try {
            link = [CADisplayLink displayLinkWithTarget:observer selector:@selector(tick:)];
            if (!link) { puts("DISPLAY_LINK unavailable"); result = 1; continue; }
            // This firmware's SBDashBoardSetupView requests 80/120/120.
            // Let native QuartzCore clamp it to the actual display capability.
            if (phase) link.preferredFrameRateRange = CAFrameRateRangeMake(80, 120, 120);
            else link.preferredFramesPerSecond = 60;
            [link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
            NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:1.5];
            while (deadline.timeIntervalSinceNow > 0)
                [[NSRunLoop mainRunLoop] runUntilDate:deadline];
            CFTimeInterval span = observer.lastTimestamp - observer.firstTimestamp;
            printf("DISPLAY_LINK_END phase=%lu callbacks=%lu invalid_times=%lu first=%.9f last=%.9f observed_hz=%.3f rendering_unverified=1\n",
                (unsigned long)phase, (unsigned long)observer.callbacks, (unsigned long)observer.invalidTimes,
                observer.firstTimestamp, observer.lastTimestamp,
                observer.callbacks > 1 && span > 0 ? (observer.callbacks - 1) / span : 0);
            if (observer.callbacks < 3 || observer.invalidTimes) result = 1;
        } @catch (NSException *error) {
            printf("DISPLAY_LINK_EXCEPTION phase=%lu %s %s\n", (unsigned long)phase,
                error.name.UTF8String, error.reason.UTF8String); result = 1;
        } @finally { [link invalidate]; }
    }
    return result;
}

// Read-only selector and reply types verified against this firmware's
// KBXPCProtocol. The service supplies the state and any native NSError.
@protocol KeybagStateDiagnostics
- (void)getDeviceLockState:(int)user needsExtended:(BOOL)extended
                withReply:(void (^)(unsigned int state, NSError *error))reply;
@end

// Native QuartzCore SPI. Validate the loaded image and selector encodings
// before creating a temporary diagnostic context in the foreground session.
@interface CAContext : NSObject
+ (instancetype)remoteContextWithOptions:(NSDictionary *)options;
@property(strong) CALayer *layer;
@property float level;
@property(getter=isSecure) BOOL secure;
- (BOOL)waitForRenderingWithTimeout:(double)timeout;
- (void)invalidate;
@end

// Apple liblaunch C ABI (launch.h); SDK libSystem still exports these on iOS.
extern "C" {
typedef struct _launch_data *launch_data_t;
launch_data_t launch_data_alloc(int);
launch_data_t launch_data_new_string(const char *);
bool launch_data_dict_insert(launch_data_t, launch_data_t, const char *);
launch_data_t launch_data_dict_lookup(launch_data_t, const char *);
int launch_data_get_type(launch_data_t);
long long launch_data_get_integer(launch_data_t);
int launch_data_get_errno(launch_data_t);
void launch_data_free(launch_data_t);
launch_data_t launch_msg(launch_data_t);
}
static int jobPid(const char *label) {
    launch_data_t request = launch_data_alloc(1);
    if (!request || !launch_data_dict_insert(request, launch_data_new_string(label), "GetJob")) return 0;
    launch_data_t response = launch_msg(request); launch_data_free(request);
    auto value = response && launch_data_get_type(response) == 1 ? launch_data_dict_lookup(response, "PID") : nullptr;
    int pid = value && launch_data_get_type(value) == 4 ? (int)launch_data_get_integer(value) : 0;
    if (response) launch_data_free(response);
    return pid;
}

static int initializePersona() {
    // iOS 18 UMDPersonaManifestEmbedded v1 metadata for an empty, unencrypted
    // development Data volume. This contains no keybag or unlocked-key state.
    auto nativeGetuid = reinterpret_cast<uid_t (*)()>(dlsym(RTLD_DEFAULT, "getuid"));
    auto nativeMkdir = reinterpret_cast<int (*)(const char *, mode_t)>(dlsym(RTLD_DEFAULT, "mkdir"));
    auto nativeOpen = reinterpret_cast<int (*)(const char *, int, ...)>(dlsym(RTLD_DEFAULT, "open"));
    auto nativeOpenat = reinterpret_cast<int (*)(int, const char *, int, ...)>(dlsym(RTLD_DEFAULT, "openat"));
    auto nativeFstat = reinterpret_cast<int (*)(int, struct stat *)>(dlsym(RTLD_DEFAULT, "fstat"));
    auto nativeWrite = reinterpret_cast<ssize_t (*)(int, const void *, size_t)>(dlsym(RTLD_DEFAULT, "write"));
    auto nativeSync = reinterpret_cast<int (*)(int)>(dlsym(RTLD_DEFAULT, "fsync"));
    auto nativeClose = reinterpret_cast<int (*)(int)>(dlsym(RTLD_DEFAULT, "close"));
    auto nativeUnlinkat = reinterpret_cast<int (*)(int, const char *, int)>(dlsym(RTLD_DEFAULT, "unlinkat"));
    if (!nativeGetuid || !nativeMkdir || !nativeOpen || !nativeOpenat || !nativeFstat || !nativeWrite || !nativeSync || !nativeClose || !nativeUnlinkat || nativeGetuid()) return 1;
    NSString *xml = @"<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>UsePersonaManifestVersion</key><integer>1</integer><key>UsePersonaGenerationID</key><integer>1</integer><key>UserPersonaDictionary</key><dict/></dict></plist>";
    NSError *error = nil;
    Class serializer = NSClassFromString(@"NSPropertyListSerialization");
    id manifest = [serializer propertyListWithData:[xml dataUsingEncoding:NSUTF8StringEncoding] options:0 format:nullptr error:&error];
    NSData *data = manifest ? [serializer dataWithPropertyList:manifest format:NSPropertyListBinaryFormat_v1_0 options:0 error:&error] : nil;
    if (!data) { printf("PERSONA_INIT serialization=%s\n", error.description.UTF8String); return 1; }
    const char *path = "/private/var/keybags";
    if (nativeMkdir(path, 0700) && errno != EEXIST) { printf("PERSONA_INIT mkdir errno=%d\n", errno); return 1; }
    int directory = nativeOpen(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (directory < 0) { printf("PERSONA_INIT directory errno=%d\n", errno); return 1; }
    struct stat info = {};
    if (nativeFstat(directory, &info) || info.st_uid || !S_ISDIR(info.st_mode) || (info.st_mode & 0022)) { nativeClose(directory); return 1; }
    int file = nativeOpenat(directory, "persona.kb", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (file < 0) { printf("PERSONA_INIT create errno=%d\n", errno); nativeClose(directory); return 1; }
    NSUInteger written = 0;
    while (written < data.length) {
        ssize_t count = nativeWrite(file, static_cast<const char *>(data.bytes) + written, data.length - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        written += count;
    }
    int result = written != data.length || nativeSync(file);
    int saved = errno;
    nativeClose(file);
    if (result) nativeUnlinkat(directory, "persona.kb", 0);
    nativeSync(directory); nativeClose(directory);
    printf("PERSONA_INIT result=%d errno=%d bytes=%lu path=/private/var/keybags/persona.kb\n", result, result ? saved : 0, (unsigned long)written);
    return result;
}

static int containerQueryState;
static int containerQueryResult = 1;
static void *containerQueryWorker(void *) {
    // Mirror this firmware's _LSDefaults userContainerURL query. Use the native
    // library and its permissions; the returned object belongs to the query.
    void *library = dlopen("/usr/lib/system/libsystem_containermanager.dylib", RTLD_LAZY);
    auto create = reinterpret_cast<void *(*)()>(library ? dlsym(library, "container_query_create") : nullptr);
    auto setClass = reinterpret_cast<void (*)(void *, uint64_t)>(library ? dlsym(library, "container_query_set_class") : nullptr);
    auto setPersona = reinterpret_cast<void (*)(void *, const char *)>(library ? dlsym(library, "container_query_set_persona_unique_string") : nullptr);
    auto setFlags = reinterpret_cast<void (*)(void *, uint64_t)>(library ? dlsym(library, "container_query_operation_set_flags") : nullptr);
    auto getResult = reinterpret_cast<void *(*)(void *)>(library ? dlsym(library, "container_query_get_single_result") : nullptr);
    auto getError = reinterpret_cast<void *(*)(void *)>(library ? dlsym(library, "container_query_get_last_error") : nullptr);
    auto describe = reinterpret_cast<char *(*)(void *)>(library ? dlsym(library, "container_error_copy_unlocalized_description") : nullptr);
    auto getPath = reinterpret_cast<const char *(*)(void *)>(library ? dlsym(library, "container_object_get_path") : nullptr);
    auto release = reinterpret_cast<void (*)(void *)>(library ? dlsym(library, "container_query_free") : nullptr);
    auto nativeFree = reinterpret_cast<void (*)(void *)>(dlsym(RTLD_DEFAULT, "free"));
    auto primary = reinterpret_cast<const char **>(library ? dlsym(library, "CONTAINER_PERSONA_PRIMARY") : nullptr);
    if (!create || !setClass || !setPersona || !setFlags || !getResult || !getError || !describe || !getPath || !release || !nativeFree || !primary || !*primary) {
        puts("CONTAINER_QUERY native APIs unavailable");
    } else if (void *query = create()) {
        setClass(query, 10);
        setPersona(query, *primary);
        setFlags(query, 0x900000001ULL);
        printf("CONTAINER_QUERY_BEGIN class=10 persona=%s flags=0x900000001\n", *primary);
        void *object = getResult(query);
        const char *path = object ? getPath(object) : nullptr;
        char *error = describe(getError(query));
        printf("CONTAINER_QUERY_RESULT object=%p path=%s error=%s\n", object,
            path ? path : "(none)", error ? error : "(none)");
        nativeFree(error);
        containerQueryResult = path ? 0 : 1;
        release(query);
    }
    // Keep the library loaded while native objects or TLS may still refer to it.
    __atomic_store_n(&containerQueryState, 2, __ATOMIC_RELEASE);
    return nullptr;
}

static int rebootDevice() {
    auto nativeSync = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "sync"));
    auto nativeReboot = reinterpret_cast<int (*)(int)>(dlsym(RTLD_DEFAULT, "reboot"));
    if (!nativeSync || !nativeReboot) return 1;
    puts("DISPLAY_REBOOT requesting native orderly reboot");
    nativeSync();
    int result = nativeReboot(0); // RB_AUTOBOOT; retain native authorization/sync.
    printf("DISPLAY_REBOOT result=%d errno=%d\n", result, errno);
    return 1;
}

static int displayIdentity() {
    void *security = dlopen("/System/Library/Frameworks/Security.framework/Security", RTLD_LAZY);
    auto create = reinterpret_cast<CFTypeRef (*)(CFAllocatorRef)>(security ? dlsym(security, "SecTaskCreateFromSelf") : nullptr);
    auto answer = reinterpret_cast<CFTypeRef (*)(CFTypeRef, CFStringRef, CFErrorRef *)>(security ? dlsym(security, "SecTaskCopyValueForEntitlement") : nullptr);
    if (!create || !answer) { puts("DISPLAY_IDENTITY native Security APIs unavailable"); return 1; }
    CFTypeRef task = create(nullptr);
    if (!task) return 1;
    for (NSString *key in @[@"com.apple.QuartzCore.displayable-context", @"com.apple.QuartzCore.secure-mode", @"com.apple.QuartzCore.display-state"]) {
        CFErrorRef error = nullptr;
        CFTypeRef value = answer(task, (__bridge CFStringRef)key, &error);
        printf("DISPLAY_ENTITLEMENT %s=%s error=%s\n", key.UTF8String,
            value ? [[(__bridge id)value description] UTF8String] : "(none)",
            error ? [[(__bridge id)error description] UTF8String] : "(none)");
        if (value) CFRelease(value);
        if (error) CFRelease(error);
    }
    CFRelease(task);
    return 0;
}

static int displayIdentities() {
    void *security = dlopen("/System/Library/Frameworks/Security.framework/Security", RTLD_LAZY);
    auto create = reinterpret_cast<CFTypeRef (*)(CFAllocatorRef, audit_token_t)>(security ? dlsym(security, "SecTaskCreateWithAuditToken") : nullptr);
    auto answer = reinterpret_cast<CFTypeRef (*)(CFTypeRef, CFStringRef, CFErrorRef *)>(security ? dlsym(security, "SecTaskCopyValueForEntitlement") : nullptr);
    auto list = reinterpret_cast<int (*)(uint32_t, uint32_t, void *, int)>(dlsym(RTLD_DEFAULT, "proc_listpids"));
    auto name = reinterpret_cast<int (*)(int, void *, uint32_t)>(dlsym(RTLD_DEFAULT, "proc_name"));
    auto taskName = reinterpret_cast<kern_return_t (*)(mach_port_name_t, int, mach_port_name_t *)>(dlsym(RTLD_DEFAULT, "task_name_for_pid"));
    if (!create || !answer || !list || !name || !taskName) return 1;
    int pids[4096] = {};
    int bytes = list(1, 0, pids, sizeof(pids)); // Native PROC_ALL_PIDS, bounded.
    if (bytes < 0 || bytes > int(sizeof(pids)) || bytes % sizeof(pids[0])) return 1;
    for (int i = 0; i < bytes / int(sizeof(pids[0])); ++i) {
        char process[64] = {};
        if (pids[i] <= 0 || name(pids[i], process, sizeof(process) - 1) <= 0) continue;
        if (strcmp(process, "PreBoard") && strcmp(process, "backboardd") &&
                strcmp(process, "SpringBoard") && strcmp(process, "com.apple.datamigrator")) continue;
        mach_port_name_t port = MACH_PORT_NULL;
        kern_return_t result = taskName(mach_task_self(), pids[i], &port);
        audit_token_t token = {};
        mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
        if (!result) result = task_info(port, TASK_AUDIT_TOKEN, reinterpret_cast<task_info_t>(&token), &count);
        if (port) mach_port_deallocate(mach_task_self(), port);
        printf("DISPLAY_PROCESS_IDENTITY name=%s pid=%d audit_result=0x%x\n", process, pids[i], result);
        if (result) continue;
        CFTypeRef task = create(nullptr, token);
        if (!task) continue;
        for (NSString *key in @[@"com.apple.QuartzCore.displayable-context", @"com.apple.QuartzCore.secure-mode", @"com.apple.QuartzCore.display-state"]) {
            CFErrorRef error = nullptr;
            CFTypeRef value = answer(task, (__bridge CFStringRef)key, &error);
            printf("DISPLAY_PROCESS_ENTITLEMENT name=%s pid=%d %s=%s error=%s\n", process, pids[i], key.UTF8String,
                value ? [[(__bridge id)value description] UTF8String] : "(none)",
                error ? [[(__bridge id)error description] UTF8String] : "(none)");
            if (value) CFRelease(value);
            if (error) CFRelease(error);
        }
        CFRelease(task);
    }
    return 0;
}

static int prepareExecutable() {
    // One-time bootstrap from the loadable builtin to the independently signed
    // executable. Native APFS performs the rename/mode update; USB later replaces
    // only this preallocated diagnostic file's owned data extents.
    auto nativeGetuid = reinterpret_cast<uid_t (*)()>(dlsym(RTLD_DEFAULT, "getuid"));
    auto nativeSync = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "sync"));
    if (!nativeGetuid || nativeGetuid() || !nativeSync) return 1;
    NSString *source = @"/private/var/displayctl.dylib";
    NSString *destination = @"/private/var/displayctl";
    NSFileManager *files = [NSFileManager defaultManager];
    NSError *error = nil;
    NSDictionary *attributes = [files attributesOfItemAtPath:source error:&error];
    if (![attributes[NSFileType] isEqualToString:NSFileTypeRegular] ||
            [attributes[NSFileSize] unsignedLongLongValue] != 131072 ||
            [attributes[NSFileOwnerAccountID] unsignedLongLongValue] != 0 ||
            [files fileExistsAtPath:destination]) {
        puts("DISPLAY_PREPARE unexpected source or existing destination; retained"); return 1;
    }
    if (![files moveItemAtPath:source toPath:destination error:&error]) {
        printf("DISPLAY_PREPARE rename failed=%s\n", error.description.UTF8String); return 1;
    }
    if (![files setAttributes:@{NSFilePosixPermissions: @0700} ofItemAtPath:destination error:&error]) {
        printf("DISPLAY_PREPARE mode failed=%s\n", error.description.UTF8String);
        NSError *rollback = nil;
        if (![files moveItemAtPath:destination toPath:source error:&rollback])
            printf("DISPLAY_PREPARE rollback failed=%s\n", rollback.description.UTF8String);
        return 1;
    }
    nativeSync();
    puts("DISPLAY_PREPARE renamed=/private/var/displayctl mode=0700; executable upload required before use");
    return 0;
}

static int stacks(bool selfOnly = false, bool allTasks = false, int selectedPID = 0) {
    // Native sys/stackshot.h ABI. The libsyscall wrapper supplies output pointers.
    struct Config {
        int pid;
        uint64_t flags, timestamp, buffer;
        uint32_t size;
        uint64_t outputBuffer, outputSize, pagetableMask;
    };
    static_assert(sizeof(Config) == 64, "native stackshot configuration ABI");
    auto capture = reinterpret_cast<int (*)(Config *)>(dlsym(RTLD_DEFAULT, "stackshot_capture_with_config"));
    auto dealloc = reinterpret_cast<int (*)(Config *)>(dlsym(RTLD_DEFAULT, "stackshot_config_dealloc_buffer"));
    if (!capture || !dealloc) { puts("DISPLAY_STACKSHOT native APIs unavailable"); return 1; }
    const char *labels[] = {"com.apple.containermanagerd", "com.apple.containermanagerd.system",
        "com.apple.mobile.usermanagerd", "com.apple.mobile.keybagd",
        "com.apple.cfprefsd.xpc.daemon", "com.apple.backboardd",
        "com.apple.datamigrator", "com.apple.migrationpluginwrapper",
        "com.apple.SpringBoard", "com.apple.PreBoard",
        "com.apple.lsd", "com.apple.runningboardd", "self", "system"};
    auto nativeGetpid = reinterpret_cast<int (*)()>(dlsym(RTLD_DEFAULT, "getpid"));
    for (const char *label : labels) {
        bool current = !strcmp(label, "self");
        bool system = !strcmp(label, "system");
        if (selectedPID > 0 && !current) continue;
        if (allTasks != system) continue;
        if (selfOnly && !current) continue;
        Config config = {};
        config.pid = selectedPID > 0 ? selectedPID : system ? -1 : current ? (nativeGetpid ? nativeGetpid() : 0) : jobPid(label);
        if (!system && config.pid <= 0) continue;
        // GET_DQ | SAVE_LOADINFO | KCDATA_FORMAT | THREAD_WAITINFO.
        config.flags = 0x01 | 0x02 | 0x10000 | 0x1000000;
        int result = capture(&config);
        printf("DISPLAY_STACKSHOT label=%s pid=%d result=%d bytes=%u\n", selectedPID > 0 ? "selected" : label, config.pid, result, config.size);
        uint32_t limit = system ? 4 * 1024 * 1024 : 256 * 1024;
        if (!result && config.buffer && config.size <= limit) {
            const auto *bytes = reinterpret_cast<const unsigned char *>(config.buffer);
            static const char hex[] = "0123456789abcdef";
            for (uint32_t offset = 0; offset < config.size; offset += 64) {
                char line[129];
                unsigned count = 0;
                for (uint32_t i = offset; i < config.size && i < offset + 64; ++i) {
                    line[count++] = hex[bytes[i] >> 4]; line[count++] = hex[bytes[i] & 15];
                }
                line[count] = 0;
                printf("DISPLAY_STACKSHOT_DATA pid=%d offset=%u data=%s\n", config.pid, offset, line);
            }
        } else if (!result && config.size > limit) {
            printf("DISPLAY_STACKSHOT_OMITTED pid=%d bytes=%u limit=%u\n", config.pid, config.size, limit);
        }
        bool exported = !result && config.buffer && config.size <= limit;
        if (config.buffer) dealloc(&config);
        printf("DISPLAY_STACKSHOT_END pid=%d\n", config.pid);
        if (selectedPID > 0) return exported ? 0 : 1;
    }
    return 0;
}
static int jobs() {
    const char *labels[] = {"com.apple.backboardd", "com.apple.SpringBoard", "com.apple.watchdogd",
        "com.apple.containermanagerd", "com.apple.containermanagerd.system", "com.apple.cfprefsd.xpc.daemon",
        "com.apple.mobile.usermanagerd", "com.apple.mobile.keybagd",
        "com.apple.datamigrator", "com.apple.migrationpluginwrapper", "com.apple.PreBoard",
        "com.apple.lsd", "com.apple.runningboardd", "com.apple.fairplayd.A2",
        "com.apple.fairplaydeviceidentityd", "com.apple.appstored", "com.apple.accountsd",
        "com.apple.installd", "com.apple.installcoordinationd"};
    for (const char *label : labels) {
        launch_data_t request = launch_data_alloc(1);
        if (!request || !launch_data_dict_insert(request,launch_data_new_string(label),"GetJob")) return 1;
        launch_data_t response = launch_msg(request); launch_data_free(request);
        printf("DISPLAY_JOB label=%s type=%d",label,response ? launch_data_get_type(response) : 0);
        if (response && launch_data_get_type(response) == 1) {
            const char *keys[] = {"PID", "LastExitStatus"};
            for (const char *key : keys) {
                auto value = launch_data_dict_lookup(response, key);
                if (!value) continue;
                if (launch_data_get_type(value) == 4) printf(" %s=%lld",key,launch_data_get_integer(value));
            }
        } else if (response && launch_data_get_type(response) == 9) printf(" errno=%d",launch_data_get_errno(response));
        puts("");
        if (response) launch_data_free(response); else printf("DISPLAY_JOB_ERROR errno=%d\n",errno);
    }
    return 0;
}

static uint32_t chosenDisplayScale() {
    auto fromPath = reinterpret_cast<mach_port_t (*)(mach_port_t, const char *)>(dlsym(RTLD_DEFAULT, "IORegistryEntryFromPath"));
    mach_port_t chosen = fromPath ? fromPath(0, "IODeviceTree:/chosen") : 0;
    uint32_t scale = 0;
    printf("DISPLAY_CHOSEN entry=0x%x\n", chosen);
    if (chosen) {
        for (NSString *key in @[@"display-scale", @"display-rotation"]) {
            CFTypeRef value = IORegistryEntryCreateCFProperty(chosen, (__bridge CFStringRef)key, nullptr, 0);
            printf("DISPLAY_CHOSEN_VALUE %s=%s\n", key.UTF8String,
                value ? [[(__bridge id)value description] UTF8String] : "(none)");
            if (value && [key isEqualToString:@"display-scale"] && CFGetTypeID(value) == CFDataGetTypeID() && CFDataGetLength((CFDataRef)value) == sizeof(scale))
                memcpy(&scale, CFDataGetBytePtr((CFDataRef)value), sizeof(scale));
            if (value) CFRelease(value);
        }
        IOObjectRelease(chosen);
    }
    return scale;
}
static bool pinnedImage(const void *base, const uint8_t (&uuid)[16]) {
    auto header = static_cast<const mach_header_64 *>(base);
    if (!header || header->magic != MH_MAGIC_64 || header->sizeofcmds > 65536) return false;
    auto command = reinterpret_cast<const uint8_t *>(header + 1);
    const uint8_t *end = command + header->sizeofcmds;
    for (uint32_t i = 0; i < header->ncmds && size_t(end - command) >= sizeof(load_command); ++i) {
        auto load = reinterpret_cast<const load_command *>(command);
        if (load->cmdsize < sizeof(*load) || load->cmdsize > size_t(end - command)) return false;
        if (load->cmd == LC_UUID && load->cmdsize >= sizeof(uuid_command))
            return !memcmp(reinterpret_cast<const uuid_command *>(command)->uuid, uuid, sizeof(uuid));
        command += load->cmdsize;
    }
    return false;
}
static int displayAPIs() {
    // Resolve original exports without calling an unaudited private ABI.
    // Offsets and image UUID checks permit offline inspection of this build.
    const char *paths[] = {
        "/System/Library/PrivateFrameworks/IOMobileFramebuffer.framework/IOMobileFramebuffer",
        "/System/Library/PrivateFrameworks/BackBoardServices.framework/BackBoardServices",
        "/System/Library/Frameworks/QuartzCore.framework/QuartzCore",
        "/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices"};
    const uint8_t uuids[][16] = {
        {0xe2,0x31,0x18,0x84,0x56,0xcd,0x33,0x79,0x88,0x2d,0xc8,0xc6,0xa6,0xcd,0x0a,0x98},
        {0x3a,0x43,0x8d,0x91,0x42,0xc7,0x3a,0x13,0xb8,0xa7,0x8a,0xef,0x17,0xb6,0x97,0xc7},
        {0xfa,0xf7,0xd7,0xea,0x18,0xf0,0x3c,0x2d,0x81,0x6c,0xac,0x56,0xd1,0x6c,0x40,0xa3},
        {0xa6,0xaa,0x78,0xc3,0x67,0xeb,0x37,0x6b,0xa3,0x8a,0x44,0x99,0x5f,0x7e,0x38,0xa0}};
    const char *names[][12] = {
        {"IOMobileFramebufferGetMainDisplay", "IOMobileFramebufferGetLayerDefaultSurface",
         "IOMobileFramebufferSwapBegin", "IOMobileFramebufferSwapSetLayer", "IOMobileFramebufferSwapEnd",
         "IOMobileFramebufferSwapWaitWithTimeout", "IOMobileFramebufferGetDisplayState",
         "IOMobileFramebufferSetDisplayState", "IOMobileFramebufferGetCanvasSizes"},
        {"BKSDisplayServicesGetScreenBlanked", "BKSDisplayServicesIsScreenBlanked",
         "BKSDisplayServicesSetScreenBlanked", "BKSDisplayServicesGetScreenDimmed",
         "BKSDisplayServicesSetScreenDimmed", "BKSDisplayServicesGetMainScreenInfo"},
        {"CARenderServerRenderDisplay", "CARenderServerGetStatistics", "CARenderServerGetDebugInfo", "CARenderServerGetInfo"},
        {"SBSUndimScreen", "SBSGetScreenLockStatus", "SBSLockDevice"}};
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        void *library = dlopen(paths[i], RTLD_LAZY);
        for (auto name : names[i]) {
            if (!name) break;
            void *symbol = library ? dlsym(library, name) : nullptr;
            Dl_info info = {};
            bool found = symbol && dladdr(symbol, &info) && info.dli_fbase;
            printf("DISPLAY_NATIVE_API name=%s found=%u pinned=%u offset=0x%llx base=%p image=%s\n",
                name, found, found && pinnedImage(info.dli_fbase, uuids[i]),
                found ? (unsigned long long)(reinterpret_cast<uintptr_t>(symbol) - reinterpret_cast<uintptr_t>(info.dli_fbase)) : 0,
                found ? info.dli_fbase : nullptr, found && info.dli_fname ? info.dli_fname : "(none)");
        }
    }
    return 0;
}
static int renderInfo() {
    if (setenv("CA_SYSTEM_SESSION_CLIENT", "1", 1)) return 1;
    void *library = dlopen("/System/Library/Frameworks/QuartzCore.framework/QuartzCore", RTLD_LAZY);
    void *anchor = library ? dlsym(library, "CARenderServerGetStatistics") : nullptr;
    const uint8_t uuid[] = {0xfa,0xf7,0xd7,0xea,0x18,0xf0,0x3c,0x2d,0x81,0x6c,0xac,0x56,0xd1,0x6c,0x40,0xa3};
    Dl_info info = {};
    // Audited native getter: returns a retained CF property list decoded from
    // the server's OOL reply. Refuse a different implementation or ABI.
    if (!anchor || !dladdr(anchor, &info) || !pinnedImage(info.dli_fbase, uuid) ||
            reinterpret_cast<uintptr_t>(anchor) - reinterpret_cast<uintptr_t>(info.dli_fbase) != 0x238588) {
        puts("RENDER_INFO native getter ABI unavailable"); return 1;
    }
    // This is the property-list getter called by the original statistics API;
    // the exported CARenderServerGetInfo has a different, three-argument ABI.
    auto query = reinterpret_cast<CFTypeRef (*)(mach_port_t)>(reinterpret_cast<uintptr_t>(info.dli_fbase) + 0x88088);
    CFTypeRef value = query(0);
    printf("RENDER_INFO %s\n", value ? [(__bridge id)value description].UTF8String : "(none)");
    if (value) CFRelease(value);
    return value ? 0 : 1;
}
static int renderInfoState, renderInfoResult=1;
static void *renderInfoWorker(void *) {
    @autoreleasepool { renderInfoResult=renderInfo(); }
    __atomic_store_n(&renderInfoState,2,__ATOMIC_RELEASE);
    return nullptr;
}
static void describeNativeObject(const char *label, id object, NSArray<NSString *> *keys) {
    printf("DISPLAY_OBJECT %s=%s\n", label, object ? [[object description] UTF8String] : "(none)");
    for (NSString *key in keys) {
        if (![object respondsToSelector:NSSelectorFromString(key)]) continue;
        @try {
            printf("DISPLAY_OBJECT_VALUE %s.%s=%s\n", label, key.UTF8String,
                [[[object valueForKey:key] description] UTF8String] ?: "(none)");
        } @catch (NSException *error) {
            printf("DISPLAY_OBJECT_ERROR %s.%s=%s\n", label, key.UTF8String, error.reason.UTF8String);
        }
    }
}
static id activeDisplayControl;
static int uiWakeState;
static int uiWakeResult=1;
static void *uiWakeWorker(void *) {
    @autoreleasepool {
        void *library=dlopen("/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices",RTLD_LAZY);
        auto wake=reinterpret_cast<void (*)(void)>(library?dlsym(library,"SBSUndimScreen"):nullptr);
        Dl_info image={};
        const uint8_t uuid[]={0xa6,0xaa,0x78,0xc3,0x67,0xeb,0x37,0x6b,0xa3,0x8a,0x44,0x99,0x5f,0x7e,0x38,0xa0};
        bool pinned=wake && dladdr(reinterpret_cast<void *>(wake),&image) && pinnedImage(image.dli_fbase,uuid) &&
            reinterpret_cast<uintptr_t>(wake)-reinterpret_cast<uintptr_t>(image.dli_fbase)==0x12704;
        printf("NATIVE_UI_WAKE api=SBSUndimScreen pinned=%u offset=0x%llx\n",pinned,
            pinned?(unsigned long long)(reinterpret_cast<uintptr_t>(wake)-reinterpret_cast<uintptr_t>(image.dli_fbase)):0);
        if(pinned) {
            // Original 0x18ed13704 consumes no arguments, obtains the native
            // service port and sends message 0x186ab with a zero byte. Audited
            // against this exact UUID; a transport return is not UI activity.
            // Ask the native UI policy owner to wake. Merely taking a
            // CADisplayStateControl on-lease does not establish UI activity.
            wake();uiWakeResult=0;
            puts("NATIVE_UI_WAKE request_returned=1 presentation_verified=0");
        }
    }
    __atomic_store_n(&uiWakeState,2,__ATOMIC_RELEASE);return nullptr;
}
static int displayState(bool turnOn = false, bool retainObserver = false) {
    // Native display discovery can wait behind an unresponsive backboardd.
    // Bound this diagnostic's IPC without changing the system service.
    struct Deadline {
        void (*previous)(int);
        Deadline():previous(signal(SIGALRM,+[](int) {
            static constexpr char message[]="DISPLAY_STATE_TIMEOUT seconds=10\n";
            (void)write(STDERR_FILENO,message,sizeof(message)-1);_exit(124);
        })) { alarm(10); }
        ~Deadline(){alarm(0);if(previous!=SIG_ERR)signal(SIGALRM,previous);}
    } deadline;
    if (setenv("CA_SYSTEM_SESSION_CLIENT", "1", 1)) return 1;
    Class cls = NSClassFromString(@"CADisplay");
    SEL main = sel_registerName("mainDisplay"), control = sel_registerName("stateControl");
    Method factory = class_getClassMethod(cls, main);
    Method getter = class_getInstanceMethod(cls, control);
    Dl_info image = {};
    const uint8_t uuid[] = {0xfa,0xf7,0xd7,0xea,0x18,0xf0,0x3c,0x2d,0x81,0x6c,0xac,0x56,0xd1,0x6c,0x40,0xa3};
    if (!factory || !getter || strcmp(method_getTypeEncoding(factory), "@16@0:8") ||
            strcmp(method_getTypeEncoding(getter), "@16@0:8") ||
            !dladdr(reinterpret_cast<const void *>(method_getImplementation(getter)), &image) || !pinnedImage(image.dli_fbase, uuid)) {
        puts("DISPLAY_STATE native image/ABI unavailable"); return 1;
    }
    @try {
        SEL all = sel_registerName("displays");
        if ([cls respondsToSelector:all]) reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(cls, all);
        id display = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(cls, main);
        id state = turnOn && activeDisplayControl ? activeDisplayControl :
            reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(display, control);
        printf("DISPLAY_STATE_OBJECT display=%s control=%s\n", display ? [[display description] UTF8String] : "nil",
            state ? object_getClassName(state) : "nil");
        NSArray *keys = @[@"displayState", @"powerState", @"targetDisplayState", @"targetPowerState"];
        for (NSString *key in keys) {
            Method member = class_getInstanceMethod(object_getClass(state), NSSelectorFromString(key));
            if (!member || strcmp(method_getTypeEncoding(member), "q16@0:8")) {
                printf("DISPLAY_STATE getter unavailable=%s\n", key.UTF8String); return 1;
            }
        }
        puts("DISPLAY_STATE_ENUM display_off=0 display_on=1 flipbook=2 suppressed=3; power_values_raw=1");
        describeNativeObject("state-control", state, keys);
        if(retainObserver) activeDisplayControl=state;
        if (turnOn) {
            SEL transition = sel_registerName("transitionToDisplayState:withCompletion:");
            Method member = class_getInstanceMethod(object_getClass(state), transition);
            Dl_info origin = {};
            if (!member || strcmp(method_getTypeEncoding(member), "v32@0:8q16@?24") ||
                    !dladdr(reinterpret_cast<const void *>(method_getImplementation(member)), &origin) ||
                    !pinnedImage(origin.dli_fbase, uuid) ||
                    reinterpret_cast<uintptr_t>(method_getImplementation(member)) - reinterpret_cast<uintptr_t>(origin.dli_fbase) != 0x270944) {
                puts("DISPLAY_STATE native transition ABI unavailable"); return 1;
            }
            // Original enum table maps 1 to on. Let the native render server
            // authorize and execute the transition, including IOMFB policy.
            activeDisplayControl = state;
            reinterpret_cast<void (*)(id, SEL, NSInteger, id)>(objc_msgSend)(state, transition, 1, nil);
            puts("DISPLAY_STATE on requested through native QuartzCore; completion not assumed");
            bool reached = false;
            for (unsigned attempt = 0; attempt < 100; ++attempt) {
                [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
                auto read = reinterpret_cast<NSInteger (*)(id, SEL)>(objc_msgSend);
                reached = read(state, sel_registerName("displayState")) == 1 && read(state, sel_registerName("powerState")) == 1 &&
                    read(state, sel_registerName("targetDisplayState")) == 1 && read(state, sel_registerName("targetPowerState")) == 1;
                if (reached) break;
            }
            printf("DISPLAY_STATE request_reached_on=%u optical_verification=0\n", reached);
            describeNativeObject("state-control-after-request", state, keys);
        }
        return 0;
    } @catch (NSException *error) {
        printf("DISPLAY_STATE_EXCEPTION %s %s\n", error.name.UTF8String, error.reason.UTF8String); return 1;
    }
}
static int frameStatistics(bool raw);
static int sampleDisplayPower(unsigned seconds, bool requestOn) {
    if(!seconds || seconds>(requestOn?60u:180u))return 2;
    // Apple's init obtains shared display state; the four getters only read
    // that mapping. Observation never calls transitionToDisplayState: or
    // creates a power assertion. Retain the observer for the bounded sample.
    if(displayState(requestOn,!requestOn) || !activeDisplayControl)return 1;
    struct Deadline {
        void (*previous)(int);
        Deadline(unsigned seconds):previous(signal(SIGALRM,+[](int) {
            static constexpr char message[]="DISPLAY_POWER_TIMEOUT\n";
            (void)write(STDERR_FILENO,message,sizeof(message)-1);_exit(124);
        })) {alarm(seconds+10);}
        ~Deadline(){alarm(0);if(previous!=SIG_ERR)signal(SIGALRM,previous);}
    } deadline(seconds);
    struct Sample {uint64_t absolute,continuous;NSInteger state[4];};
    std::vector<Sample> samples;samples.reserve(seconds*10+4);
    const SEL getters[]={sel_registerName("displayState"),sel_registerName("powerState"),
        sel_registerName("targetDisplayState"),sel_registerName("targetPowerState")};
    mach_timebase_info_data_t timebase{};
    if(mach_timebase_info(&timebase)!=KERN_SUCCESS || !timebase.numer || !timebase.denom)return 1;
    const double begin=CACurrentMediaTime(),end=begin+seconds;
    printf("DISPLAY_POWER_READY seconds=%u absolute=%llu continuous=%llu numer=%u denom=%u sampled_hz=10 requested_on=%u optical_verified=0\n",
        seconds,mach_absolute_time(),mach_continuous_time(),timebase.numer,timebase.denom,requestOn);fflush(stdout);
    bool allOn=true;
    @try {
        do {
            Sample sample{mach_absolute_time(),mach_continuous_time(),{}};
            for(unsigned i=0;i<4;++i) {
                sample.state[i]=reinterpret_cast<NSInteger (*)(id,SEL)>(objc_msgSend)(activeDisplayControl,getters[i]);
                allOn&=sample.state[i]==1;
            }
            samples.push_back(sample);
            const double remaining=end-CACurrentMediaTime();if(remaining<=0)break;
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:std::min(0.1,remaining)]];
        } while(samples.size()<seconds*10+4);
    } @catch(NSException *error) {
        printf("DISPLAY_POWER_EXCEPTION %s %s\n",error.name.UTF8String,error.reason.UTF8String);return 1;
    }
    // Bulk output after the measured period. Keep UART printing out of the
    // active interval and retain every sample, including off/unknown states.
    const double elapsed=CACurrentMediaTime()-begin;
    const bool complete=elapsed>=seconds-0.05 && elapsed<=seconds+2;
    for(const auto &s:samples)printf("DISPLAY_POWER_SAMPLE absolute=%llu continuous=%llu display=%ld power=%ld target_display=%ld target_power=%ld\n",
        s.absolute,s.continuous,(long)s.state[0],(long)s.state[1],(long)s.state[2],(long)s.state[3]);
    printf("DISPLAY_POWER_END samples=%lu all_sampled_on=%u elapsed=%.9f duration_complete=%u requested_on=%u optical_verified=0\n",
        (unsigned long)samples.size(),allOn,elapsed,complete,requestOn);
    activeDisplayControl=nil;
    return complete && (!requestOn || allOn)?0:1;
}
static int layerTest(bool animate=false) {
    // Native display discovery/context IPC can wait beyond the framework's
    // rendering timeout. Bound this diagnostic process, including its 60 s
    // presentation window, so one stalled service cannot hold a UART batch.
    struct Deadline {
        void (*previous)(int);
        Deadline():previous(signal(SIGALRM, +[](int) {
            static constexpr char message[]="LAYER_TEST_TIMEOUT seconds=180\n";
            (void)write(STDERR_FILENO,message,sizeof(message)-1);_exit(124);
        })) { alarm(180); }
        ~Deadline() { alarm(0);if(previous!=SIG_ERR)signal(SIGALRM,previous); }
    } deadline;
    puts("LAYER_TEST_DEADLINE seconds=180");fflush(stdout);
    auto nativeSetenv = reinterpret_cast<int (*)(const char *, const char *, int)>(dlsym(RTLD_DEFAULT, "setenv"));
    auto nativeIsMain = reinterpret_cast<int (*)()>(dlsym(RTLD_DEFAULT, "pthread_main_np"));
    if (!nativeSetenv || !nativeIsMain || !nativeIsMain() || nativeSetenv("CA_SYSTEM_SESSION_CLIENT", "1", 1)) return 1;
    Class cls = NSClassFromString(@"CAContext");
    SEL factory = sel_registerName("remoteContextWithOptions:");
    Method method = class_getClassMethod(cls, factory);
    Dl_info image = {};
    const uint8_t uuid[] = {0xfa,0xf7,0xd7,0xea,0x18,0xf0,0x3c,0x2d,0x81,0x6c,0xac,0x56,0xd1,0x6c,0x40,0xa3};
    if (!method || strcmp(method_getTypeEncoding(method), "@24@0:8@16") ||
            !dladdr(reinterpret_cast<const void *>(method_getImplementation(method)), &image) || !pinnedImage(image.dli_fbase, uuid)) {
        puts("LAYER_TEST native context image/factory unavailable"); return 1;
    }
    const char *selectors[] = {"setLayer:", "setLevel:", "setSecure:", "invalidate", "waitForRenderingWithTimeout:"};
    const char *encodings[] = {"v24@0:8@16", "v20@0:8f16", "v20@0:8B16", "v16@0:8", "B24@0:8d16"};
    for (unsigned i = 0; i < sizeof(selectors) / sizeof(selectors[0]); ++i) {
        Method member = class_getInstanceMethod(cls, sel_registerName(selectors[i]));
        const char *encoding = member ? method_getTypeEncoding(member) : nullptr;
        if (!encoding || strcmp(encoding, encodings[i])) {
            printf("LAYER_TEST_ABI selector=%s actual=%s\n", selectors[i], encoding ?: "(none)"); return 1;
        }
    }
    void *library = dlopen("/System/Library/PrivateFrameworks/ProgressUI.framework/ProgressUI", RTLD_LAZY);
    auto level = static_cast<const float *>(library ? dlsym(library, "kPUIDefaultContextLevel") : nullptr);
    if (!level || !isfinite(*level)) { puts("LAYER_TEST native context level unavailable"); return 1; }
    @try {
        Class displayClass = NSClassFromString(@"CADisplay");
        SEL displaysSelector = sel_registerName("displays");
        if (![displayClass respondsToSelector:displaysSelector]) return 1;
        id displays = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(displayClass, displaysSelector);
        id display = [displays isKindOfClass:[NSArray class]] ? [displays firstObject] : nil;
        id value = [display valueForKey:@"bounds"];
        if (![value isKindOfClass:[NSValue class]]) return 1;
        CGRect bounds = {};
        [value getValue:&bounds size:sizeof(bounds)];
        if (!isfinite(bounds.size.width) || !isfinite(bounds.size.height) ||
                bounds.size.width <= 0 || bounds.size.height <= 0 || bounds.size.width > 4096 || bounds.size.height > 4096) return 1;
        printf("LAYER_TEST_DISPLAY %s width=%.9g height=%.9g\n", [[display description] UTF8String], bounds.size.width, bounds.size.height);
        printf("LAYER_TEST_BEGIN native_remote_context=1 displayable=1 duration_seconds=%u animated=%u\n",animate?20:60,animate);
        if(animate && displayState(true))return 1;
        if(animate)frameStatistics(false);
        // Original ProgressUI _createContext passes kCAContextDisplayable=YES
        // (the native exported CFString's value is "displayable"). An ordinary
        // remote context requires a host and does not present on its own.
        CAContext *context = [cls remoteContextWithOptions:@{@"displayable": @YES}];
        if (!context) { puts("LAYER_TEST_CONTEXT nil"); return 1; }
        CALayer *root = [CALayer layer];
        CALayer *bar = [CALayer layer];
        CGColorSpaceRef colors = CGColorSpaceCreateDeviceRGB();
        CGFloat background[] = {0.8, 0.05, 0.05, 1};
        CGFloat foreground[] = {1, 1, 1, 1};
        CGColorRef red = colors ? CGColorCreate(colors, background) : nullptr;
        CGColorRef white = colors ? CGColorCreate(colors, foreground) : nullptr;
        if (!red || !white) {
            if (red) CGColorRelease(red);
            if (white) CGColorRelease(white);
            if (colors) CGColorSpaceRelease(colors);
            [context invalidate]; return 1;
        }
        // The layer content is independent of product-specific artwork. The
        // native compositor chooses its renderer and submits the real swap.
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        root.frame = CGRectMake(0, 0, bounds.size.width, bounds.size.height);
        root.backgroundColor = red;
        root.opaque = YES;
        bar.frame = CGRectMake(bounds.size.width / 4, bounds.size.height / 2 - 20, bounds.size.width / 2, 40);
        bar.backgroundColor = white;
        [root addSublayer:bar];
        context.secure = YES;
        context.level = *level;
        context.layer = root;
        if(animate) {
            CABasicAnimation *motion=[CABasicAnimation animationWithKeyPath:@"position.x"];
            motion.fromValue=@(bounds.size.width/4);motion.toValue=@(bounds.size.width*3/4);
            motion.duration=1;motion.autoreverses=YES;motion.repeatCount=30;
            [bar addAnimation:motion forKey:@"displayctl-motion"];
        }
        [CATransaction commit];
        [CATransaction flush];
        CGColorRelease(red); CGColorRelease(white); CGColorSpaceRelease(colors);
        describeNativeObject("context", context, @[@"contextId", @"displayId", @"commitId", @"valid", @"isValid", @"isSecure", @"level", @"layer", @"options"]);
        describeNativeObject("layer", root, @[@"bounds", @"position", @"hidden", @"opacity", @"contentsScale", @"sublayers"]);
        fflush(stdout);
        BOOL rendered = [context waitForRenderingWithTimeout:2.0];
        printf("LAYER_TEST_RENDER_WAIT result=%u timeout_seconds=2; physical_presentation_unverified=1\n", rendered);
        fflush(stdout);
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:animate?20:60]];
        if(animate)frameStatistics(true);
        [context invalidate];
        puts("LAYER_TEST_END hardware_presentation_unverified=1");
        return 0;
    } @catch (NSException *error) {
        printf("LAYER_TEST_EXCEPTION %s %s\n", error.name.UTF8String, error.reason.UTF8String); return 1;
    }
}
static int progressUI() {
    // Run in a fresh Bash child: native QuartzCore chooses its bootstrap domain
    // on first use. This selects the original foreground-user client path.
    auto nativeSetenv = reinterpret_cast<int (*)(const char *, const char *, int)>(dlsym(RTLD_DEFAULT, "setenv"));
    auto nativeIsMain = reinterpret_cast<int (*)()>(dlsym(RTLD_DEFAULT, "pthread_main_np"));
    if (!nativeSetenv || !nativeIsMain || !nativeIsMain() || nativeSetenv("CA_SYSTEM_SESSION_CLIENT", "1", 1)) return 1;
    void *library = dlopen("/System/Library/PrivateFrameworks/ProgressUI.framework/ProgressUI", RTLD_LAZY);
    auto level = static_cast<const float *>(library ? dlsym(library, "kPUIDefaultContextLevel") : nullptr);
    Dl_info image = {};
    const uint8_t uuid[] = {0xea,0xad,0x30,0xb2,0x55,0x53,0x38,0x5f,0xb9,0xc3,0x44,0x51,0x35,0x5b,0xd1,0x0e};
    Class cls = NSClassFromString(@"PUIProgressWindow");
    const char *selectors[] = {"initWithOptions:contextLevel:appearance:", "setProgressValue:", "setVisible:", "errorDescription", "layer"};
    const char *encodings[] = {"@36@0:8Q16f24q28", "v20@0:8f16", "v20@0:8B16", "@16@0:8", "@16@0:8"};
    if (!level || !dladdr(level, &image) || !pinnedImage(image.dli_fbase, uuid) || !isfinite(*level) || !cls) {
        puts("PROGRESS_UI native image/default level unavailable"); return 1;
    }
    for (unsigned i = 0; i < sizeof(selectors) / sizeof(selectors[0]); ++i) {
        Method method = class_getInstanceMethod(cls, sel_registerName(selectors[i]));
        const char *encoding = method ? method_getTypeEncoding(method) : nullptr;
        if (!encoding || strcmp(encoding, encodings[i])) {
            printf("PROGRESS_UI_ABI selector=%s actual=%s\n", selectors[i], encoding ? encoding : "(none)"); return 1;
        }
    }
    @try {
        // The native datamigrator uses options 5 and the exported default level.
        // A fixed diagnostic value does not change migration/keybag state.
        puts("PROGRESS_UI_BEGIN diagnostic_progress=0.5");
        PUIProgressWindow *window = [[cls alloc] initWithOptions:5 contextLevel:*level appearance:0];
        if (!window) { puts("PROGRESS_UI_WINDOW nil"); return 1; }
        [window setProgressValue:0.5f];
        [window setVisible:YES];
        printf("PROGRESS_UI_WINDOW object=%p layer=%p error=%s\n", (__bridge void *)window,
            (__bridge void *)window.layer, window.errorDescription.UTF8String ?: "(none)");
        Ivar ivar = class_getInstanceVariable(cls, "_context");
        if (ivar) describeNativeObject("progress-context", object_getIvar(window, ivar),
            @[@"contextId", @"valid", @"isValid", @"isSecure", @"level", @"layer", @"options"]);
        describeNativeObject("progress-layer", window.layer, @[@"bounds", @"position", @"hidden", @"opacity", @"sublayers"]);
        fflush(stdout);
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:10]];
        [window setVisible:NO];
        puts("PROGRESS_UI_END hardware_presentation_unverified=1");
        return window.layer && !window.errorDescription.length ? 0 : 1;
    } @catch (NSException *error) {
        printf("PROGRESS_UI_EXCEPTION %s %s\n", error.name.UTF8String, error.reason.UTF8String); return 1;
    }
}
static int nativeExceptionWindow() {
    auto query = reinterpret_cast<int (*)(const char *, void *, size_t *, const void *, size_t)>(dlsym(RTLD_DEFAULT, "sysctlbyname"));
    auto list = reinterpret_cast<int (*)(uint32_t, uint32_t, void *, int)>(dlsym(RTLD_DEFAULT, "proc_listpids"));
    auto processName = reinterpret_cast<int (*)(int, void *, uint32_t)>(dlsym(RTLD_DEFAULT, "proc_name"));
    auto usage = reinterpret_cast<int (*)(int, int, rusage_info_t *)>(dlsym(RTLD_DEFAULT, "proc_pid_rusage"));
    mach_timebase_info_data_t timebase{};
    if (!query || !list || !processName || !usage || mach_timebase_info(&timebase) != KERN_SUCCESS || !timebase.denom || !timebase.numer) {
        puts("NATIVE_EXCEPTION_WINDOW process APIs unavailable"); return 1;
    }
    int queue = kqueue();
    if (queue < 0) { printf("NATIVE_EXCEPTION_WINDOW kqueue errno=%d\n", errno); return 1; }
    const char *name = "debug.exception_log_max_pid";
    int previous = 0, limit = INT32_MAX;
    size_t bytes = sizeof(previous);
    if (query(name, &previous, &bytes, &limit, sizeof(limit))) {
        printf("NATIVE_EXCEPTION_WINDOW unavailable errno=%d\n", errno);
        close(queue);
        return 1;
    }
    // Keep native exception/corpse delivery intact. EVFILT_PROC supplies the
    // actual wait status and jetsam/codesigning details even after reaping.
    // PROC_PIDEXITREASONINFO requires the parent/debugger on this XNU and
    // cannot be used by this observer, including when it runs as root.
    printf("NATIVE_EXCEPTION_WINDOW begin seconds=12 previous=%d timebase_numer=%u timebase_denom=%u\n",
        previous, timebase.numer, timebase.denom);
    struct Watch { int pid; uint64_t start, sampled; };
    std::vector<Watch> watches;
    unsigned observed = 0, exits = 0, errors = 0;
    const uint64_t begin = mach_absolute_time();
    const uint64_t second = uint64_t(1000000000) * timebase.denom / timebase.numer;
    while (mach_absolute_time() - begin < 12 * second) {
        int pids[4096] = {};
        int count = list(1, 0, pids, sizeof(pids)); // PROC_ALL_PIDS
        if (count <= 0 || count > int(sizeof(pids)) || count % sizeof(pids[0])) {
            printf("NATIVE_EXCEPTION_WINDOW process_list bytes=%d errno=%d\n", count, errno); ++errors; break;
        }
        for (int i = 0; i < count / int(sizeof(pids[0])); ++i) {
            char process[64] = {};
            if (pids[i] <= 0 || processName(pids[i], process, sizeof(process) - 1) <= 0 ||
                    (strcmp(process, "MercuryPosterExtension") && strcmp(process, "MercuryPosterExt"))) continue;
            auto at = std::find_if(watches.begin(), watches.end(), [&](const Watch &w) { return w.pid == pids[i]; });
            if (at == watches.end()) {
                if (watches.size() >= 64) { puts("NATIVE_EXCEPTION_WINDOW watch_limit=64"); ++errors; break; }
                struct kevent change{};
                EV_SET(&change, pids[i], EVFILT_PROC, EV_ADD | EV_ENABLE, NOTE_EXIT | NOTE_EXITSTATUS | NOTE_EXIT_DETAIL, 0, nullptr);
                struct timespec zero{};
                if (kevent(queue, &change, 1, nullptr, 0, &zero) < 0) {
                    // An exit between enumeration and registration is a gap,
                    // not proof of a particular signal or a successful watch.
                    printf("NATIVE_PROCESS_WATCH pid=%d result=-1 errno=%d\n", pids[i], errno); ++errors; continue;
                }
                watches.push_back({pids[i], 0, 0}); at = watches.end() - 1; ++observed;
                printf("NATIVE_PROCESS_WATCH pid=%d result=0\n", pids[i]);
            }
            const uint64_t now = mach_absolute_time();
            if (at->sampled && now - at->sampled < second) continue;
            rusage_info_v4 info{};
            int result = usage(pids[i], RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t *>(&info));
            if (result) { printf("NATIVE_PROCESS_USAGE pid=%d result=%d errno=%d\n", pids[i], result, errno); ++errors; }
            else {
                if (at->start && at->start != info.ri_proc_start_abstime) {
                    printf("NATIVE_PROCESS_USAGE pid=%d identity_changed=1\n", pids[i]); ++errors;
                }
                at->start = info.ri_proc_start_abstime;
                // This XNU exports recount's Mach ticks through libproc.
                // Preserve raw counters and convert with the actual timebase.
                printf("NATIVE_PROCESS_USAGE pid=%d observed_ticks=%llu start_ticks=%llu age_ms=%.3f user_ticks=%llu system_ticks=%llu user_ms=%.3f system_ms=%.3f resident=%llu footprint=%llu max_footprint=%llu\n",
                    pids[i], now, info.ri_proc_start_abstime,
                    now >= info.ri_proc_start_abstime ? double(now - info.ri_proc_start_abstime) * timebase.numer / timebase.denom / 1e6 : -1.0,
                    info.ri_user_time, info.ri_system_time,
                    double(info.ri_user_time) * timebase.numer / timebase.denom / 1e6,
                    double(info.ri_system_time) * timebase.numer / timebase.denom / 1e6, info.ri_resident_size,
                    info.ri_phys_footprint, info.ri_lifetime_max_phys_footprint);
            }
            at->sampled = now;
        }
        struct kevent events[16]{};
        struct timespec timeout{0, 250000000};
        int ready = kevent(queue, nullptr, 0, events, 16, &timeout);
        if (ready < 0) { printf("NATIVE_EXCEPTION_WINDOW wait errno=%d\n", errno); ++errors; break; }
        for (int i = 0; i < ready; ++i) {
            const auto &event = events[i];
            if (event.flags & EV_ERROR) {
                printf("NATIVE_PROCESS_EXIT pid=%lu event_error=%lld\n", (unsigned long)event.ident, (long long)event.data); ++errors; continue;
            }
            if (!(event.fflags & NOTE_EXIT)) continue;
            const uint32_t detail = uint32_t(event.data);
            const bool hasStatus = event.fflags & NOTE_EXITSTATUS;
            const int status = int(detail & 0xffff);
            printf("NATIVE_PROCESS_EXIT pid=%lu observed_ticks=%llu fflags=0x%x raw=0x%x status_valid=%d signal=%d exit_code=%d memory=%d codesigning=%d decrypt=%d\n",
                (unsigned long)event.ident, mach_absolute_time(), event.fflags, detail, hasStatus,
                hasStatus && WIFSIGNALED(status) ? WTERMSIG(status) : 0,
                hasStatus && WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                bool(detail & NOTE_EXIT_MEMORY), bool(detail & NOTE_EXIT_CSERROR), bool(detail & NOTE_EXIT_DECRYPTFAIL));
            watches.erase(std::remove_if(watches.begin(), watches.end(), [&](const Watch &w) { return w.pid == int(event.ident); }), watches.end());
            ++exits;
        }
        fflush(stdout);
    }
    close(queue);
    int result = query(name, nullptr, nullptr, &previous, sizeof(previous));
    printf("NATIVE_EXCEPTION_WINDOW end restore_result=%d errno=%d observed=%u exits=%u errors=%u\n", result, result ? errno : 0, observed, exits, errors);
    return result || errors ? 1 : 0;
}
static int deviceIdentity(bool invalidate = false) {
    // Query the kernel independently of MobileGestalt's persistent answers.
    auto query = reinterpret_cast<int (*)(const char *, void *, size_t *, const void *, size_t)>(dlsym(RTLD_DEFAULT, "sysctlbyname"));
    char product[128] = {}, machine[128] = {};
    char *buffers[] = {product, machine};
    const char *names[] = {"hw.product", "hw.machine"};
    bool kernelValid = query != nullptr;
    for (unsigned i = 0; i < 2; ++i) {
        size_t length = sizeof(product);
        int result = query ? query(names[i], buffers[i], &length, nullptr, 0) : -1;
        int error = result ? errno : 0;
        bool valid = !result && length && length <= sizeof(product) && memchr(buffers[i], 0, length);
        kernelValid &= valid;
        printf("DEVICE_SYSCTL %s=%s result=%d errno=%d\n", names[i], valid ? buffers[i] : "(unavailable)", result, error);
    }
    void *library = dlopen("/usr/lib/libMobileGestalt.dylib", RTLD_LAZY);
    auto answer = reinterpret_cast<CFTypeRef (*)(CFStringRef)>(library ? dlsym(library, "MGCopyAnswer") : nullptr);
    NSMutableDictionary *answers = [NSMutableDictionary dictionary];
    for (NSString *key in @[@"ProductType", @"DeviceClass", @"DeviceClassNumber"]) {
        CFTypeRef value = answer ? answer((__bridge CFStringRef)key) : nullptr;
        if (value) answers[key] = CFBridgingRelease(value);
        printf("DEVICE_GESTALT %s=%s\n", key.UTF8String, answers[key] ? [answers[key] description].UTF8String : "(unavailable)");
    }
    NSString *source = @"/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist";
    NSFileManager *files = [NSFileManager defaultManager];
    NSError *error = nil;
    NSDictionary *attributes = [files attributesOfItemAtPath:source error:&error];
    bool regular = [attributes[NSFileType] isEqualToString:NSFileTypeRegular] &&
        [attributes[NSFileSize] unsignedLongLongValue] <= 1024 * 1024;
    NSData *before = regular ? [NSData dataWithContentsOfFile:source] : nil;
    id plist = before ? [NSPropertyListSerialization propertyListWithData:before options:0 format:nullptr error:&error] : nil;
    NSDictionary *cache = [plist isKindOfClass:NSDictionary.class] ? plist : nil;
    printf("DEVICE_CACHE regular=%d bytes=%lu version=%s keys=%s\n", regular, (unsigned long)before.length,
        cache[@"CacheVersion"] ? [cache[@"CacheVersion"] description].UTF8String : "(none)",
        cache ? [cache.allKeys description].UTF8String : "(none)");
    NSDictionary *extra = [cache[@"CacheExtra"] isKindOfClass:NSDictionary.class] ? cache[@"CacheExtra"] : nil;
    for (NSString *key in @[@"h9jDsbgj7xIVeIQ8S3/X3Q", @"+3Uf0Pm5F8Xy7Onyvko0vA", @"mtrAoWJ3gsq+I90ZnQ0vQw"])
        printf("DEVICE_CACHE_ANSWER %s=%s\n", key.UTF8String, extra[key] ? [extra[key] description].UTF8String : "(none)");
    if (!invalidate) return kernelValid && answers.count == 3 ? 0 : 1;
    // Discard only the proven pre-migration identity cache. Never manufacture
    // answers: a subsequent native process must regenerate the entire cache.
    if (!kernelValid || strcmp(product, "iPhoneMi6,1") || strcmp(machine, product) ||
        ![answers[@"ProductType"] isEqual:@"Xiaomi Mi 6 (sagit)"] ||
        ![answers[@"DeviceClass"] isEqual:@"iPod"] || ![answers[@"DeviceClassNumber"] isEqual:@2] ||
        ![cache[@"CacheVersion"] isEqual:@"22H374"] ||
        ![extra[@"h9jDsbgj7xIVeIQ8S3/X3Q"] isEqual:@"Xiaomi Mi 6 (sagit)"]) {
        puts("DEVICE_CACHE_INVALIDATE expected kernel/cache identity mismatch absent; cache retained"); return 1;
    }
    auto nativeSync = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "sync"));
    auto nativeUnlink = reinterpret_cast<int (*)(const char *)>(dlsym(RTLD_DEFAULT, "unlink"));
    if (!nativeSync || !nativeUnlink) return 1;
    NSString *backup = [@"/private/var/device-identity-gestalt-" stringByAppendingFormat:@"%@.plist", NSUUID.UUID.UUIDString];
    if (![files copyItemAtPath:source toPath:backup error:&error] ||
        ![before isEqualToData:[NSData dataWithContentsOfFile:backup]] ||
        ![before isEqualToData:[NSData dataWithContentsOfFile:source]]) {
        printf("DEVICE_CACHE_INVALIDATE backup verification failed=%s; cache retained\n", error.description.UTF8String ?: "byte mismatch"); return 1;
    }
    nativeSync();
    int result = nativeUnlink(source.fileSystemRepresentation);
    int saved = result ? errno : 0;
    nativeSync();
    printf("DEVICE_CACHE_INVALIDATE result=%d errno=%d backup=%s bytes=%lu; native regeneration required\n",
        result, saved, backup.UTF8String, (unsigned long)before.length);
    return result ? 1 : 0;
}
static int invalidateGestaltCache() {
    // Explicit development action: discard only the known stale zero-scale
    // record after a verified backup. The next native process rebuilds it.
    uint32_t scale = chosenDisplayScale();
    if (scale < 1 || scale > 3) { puts("GESTALT_INVALIDATE invalid chosen scale; cache retained"); return 1; }
    auto nativeSync = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "sync"));
    auto nativeUnlink = reinterpret_cast<int (*)(const char *)>(dlsym(RTLD_DEFAULT, "unlink"));
    if (!nativeSync || !nativeUnlink) return 1;
    NSString *source = @"/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist";
    NSString *backup = @"/private/var/display-gestalt-cache.original.plist";
    NSFileManager *files = [NSFileManager defaultManager];
    NSError *error = nil;
    NSDictionary *attributes = [files attributesOfItemAtPath:source error:&error];
    if (![attributes[NSFileType] isEqualToString:NSFileTypeRegular] || [attributes[NSFileSize] unsignedLongLongValue] > 1024 * 1024) {
        puts("GESTALT_INVALIDATE unexpected cache file; retained"); return 1;
    }
    NSData *before = [NSData dataWithContentsOfFile:source];
    id plist = before ? [NSPropertyListSerialization propertyListWithData:before options:0 format:nullptr error:&error] : nil;
    id extra = [plist isKindOfClass:[NSDictionary class]] ? plist[@"CacheExtra"] : nil;
    id record = [extra isKindOfClass:[NSDictionary class]] ? extra[@"oBbtJ8x+s1q0OkaiocPuog"] : nil;
    float cachedScale = -1;
    if ([record isKindOfClass:[NSData class]] && [record length] == 24)
        memcpy(&cachedScale, static_cast<const uint8_t *>([record bytes]) + 12, sizeof(cachedScale));
    if (![plist isKindOfClass:[NSDictionary class]] || ![plist[@"CacheVersion"] isEqualToString:@"22H374"] || cachedScale != 0) {
        puts("GESTALT_INVALIDATE expected native stale zero-scale record absent; cache retained"); return 1;
    }
    if (![files copyItemAtPath:source toPath:backup error:&error]) {
        printf("GESTALT_INVALIDATE backup failed=%s; cache retained\n", error.description.UTF8String); return 1;
    }
    if (![before isEqualToData:[NSData dataWithContentsOfFile:backup]] ||
            ![before isEqualToData:[NSData dataWithContentsOfFile:source]]) {
        puts("GESTALT_INVALIDATE backup verification failed; cache retained"); return 1;
    }
    nativeSync();
    int result = nativeUnlink(source.fileSystemRepresentation);
    int saved = errno;
    nativeSync();
    printf("GESTALT_INVALIDATE result=%d errno=%d backup=%s bytes=%lu chosen_scale=%u; next process uses native regeneration\n",
        result, result ? saved : 0, backup.UTF8String, (unsigned long)before.length, scale);
    return result ? 1 : 0;
}
static void registry() {
    chosenDisplayScale();
    const char *classes[] = {"IOMobileFramebuffer", "IOSurfaceRoot", "AppleARMMIPIDSIController", "QcomPanel", "PMI8998WLED", "AppleKeyStore"};
    for (const char *name : classes) {
        mach_port_t iterator = 0;
        kern_return_t result = IOServiceGetMatchingServices(0, IOServiceMatching(name), &iterator);
        printf("DISPLAY_REGISTRY class=%s result=0x%x\n", name, result);
        if (result) continue;
        unsigned count = 0;
        for (mach_port_t service; (service = IOIteratorNext(iterator));) {
            ++count;
            uint64_t identifier = 0; IORegistryEntryGetRegistryEntryID(service, &identifier);
            printf("DISPLAY_SERVICE id=%llu\n", identifier);
            for (NSString *key in @[@"IOClass", @"IOMFBUUID", @"DisplayWidth", @"DisplayHeight",
                    @"Width", @"Height", @"RowBytes", @"PixelFormat", @"Location", @"IOMFBDisplayRefresh",
                    @"BacklightStateValid", @"BacklightLastError", @"DisplayBrightness", @"DisplayAppliedBrightness",
                    @"DisplayBacklightPower", @"DisplayPowerTransitionError", @"PanelReady", @"MDP Double Buffered",
                    @"BacklightPower", @"BacklightBrightness", @"BacklightAppliedBrightness", @"NativeBrightnessRequestCount"]) {
                CFTypeRef value = IORegistryEntryCreateCFProperty(service, (__bridge CFStringRef)key, nullptr, 0);
                if (value) {
                    NSString *description = [(__bridge id)value description];
                    printf("  %s=%s\n", key.UTF8String, description.UTF8String); CFRelease(value);
                }
            }
            IOObjectRelease(service);
        }
        IOObjectRelease(iterator);
        printf("DISPLAY_REGISTRY_COUNT class=%s count=%u\n", name, count);
    }
}
static int streamCompressedData(NSData *data) {
    void *library=dlopen("/usr/lib/libz.1.dylib",RTLD_LAZY);
    using Bound=unsigned long (*)(unsigned long);
    using Compress=int (*)(unsigned char *,unsigned long *,const unsigned char *,unsigned long,int);
    auto bound=reinterpret_cast<Bound>(library?dlsym(library,"compressBound"):nullptr);
    auto compress=reinterpret_cast<Compress>(library?dlsym(library,"compress2"):nullptr);
    if(!bound || !compress)return 1;
    NSMutableData *packed=[NSMutableData dataWithLength:bound(data.length)];unsigned long bytes=packed.length;
    // Export runs after the capture window. Spend CPU here to reduce UART
    // time; large records still retain exact offsets and the zlib checksum.
    if(compress(static_cast<unsigned char *>(packed.mutableBytes),&bytes,static_cast<const unsigned char *>(data.bytes),data.length,6))return 1;
    packed.length=bytes;NSString *base64=[packed base64EncodedStringWithOptions:0];const char *text=base64.UTF8String;const size_t length=strlen(text);
    printf("DISPLAY_BINARY_BEGIN version=1 bytes=%lu packed=%lu base64=%zu codec=zlib\n",(unsigned long)data.length,bytes,length);
    constexpr size_t chunk=1024;
    for(size_t offset=0;offset<length;offset+=chunk)printf("DISPLAY_BINARY_DATA offset=%zu data=%.*s\n",offset,int(std::min(chunk,length-offset)),text+offset);
    puts("DISPLAY_BINARY_END");return 0;
}
static int diagnosticFile(const char *name,bool image=false,bool thumbnail=false) {
    // Foundation canonicalizes /private/var to /var on the target. Validate
    // the fixed directory and a single ASCII basename lexically instead.
    constexpr char prefix[]="/private/var/tmp/setup-";
    if(!name || strncmp(name,prefix,sizeof(prefix)-1))return 2;
    const size_t length=strlen(name);
    if(length<=sizeof(prefix)-1+4 || length>255 || strcmp(name+length-4,image?".png":".txt"))return 2;
    for(size_t i=sizeof(prefix)-1;i<length-4;++i)
        if(!((name[i]>='a' && name[i]<='z') || (name[i]>='A' && name[i]<='Z') ||
            (name[i]>='0' && name[i]<='9') || name[i]=='-'))return 2;
    NSString *path=[NSString stringWithUTF8String:name];
    if(!path)return 2;
    NSError *error=nil;
    NSDictionary *attributes=[[NSFileManager defaultManager] attributesOfItemAtPath:path error:&error];
    if(!attributes || ![attributes[NSFileType] isEqualToString:NSFileTypeRegular] ||
        [attributes[NSFileSize] unsignedLongLongValue]>8*1024*1024)return 1;
    NSData *data=[NSData dataWithContentsOfFile:path options:NSDataReadingUncached error:&error];
    if (thumbnail && data) {
        CGImageSourceRef source=CGImageSourceCreateWithData((__bridge CFDataRef)data,nullptr);
        NSDictionary *options=@{(__bridge NSString *)kCGImageSourceCreateThumbnailFromImageAlways:@YES,
            (__bridge NSString *)kCGImageSourceThumbnailMaxPixelSize:@640};
        CGImageRef small=source?CGImageSourceCreateThumbnailAtIndex(source,0,(__bridge CFDictionaryRef)options):nullptr;
        NSMutableData *output=[NSMutableData data];
        CGImageDestinationRef destination=small?CGImageDestinationCreateWithData((__bridge CFMutableDataRef)output,CFSTR("public.png"),1,nullptr):nullptr;
        if(destination)CGImageDestinationAddImage(destination,small,nullptr);
        const bool ok=destination && CGImageDestinationFinalize(destination);
        if(destination)CFRelease(destination);if(small)CGImageRelease(small);if(source)CFRelease(source);
        if(!ok)return 1;data=output;
    }
    return data && data.length<=8*1024*1024?streamCompressedData(data):1;
}
static int displayTimeline(bool binary=false) {
    mach_port_t iterator=0;
    auto status=IOServiceGetMatchingServices(0,IOServiceMatching("QcomDisplay"),&iterator);if(status)return 1;
    auto service=IOIteratorNext(iterator);IOObjectRelease(iterator);if(!service)return 1;
    status=IORegistryEntrySetCFProperties(service,(__bridge CFTypeRef)@{@"DisplayTimeline":@YES});
    CFTypeRef value=status?nullptr:IORegistryEntryCreateCFProperty(service,CFSTR("DisplayTimeline"),nullptr,0);IOObjectRelease(service);
    NSData *data=value && CFGetTypeID(value)==CFDataGetTypeID()?(__bridge NSData *)value:nil;
    Display::TimelineHeader h{};if(data.length>=sizeof(h))memcpy(&h,data.bytes,sizeof(h));
    const bool valid=!status && data.length>=sizeof(h) && h.version==Display::TimelineVersion && h.sampleBytes==sizeof(Display::TimelineSample) &&
        h.capacity==Display::TimelineCapacity && h.count<=h.capacity && h.total>=h.firstSerial &&
        h.count+h.unavailable==h.total-h.firstSerial && data.length==sizeof(h)+h.count*sizeof(Display::TimelineSample);
    printf("DISPLAY_TIMELINE version=%u valid=%u status=0x%x count=%u total=%llu dropped=%llu unavailable=%llu first_serial=%llu now=%llu timebase_ticks=%llu timebase_ns=%llu\n",
        h.version,valid,status,h.count,h.total,h.dropped,h.unavailable,h.firstSerial,h.nowTicks,h.timebaseTicks,h.timebaseNS);
    if(valid && binary){const int result=streamCompressedData(data);if(value)CFRelease(value);return result;}
    if(valid) {
        printf("DISPLAY_TEAR tear=%x sync=%x height=%u write_init=%u read_init=%u threshold=%x start=%u read_irq=%u write_irq=%u auto=%x\n",
            h.tear,h.sync,h.height,h.writeInit,h.readInit,h.threshold,h.start,h.readIRQ,h.writeIRQ,h.autoRefresh);
        for(uint32_t i=0;i<h.count;++i) {
            Display::TimelineSample v;memcpy(&v,static_cast<const uint8_t *>(data.bytes)+sizeof(h)+i*sizeof(v),sizeof(v));
            printf("DISPLAY_EVENT index=%u ticks=%llu request=%llx kind=%u token=%u irq=%x flush=%x ctl=%x read=%x write=%x address=%x aux=%llu\n",
                i,v.words[0],v.words[1],uint32_t(v.words[2]>>32),uint32_t(v.words[2]),uint32_t(v.words[3]>>32),uint32_t(v.words[3]),
                uint32_t(v.words[4]>>32),uint32_t(v.words[4]),uint32_t(v.words[5]>>32),uint32_t(v.words[5]),v.words[6]);
        }
    }
    if(value)CFRelease(value);return valid?0:1;
}
static int leaseHistory() {
    mach_port_t iterator=0;
    auto status=IOServiceGetMatchingServices(0,IOServiceMatching("QcomDisplay"),&iterator);
    if(status)return 1;
    auto service=IOIteratorNext(iterator);IOObjectRelease(iterator);if(!service)return 1;
    status=IORegistryEntrySetCFProperties(service,(__bridge CFTypeRef)@{@"DisplayLeaseHistory":@YES});
    CFTypeRef value=status?nullptr:IORegistryEntryCreateCFProperty(service,CFSTR("DisplayLeaseHistory"),nullptr,0);
    IOObjectRelease(service);
    NSData *data=value && CFGetTypeID(value)==CFDataGetTypeID()?(__bridge NSData *)value:nil;
    Display::LeaseHistoryHeader h{};bool valid=data.length>=sizeof(h);
    if(valid)memcpy(&h,data.bytes,sizeof(h));
    valid=valid && h.version==Display::LeaseHistoryVersion && h.sampleBytes==sizeof(Display::LeaseSample) &&
        h.capacity==Display::LeaseHistoryCapacity && h.count<=h.capacity && h.total>=h.count &&
        data.length==sizeof(h)+h.count*sizeof(Display::LeaseSample) && h.timebaseSpanTicks && h.timebaseSpanNanoseconds;
    printf("DISPLAY_LEASE_HISTORY status=0x%x valid=%u count=%u total=%llu now=%llu timebase_ticks=%llu timebase_ns=%llu owner=0x%llx frozen_at=%llu\n",
        status,valid,h.count,(unsigned long long)h.total,(unsigned long long)h.nowTicks,
        (unsigned long long)h.timebaseSpanTicks,(unsigned long long)h.timebaseSpanNanoseconds,(unsigned long long)h.owner,(unsigned long long)h.frozenAtTicks);
    if(valid)for(uint32_t i=0;i<h.count;++i) {
        Display::LeaseSample s{};memcpy(&s,static_cast<const uint8_t *>(data.bytes)+sizeof(h)+i*sizeof(s),sizeof(s));
        printf("DISPLAY_LEASE index=%u phase=%u ticks=%llu request=0x%llx token=%u intent=%u active=0x%llx queued=0x%llx changed=%x ready=%x state=%u cancelled=%u front=%u staged=%u completed=%u displayed=%u flags=%x\n",
            i,s.phase,(unsigned long long)s.ticks,(unsigned long long)s.request,s.token,s.intent,
            (unsigned long long)s.active,(unsigned long long)s.queued,s.changed,s.ready,s.state,s.cancelled,
            s.front,s.staged,s.lastCompleted,s.lastDisplayed,s.flags);
        for(unsigned layer=0;layer<4;++layer)if(s.surfaces[layer] || s.effective[layer] || s.displayed[layer] || s.fences[layer] || s.displayedFences[layer])
            printf("DISPLAY_LEASE_LAYER index=%u layer=%u surface=%u effective=0x%llx fence=0x%llx displayed=%u displayed_fence=0x%llx displayed_token=%u\n",
                i,layer,s.surfaces[layer],(unsigned long long)s.effective[layer],(unsigned long long)s.fences[layer],s.displayed[layer],
                (unsigned long long)s.displayedFences[layer],s.displayedTokens[layer]);
    }
    if(value)CFRelease(value);return valid?0:1;
}
static int frameStatistics(bool raw) {
    mach_port_t iterator=0;
    auto status=IOServiceGetMatchingServices(0,IOServiceMatching("QcomDisplay"),&iterator);
    if(status)return 1;
    auto service=IOIteratorNext(iterator);IOObjectRelease(iterator);if(!service)return 1;
    status=IORegistryEntrySetCFProperties(service,(__bridge CFTypeRef)@{@"DisplayFrameStatistics":@YES});
    CFTypeRef value=status?nullptr:IORegistryEntryCreateCFProperty(service,CFSTR("DisplayFrameStatistics"),nullptr,0);
    CFTypeRef prepareValue=status?nullptr:IORegistryEntryCreateCFProperty(service,CFSTR("DisplayPrepareStatistics"),nullptr,0);
    CFTypeRef cacheValue=status?nullptr:IORegistryEntryCreateCFProperty(service,CFSTR("DisplayMappingCacheStatistics"),nullptr,0);
    IOObjectRelease(service);
    NSData *data=value && CFGetTypeID(value)==CFDataGetTypeID()?(__bridge NSData *)value:nil;
    Display::SwapStatisticsHeader header{};bool valid=data.length>=sizeof(header);
    if(valid)memcpy(&header,data.bytes,sizeof(header));
    valid=valid && header.version==Display::SwapStatisticsVersion && !header.reserved &&
        header.sampleBytes==sizeof(Display::SwapSample) && header.count<=Display::SwapSampleCapacity &&
        data.length==sizeof(header)+header.count*sizeof(Display::SwapSample) && header.timebaseSpanTicks && header.timebaseSpanNanoseconds;
    if(!valid){printf("DISPLAY_FRAMES status=0x%x valid=0\n",status);if(value)CFRelease(value);if(prepareValue)CFRelease(prepareValue);if(cacheValue)CFRelease(cacheValue);return 1;}
    const double usPerTick=double(header.timebaseSpanNanoseconds)/header.timebaseSpanTicks/1000;
    const auto *samples=reinterpret_cast<const Display::SwapSample *>(static_cast<const uint8_t *>(data.bytes)+sizeof(header));
    std::vector<double> intervals;uint64_t shortGaps=0,longGaps=0;double span=0;
    for(uint32_t i=0;i<header.count;++i) {
        if(i) {
            if(samples[i].ticks<=samples[i-1].ticks){if(value)CFRelease(value);if(prepareValue)CFRelease(prepareValue);if(cacheValue)CFRelease(cacheValue);return 1;}
            const double us=(samples[i].ticks-samples[i-1].ticks)*usPerTick;intervals.push_back(us);span+=us;
            if(us>20000)++shortGaps;if(us>33333.334)++longGaps;
        }
        if(raw)printf("DISPLAY_FRAME index=%u token=%u ticks=%llu flags=%u\n",i,samples[i].token,(unsigned long long)samples[i].ticks,samples[i].flags);
    }
    std::sort(intervals.begin(),intervals.end());
    auto percentile=[&](size_t numerator){return intervals.empty()?0.0:intervals[(intervals.size()-1)*numerator/100];};
    printf("DISPLAY_FRAMES version=%u displayed=%llu failed=%llu samples=%u span_ms=%.3f rate=%.3f p50_us=%.3f p95_us=%.3f p99_us=%.3f max_us=%.3f over20ms=%llu over33ms=%llu age_ms=%.3f timebase_ticks=%llu timebase_ns=%llu\n",
        header.version,(unsigned long long)header.displayed,(unsigned long long)header.failed,header.count,span/1000,
        span>0?intervals.size()*1000000/span:0,percentile(50),percentile(95),percentile(99),intervals.empty()?0:intervals.back(),
        (unsigned long long)shortGaps,(unsigned long long)longGaps,
        header.count && header.nowTicks>=samples[header.count-1].ticks?(header.nowTicks-samples[header.count-1].ticks)*usPerTick/1000:0,
        (unsigned long long)header.timebaseSpanTicks,(unsigned long long)header.timebaseSpanNanoseconds);
    Display::PrepareStatistics preparation{};
    if(prepareValue && CFGetTypeID(prepareValue)==CFDataGetTypeID() && CFDataGetLength((CFDataRef)prepareValue)==sizeof(preparation)) {
        memcpy(&preparation,CFDataGetBytePtr((CFDataRef)prepareValue),sizeof(preparation));
        if(preparation.version==1 && preparation.bytes==sizeof(preparation) &&
            preparation.nowTicks==header.nowTicks && preparation.displayed==header.displayed)
            printf("DISPLAY_PREPARE version=1 now=%llu displayed=%llu hardware_vbl=%llu attempts=%llu hits=%llu discarded=%llu prepare_ticks=%llu hit_lead_ticks=%llu late_calls=%llu late_ticks=%llu\n",
                (unsigned long long)preparation.nowTicks,(unsigned long long)preparation.displayed,
                (unsigned long long)preparation.hardwareVBL,
                (unsigned long long)preparation.attempts,(unsigned long long)preparation.hits,(unsigned long long)preparation.discarded,
                (unsigned long long)preparation.prepareTicks,(unsigned long long)preparation.hitLeadTicks,
                (unsigned long long)preparation.lateCalls,(unsigned long long)preparation.lateTicks);
    }
    Display::MappingCacheStatistics cache;
    if(cacheValue && CFGetTypeID(cacheValue)==CFDataGetTypeID() && CFDataGetLength((CFDataRef)cacheValue)==sizeof(cache)) {
        memcpy(&cache,CFDataGetBytePtr((CFDataRef)cacheValue),sizeof(cache));
        if(cache.version==1 && cache.bytes==sizeof(cache) && cache.nowTicks==header.nowTicks && cache.displayed==header.displayed)
            printf("DISPLAY_MAP_CACHE version=1 now=%llu displayed=%llu hits=%llu misses=%llu evicted=%llu cached=%llu\n",cache.nowTicks,cache.displayed,cache.hits,cache.misses,cache.evicted,cache.cached);
    }
    if(prepareValue)CFRelease(prepareValue);if(cacheValue)CFRelease(cacheValue);
    if(value)CFRelease(value);return 0;
}
static int streamScanoutPNG(NSData *pixels, size_t width, size_t height, size_t stride,const char *imagePath=nullptr,
                            const char *captureSource="scanout-buffer") {
    // Encode the real scanout, ignoring its unused alpha byte. This lets the
    // UART console inspect native UI without rebooting into the UFS exporter.
    void *library = dlopen("/System/Library/Frameworks/ImageIO.framework/ImageIO", RTLD_LAZY);
    auto create = reinterpret_cast<decltype(&CGImageDestinationCreateWithData)>(library ? dlsym(library, "CGImageDestinationCreateWithData") : nullptr);
    auto add = reinterpret_cast<decltype(&CGImageDestinationAddImage)>(library ? dlsym(library, "CGImageDestinationAddImage") : nullptr);
    auto finish = reinterpret_cast<decltype(&CGImageDestinationFinalize)>(library ? dlsym(library, "CGImageDestinationFinalize") : nullptr);
    if (!create || !add || !finish) return 1;
    CGColorSpaceRef color = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)pixels);
    CGImageRef image = color && provider ? CGImageCreate(width, height, 8, 32, stride, color,
        kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst, provider, nullptr, false, kCGRenderingIntentDefault) : nullptr;
    NSMutableData *data = [NSMutableData data];
    CGImageDestinationRef destination = image ? create((__bridge CFMutableDataRef)data, CFSTR("public.png"), 1, nullptr) : nullptr;
    bool encoded = false;
    if (destination) { add(destination, image, nullptr); encoded = finish(destination); CFRelease(destination); }
    if (image) CGImageRelease(image);
    if (provider) CGDataProviderRelease(provider);
    if (color) CGColorSpaceRelease(color);
    if (!encoded || !data.length || data.length > 4 * 1024 * 1024) return 1;
    if(imagePath) {
        NSError *error=nil;
        bool saved=[data writeToFile:[NSString stringWithUTF8String:imagePath] options:NSDataWritingWithoutOverwriting error:&error];
        printf("SCANOUT_PNG_FILE saved=%u bytes=%lu width=%zu height=%zu capture_source=%s optical_verified=0 path=%s\n",
            saved,(unsigned long)data.length,width,height,captureSource,imagePath);
        return saved?0:1;
    }
    NSString *base64 = [data base64EncodedStringWithOptions:0];
    const char *bytes = base64.UTF8String;
    size_t count = strlen(bytes);
    printf("SCANOUT_PNG_BEGIN bytes=%lu width=%zu height=%zu base64=%zu capture_source=%s optical_verified=0\n",
        (unsigned long)data.length, width, height, count,captureSource);
    for (size_t at = 0; at < count; at += 128)
        printf("SCANOUT_PNG_DATA offset=%zu data=%.*s\n", at, (int)(count - at < 128 ? count - at : 128), bytes + at);
    puts("SCANOUT_PNG_END");
    return 0;
}
// An owned render-server destination verifies logical page content without
// reading a DPU buffer whose single-transfer read lease has already ended.
static int captureLogicalDisplay(const char *imagePath) {
    if (geteuid() || !imagePath || strncmp(imagePath,"/private/var/tmp/setup-",23) || strstr(imagePath,"..")) return 2;
    if (setenv("CA_SYSTEM_SESSION_CLIENT","1",1)) return 1;
    struct Deadline {
        void (*previous)(int);
        Deadline():previous(signal(SIGALRM,+[](int) { _exit(124); })) { alarm(10); }
        ~Deadline(){alarm(0);if(previous!=SIG_ERR)signal(SIGALRM,previous);}
    } deadline;
    void *library=dlopen("/System/Library/Frameworks/QuartzCore.framework/QuartzCore",RTLD_LAZY);
    auto render=reinterpret_cast<bool (*)(mach_port_t,CFStringRef,IOSurfaceRef,uint32_t,uint32_t)>(library?dlsym(library,"CARenderServerRenderDisplay"):nullptr);
    const uint8_t uuid[]={0xfa,0xf7,0xd7,0xea,0x18,0xf0,0x3c,0x2d,0x81,0x6c,0xac,0x56,0xd1,0x6c,0x40,0xa3};
    Dl_info info{};
    if (!render || !dladdr(reinterpret_cast<void *>(render),&info) || !pinnedImage(info.dli_fbase,uuid) ||
        reinterpret_cast<uintptr_t>(render)-reinterpret_cast<uintptr_t>(info.dli_fbase)!=0x23a0fc) return 1;
    // Audited 22H374 wrapper: server port, display name, IOSurface, two int32
    // options. It validates the IOSurface type and passes its Mach port.
    constexpr size_t width=1080,height=1920,stride=4352;
    NSDictionary *properties=@{(__bridge NSString *)kIOSurfaceWidth:@(width),(__bridge NSString *)kIOSurfaceHeight:@(height),
        (__bridge NSString *)kIOSurfaceBytesPerElement:@4,(__bridge NSString *)kIOSurfaceBytesPerRow:@(stride),
        (__bridge NSString *)kIOSurfaceAllocSize:@(height*stride),(__bridge NSString *)kIOSurfacePixelFormat:@0x42475241};
    IOSurfaceRef surface=IOSurfaceCreate((__bridge CFDictionaryRef)properties);
    bool rendered=surface && render(0,CFSTR("LCD"),surface,0,0);
    printf("SETUP_LOGICAL_IMAGE rendered=%u absolute=%llu capture_source=render-server display=LCD optical_verified=0 path=%s\n",
        rendered,mach_absolute_time(),imagePath);
    int result=1;
    if (rendered && IOSurfaceGetAllocSize(surface)>=stride*height && !IOSurfaceLock(surface,kIOSurfaceLockReadOnly,nullptr)) {
        NSData *pixels=[NSData dataWithBytes:IOSurfaceGetBaseAddress(surface) length:stride*height];
        IOSurfaceUnlock(surface,kIOSurfaceLockReadOnly,nullptr);
        result=streamScanoutPNG(pixels,width,height,stride,imagePath,"render-server");
    }
    if (surface) CFRelease(surface);
    if (library) dlclose(library);
    return result;
}
static int captureScanout(const char *outputPrefix, bool stream = false,const char *imagePath=nullptr) {
    NSString *prefix = outputPrefix ? [NSString stringWithUTF8String:outputPrefix] : nil;
    if (!prefix || ![prefix hasPrefix:@"/private/var/tmp/"] || [prefix containsString:@".."] || prefix.length > 240) {
        puts("SCANOUT expected output prefix below /private/var/tmp/"); return 2;
    }
    mach_port_t iterator = 0;
    kern_return_t status = IOServiceGetMatchingServices(0, IOServiceMatching("QcomDisplay"), &iterator);
    if (status) { printf("SCANOUT matching result=0x%x\n", status); return 1; }
    mach_port_t service = IOIteratorNext(iterator), extra = IOIteratorNext(iterator);
    IOObjectRelease(iterator);
    if (!service || extra) {
        if (service) IOObjectRelease(service);
        if (extra) IOObjectRelease(extra);
        puts("SCANOUT expected exactly one display service"); return 1;
    }
    status = IORegistryEntrySetCFProperties(service, (__bridge CFTypeRef)@{@"DisplayScanoutCapture": @YES});
    printf("SCANOUT request_result=0x%x\n", status);
    CFTypeRef property = status ? nullptr : IORegistryEntryCreateCFProperty(service, CFSTR("DisplayScanoutSnapshot"), nullptr, 0);
    IOObjectRelease(service);
    NSDictionary *snapshot = property && CFGetTypeID(property) == CFDictionaryGetTypeID() ? (__bridge NSDictionary *)property : nil;
    NSData *pixels = [snapshot[@"Pixels"] isKindOfClass:[NSData class]] ? snapshot[@"Pixels"] : nil;
    NSUInteger width = [snapshot[@"Width"] unsignedIntegerValue], height = [snapshot[@"Height"] unsignedIntegerValue];
    NSUInteger stride = [snapshot[@"RowBytes"] unsignedIntegerValue];
    bool valid = [snapshot[@"Version"] unsignedIntValue] == 1 && width && height && width <= 4096 && height <= 4096 &&
        stride >= width * 4 && stride <= 65536 && pixels.length == stride * height && pixels.length <= 16 * 1024 * 1024 &&
        [snapshot[@"PixelFormat"] unsignedIntValue] == 0x42475241;
    int result = 1;
    if (valid) {
        NSMutableDictionary *metadata = [snapshot mutableCopy];
        [metadata removeObjectForKey:@"Pixels"];
        // IORegistry serializes OSNumber(32) as a signed CFNumber on this
        // firmware. Preserve register bit patterns in JSON and diagnostics.
        for (NSString *key in @[@"SourceAddress", @"SourceAddressAfter", @"SourceFormat", @"SourceUnpack",
                @"SourceOperation", @"ControlFlushBefore", @"ControlFlushAfter", @"InterruptStatus"]) {
            if (snapshot[key]) metadata[key] = @([snapshot[key] unsignedIntValue]);
        }
        metadata[@"Bytes"] = @(pixels.length);
        metadata[@"OpticalPresentationVerified"] = @NO;
        NSError *error = nil;
        NSData *json = [NSJSONSerialization dataWithJSONObject:metadata options:NSJSONWritingPrettyPrinted error:&error];
        NSString *rawPath = [prefix stringByAppendingString:@"-scanout.bgra"];
        NSString *jsonPath = [prefix stringByAppendingString:@"-scanout.json"];
        bool written = !stream && json && [pixels writeToFile:rawPath options:NSDataWritingWithoutOverwriting error:&error] &&
            [json writeToFile:jsonPath options:NSDataWritingWithoutOverwriting error:&error];
        printf("SCANOUT exported=%u generation=%u width=%lu height=%lu row=%lu bytes=%lu source=0x%llx source_after=0x%llx vbl=%llu prefix=%s error=%s\n",
            written, [snapshot[@"Generation"] unsignedIntValue], (unsigned long)width, (unsigned long)height,
            (unsigned long)stride, (unsigned long)pixels.length, (unsigned long long)[snapshot[@"SourceAddress"] unsignedIntValue],
            (unsigned long long)[snapshot[@"SourceAddressAfter"] unsignedIntValue], [snapshot[@"HardwareVBLCount"] unsignedLongLongValue],
            prefix.UTF8String, error ? error.description.UTF8String : "none");
        if (stream) {
            printf("SCANOUT_TRANSPORT valid=%u direct=%u mapper_available=%u direct_frames=%llu quarantined=%u surface=%u mapper_capture=%u\n",
                snapshot[@"DirectScanout"] != nil, [snapshot[@"DirectScanout"] unsignedIntValue],
                [snapshot[@"DirectScanoutMapperAvailable"] unsignedIntValue],
                [snapshot[@"DirectScanoutFrames"] unsignedLongLongValue],
                [snapshot[@"DirectScanoutQuarantined"] unsignedIntValue], [snapshot[@"SurfaceID"] unsignedIntValue],
                [snapshot[@"MapperCaptureStatus"] unsignedIntValue]);
            printf("SCANOUT_STATE completed=%u displayed=%u pending=%u active=%u queued=%u\n",
                [snapshot[@"NativeLastCompletedSwap"] unsignedIntValue], [snapshot[@"NativeLastDisplayedSwap"] unsignedIntValue],
                [snapshot[@"SwapPending"] unsignedIntValue], [snapshot[@"NativeActiveRequestPresent"] unsignedIntValue],
                [snapshot[@"NativeQueuedRequestPresent"] unsignedIntValue]);
            printf("SCANOUT_CLOCK valid=%u vbl_subscriber=%u last_hardware_vbl=%llu\n",
                [snapshot[@"NativeVBLSubscriberPresent"] isKindOfClass:[NSNumber class]] &&
                    [snapshot[@"LastHardwareVBLTime"] isKindOfClass:[NSNumber class]],
                [snapshot[@"NativeVBLSubscriberPresent"] unsignedIntValue],
                [snapshot[@"LastHardwareVBLTime"] unsignedLongLongValue]);
            result = streamScanoutPNG(pixels, width, height, stride,imagePath);
        } else result = written ? 0 : 1;
    } else puts("SCANOUT snapshot unavailable or invalid");
    if (property) CFRelease(property);
    return result;
}
static int sampleScanout(unsigned count,unsigned intervalMS) {
    mach_port_t iterator=0;
    auto status=IOServiceGetMatchingServices(0,IOServiceMatching("QcomDisplay"),&iterator);
    if(status){printf("SCANOUT_SAMPLES matching=0x%x\n",status);return 1;}
    auto service=IOIteratorNext(iterator),extra=IOIteratorNext(iterator);IOObjectRelease(iterator);
    if(!service || extra) {
        if(service)IOObjectRelease(service);if(extra)IOObjectRelease(extra);return 1;
    }
    printf("SCANOUT_SAMPLES_BEGIN count=%u interval_ms=%u native_content_only=1 optical_verification=0\n",count,intervalMS);
    unsigned valid=0,changed=0;uint64_t previous=0;double begin=CACurrentMediaTime();
    for(unsigned index=0;index<count;++index) {
        @autoreleasepool {
            status=IORegistryEntrySetCFProperties(service,(__bridge CFTypeRef)@{@"DisplayScanoutCapture":@YES});
            CFTypeRef property=status?nullptr:IORegistryEntryCreateCFProperty(service,CFSTR("DisplayScanoutSnapshot"),nullptr,0);
            NSDictionary *snapshot=property && CFGetTypeID(property)==CFDictionaryGetTypeID()?(__bridge NSDictionary *)property:nil;
            NSData *pixels=[snapshot[@"Pixels"] isKindOfClass:[NSData class]]?snapshot[@"Pixels"]:nil;
            NSUInteger width=[snapshot[@"Width"] unsignedIntegerValue],height=[snapshot[@"Height"] unsignedIntegerValue];
            NSUInteger stride=[snapshot[@"RowBytes"] unsignedIntegerValue];
            bool stable=snapshot && [snapshot[@"SourceAddress"] unsignedIntValue]==[snapshot[@"SourceAddressAfter"] unsignedIntValue];
            bool readable=!status && stable && [snapshot[@"Version"] unsignedIntValue]==1 && width && height &&
                width<=4096 && height<=4096 && stride>=width*4 && stride<=65536 &&
                pixels.length==stride*height && pixels.length<=16*1024*1024 && [snapshot[@"PixelFormat"] unsignedIntValue]==0x42475241;
            if(readable) {
                // Hash only visible RGB, excluding padding and unused alpha.
                // Diagnostic snapshots copy bytes; they do not present frames.
                uint64_t hash=14695981039346656037ull;unsigned nonblack=0;
                NSUInteger left=width,top=height,right=0,bottom=0;
                auto *bytes=static_cast<const uint8_t *>(pixels.bytes);
                for(NSUInteger y=0;y<height;++y)for(NSUInteger x=0;x<width;++x) {
                    auto *p=bytes+y*stride+x*4;
                    for(unsigned c=0;c<3;++c){hash^=p[c];hash*=1099511628211ull;}
                    if(p[0] || p[1] || p[2]){++nonblack;left=MIN(left,x);top=MIN(top,y);right=MAX(right,x);bottom=MAX(bottom,y);}
                }
                if(valid && hash!=previous)++changed;previous=hash;++valid;
                printf("SCANOUT_SAMPLE index=%u elapsed_ms=%.3f status=0 source=0x%x surface=%u vbl=%llu completed=%u displayed=%u native_display=%u visible=%u rgb_fnv64=%016llx nonblack=%u bounds=%lu,%lu,%lu,%lu\n",
                    index,(CACurrentMediaTime()-begin)*1000,[snapshot[@"SourceAddress"] unsignedIntValue],
                    [snapshot[@"SurfaceID"] unsignedIntValue],[snapshot[@"HardwareVBLCount"] unsignedLongLongValue],
                    [snapshot[@"NativeLastCompletedSwap"] unsignedIntValue],[snapshot[@"NativeLastDisplayedSwap"] unsignedIntValue],
                    [snapshot[@"NativeState17F0"] unsignedIntValue],[snapshot[@"DisplayVisible"] unsignedIntValue],
                    (unsigned long long)hash,nonblack,(unsigned long)left,(unsigned long)top,(unsigned long)right,(unsigned long)bottom);
            } else printf("SCANOUT_SAMPLE index=%u elapsed_ms=%.3f status=0x%x stable=%u readable=0\n",
                index,(CACurrentMediaTime()-begin)*1000,status,stable);
            if(property)CFRelease(property);
        }
        if(index+1<count)[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:double(intervalMS)/1000]];
    }
    IOObjectRelease(service);
    printf("SCANOUT_SAMPLES_END attempts=%u valid=%u rgb_changes=%u hash=fnv1a64\n",count,valid,changed);
    return valid?0:1;
}
static bool mobileGraphicsConfiguration() {
    mach_port_t iterator=0;
    if(IOServiceGetMatchingServices(0,IOServiceMatching("QcomGPU"),&iterator))return false;
    mach_port_t service=IOIteratorNext(iterator);
    IOObjectRelease(iterator);
    if(!service){puts("SURFACE_POLICY requires QcomGPU");return false;}
    IOObjectRelease(service);
    // Use the same real user as SpringBoard and its wallpaper extension.
    // No launch service, entitlement, timeout, or extension state is changed.
    if(getuid()==0 && (setgid(501) || setuid(501)))return false;
    return getuid()==501 && geteuid()==501;
}
static int surfaceCompressionPolicy(const char *operation) {
    if(strcmp(operation,"inspect") && strcmp(operation,"linear") && strcmp(operation,"restore"))return 2;
    if(!mobileGraphicsConfiguration())return 1;
    CFStringRef key=CFSTR("CAEnableIOSurfaceCompression");
    CFStringRef domain=kCFPreferencesAnyApplication,user=kCFPreferencesCurrentUser,host=kCFPreferencesAnyHost;
    NSString *backupPath=@"/private/var/mobile/Library/Preferences/org.darwin.surface-compression-backup.plist";
    NSString *preferencesPath=@"/private/var/mobile/Library/Preferences/.GlobalPreferences.plist";
    NSDictionary *disk=[NSDictionary dictionaryWithContentsOfFile:preferencesPath];
    if(!disk && [[NSFileManager defaultManager] fileExistsAtPath:preferencesPath])return 1;
    if(strcmp(operation,"inspect")) {
        // QuartzCore 22H374, UUID FAF7D7EA-18F0-3C2D-816C-AC56D16C40A3:
        // its original initializer at RVA 0x10c4b4 infers compression from
        // ChipID, then honors this CFPreferences boolean at RVA 0x10c588.
        // The A540 backend cannot import Apple's '&BGA' compressed layout.
        // Keep the setting reversible and let original QuartzCore allocate BGRA.
        id original=disk[(__bridge NSString *)key];
        NSMutableDictionary *settings=[disk mutableCopy]?:[NSMutableDictionary new];
        NSDictionary *backup=[NSDictionary dictionaryWithContentsOfFile:backupPath];
        if(!strcmp(operation,"linear")) {
            if(!backup) {
                if([[NSFileManager defaultManager] fileExistsAtPath:backupPath])return 1;
                backup=original?@{@"version":@1,@"present":@YES,@"value":original}:@{@"version":@1,@"present":@NO};
                if(![backup writeToFile:backupPath atomically:YES])return 1;
            }
            if(![backup[@"version"] isEqual:@1] || ![backup[@"present"] isKindOfClass:NSNumber.class])return 1;
            settings[(__bridge NSString *)key]=@NO;
        } else {
            if(![backup[@"version"] isEqual:@1] || ![backup[@"present"] isKindOfClass:NSNumber.class] ||
                ([backup[@"present"] boolValue] && !backup[@"value"]))return 1;
            if(original && ![original isEqual:@NO]){puts("SURFACE_POLICY changed externally; restore refused");return 1;}
            if([backup[@"present"] boolValue])settings[(__bridge NSString *)key]=backup[@"value"];
            else [settings removeObjectForKey:(__bridge NSString *)key];
        }
        // The native CFPreferences writer rejected synchronization from this
        // development console on 22H374. Stage the same native preference file
        // atomically, preserving all unrelated keys, and require a reboot.
        // A disk readback is not proof that a running process reloaded it.
        if(![settings writeToFile:preferencesPath atomically:YES])return 1;
        int fd=open(preferencesPath.fileSystemRepresentation,O_RDONLY);
        if(fd<0)return 1;
        int flushed=fsync(fd);close(fd);if(flushed)return 1;
        disk=[NSDictionary dictionaryWithContentsOfFile:preferencesPath];
        if(![disk isEqualToDictionary:settings])return 1;
        puts("SURFACE_POLICY disk_readback=verified reboot_required=1");
        if(!strcmp(operation,"restore") && ![[NSFileManager defaultManager] removeItemAtPath:backupPath error:nil])return 1;
    }
    Boolean valid=false;
    bool value=CFPreferencesGetAppBooleanValue(key,kCFPreferencesCurrentApplication,&valid);
    id global=CFBridgingRelease(CFPreferencesCopyValue(key,domain,user,host));
    void *gestalt=dlopen("/usr/lib/libMobileGestalt.dylib",RTLD_LAZY);
    auto answer=reinterpret_cast<CFTypeRef (*)(CFStringRef)>(gestalt?dlsym(gestalt,"MGCopyAnswer"):nullptr);
    id chip=answer?CFBridgingRelease(answer(CFSTR("ChipID"))):nil;
    printf("SURFACE_POLICY operation=%s uid=%u chip=%s disk=%s global=%s effective_valid=%u effective_compression=%u backup=%s\n",
        operation,getuid(),[chip description].UTF8String?:"(none)",[disk[(__bridge NSString *)key] description].UTF8String?:"(absent)",[global description].UTF8String?:"(absent)",
        valid,value,backupPath.UTF8String);
    return 0;
}
static int metalDrawableCheck(bool linear) {
    // Diagnostic GPU access requires its original root identity. The separate
    // policy command runs as mobile; this probe changes only a volatile domain.
    if(getuid()!=0 || setenv("CA_SYSTEM_SESSION_CLIENT","1",1))return 1;
    struct Deadline {
        Deadline(){signal(SIGALRM,+[](int){const char m[]="DRAWABLE_CHECK_TIMEOUT\n";(void)write(2,m,sizeof(m)-1);_exit(124);});alarm(15);}
        ~Deadline(){alarm(0);}
    } deadline;
    if(linear) {
        // Volatile argument-domain override: this A/B probe writes no preference.
        NSUserDefaults *defaults=[NSUserDefaults standardUserDefaults];
        NSMutableDictionary *arguments=[[defaults volatileDomainForName:NSArgumentDomain] mutableCopy]?:[NSMutableDictionary new];
        arguments[@"CAEnableIOSurfaceCompression"]=@NO;
        [defaults setVolatileDomain:arguments forName:NSArgumentDomain];
    }
    Boolean valid=false;
    bool enabled=CFPreferencesGetAppBooleanValue(CFSTR("CAEnableIOSurfaceCompression"),kCFPreferencesCurrentApplication,&valid);
    printf("DRAWABLE_CHECK_BEGIN uid=%u linear_override=%u preference_valid=%u compression=%u\n",getuid(),linear,valid,enabled);
    if(linear && (!valid || enabled))return 1;
    @try {
        id<MTLDevice> device=MTLCreateSystemDefaultDevice();
        if(!device || ![device.name isEqualToString:@"Adreno 540"])return 1;
        CAMetalLayer *layer=[CAMetalLayer layer];
        layer.device=device;layer.pixelFormat=MTLPixelFormatBGRA8Unorm;layer.drawableSize=CGSizeMake(1080,1920);
        layer.framebufferOnly=YES;layer.presentsWithTransaction=YES;
        [layer setValue:@NO forKey:@"allowsDisplayCompositing"];
        id<CAMetalDrawable> drawable=[layer nextDrawable];
        id<MTLTexture> texture=drawable.texture;
        IOSurfaceRef surface=texture.iosurface;
        const uint32_t format=surface?IOSurfaceGetPixelFormat(surface):0;
        printf("DRAWABLE_CHECK_END drawable=%u texture=%u width=%lu height=%lu format=0x%08x pitch=%lu bytes=%lu presented=0\n",
            drawable!=nil,texture!=nil,(unsigned long)texture.width,(unsigned long)texture.height,format,
            (unsigned long)(surface?IOSurfaceGetBytesPerRow(surface):0),(unsigned long)(surface?IOSurfaceGetAllocSize(surface):0));
        return texture && surface && texture.width==1080 && texture.height==1920 && format==0x42475241?0:1;
    } @catch(NSException *error) {
        printf("DRAWABLE_CHECK_EXCEPTION %s %s\n",error.name.UTF8String,error.reason.UTF8String);return 1;
    }
}
static int displayBootstrapState;
static int displayBootstrapResult = 1;
static int screenInfoState;
static int screenInfoResult = 1;
static void *screenInfoWorker(void *) {
    puts("SCREEN_INFO_BEGIN native BackBoardServices / MobileGestalt");
    void *library = dlopen("/System/Library/PrivateFrameworks/BackBoardServices.framework/BackBoardServices", RTLD_LAZY);
    void *symbol = library ? dlsym(library, "BKSDisplayServicesGetMainScreenInfo") : nullptr;
    Dl_info image = {};
    if (symbol && dladdr(symbol, &image) && image.dli_fbase) {
        const uint8_t uuid[] = {0x3a,0x43,0x8d,0x91,0x42,0xc7,0x3a,0x13,0xb8,0xa7,0x8a,0xef,0x17,0xb6,0x97,0xc7};
        bool pinned = pinnedImage(image.dli_fbase, uuid);
        auto entry = static_cast<const uint8_t *>(image.dli_fbase) + 0x17e4;
        const uint8_t prefix[] = {0xff,0x83,0x02,0xd1,0xfa,0x67,0x05,0xa9,0xf8,0x5f,0x06,0xa9,0xf6,0x57,0x07,0xa9};
        auto bootstrap = reinterpret_cast<mach_port_t *>(dlsym(RTLD_DEFAULT, "bootstrap_port"));
        auto lookup = reinterpret_cast<kern_return_t (*)(mach_port_t, const char *, uid_t, mach_port_t *)>(dlsym(RTLD_DEFAULT, "bootstrap_look_up_per_user"));
        auto releasePort = reinterpret_cast<kern_return_t (*)(mach_port_t, mach_port_name_t)>(dlsym(RTLD_DEFAULT, "mach_port_deallocate"));
        if (pinned && !memcmp(entry, prefix, sizeof(prefix)) && bootstrap && lookup && releasePort) {
            mach_port_t port = 0;
            kern_return_t result = lookup(*bootstrap, "com.apple.backboard.display.services", 501, &port);
            printf("BKS_SCREEN_LOOKUP uid=501 result=0x%x port=0x%x\n", result, port);
            if (!result && port) {
                // Audited diagnostic alias for this UUID's original MIG stub.
                // Only the destination comes from a per-user lookup; native
                // message validation, server authorization and reply stay intact.
                using Query = kern_return_t (*)(mach_port_t, uint32_t *, uint32_t *, uint32_t *, uint32_t *, uint8_t *, uint8_t *);
                uint32_t values[4] = {};
                uint8_t flags[2] = {};
                result = reinterpret_cast<Query>(const_cast<uint8_t *>(entry))(port,
                    &values[0], &values[1], &values[2], &values[3], &flags[0], &flags[1]);
                float width, height, scale, fourth;
                memcpy(&width, &values[0], sizeof(width)); memcpy(&height, &values[1], sizeof(height));
                memcpy(&scale, &values[2], sizeof(scale)); memcpy(&fourth, &values[3], sizeof(fourth));
                printf("BKS_SCREEN_INFO result=0x%x width=%.9g height=%.9g scale=%.9g fourth=0x%x/%.9g flags=%u,%u\n",
                    result, width, height, scale, values[3], fourth, flags[0], flags[1]);
                screenInfoResult = result ? 1 : 0;
            }
            if (port) releasePort(mach_task_self(), port);
        } else puts("BKS_SCREEN_INFO native UUID/ABI or lookup APIs unavailable");
    } else puts("BKS_SCREEN_INFO library unavailable");
    void *gestalt = dlopen("/usr/lib/libMobileGestalt.dylib", RTLD_LAZY);
    auto answer = reinterpret_cast<CFTypeRef (*)(CFStringRef)>(gestalt ? dlsym(gestalt, "MGCopyAnswer") : nullptr);
    if (answer) for (NSString *key in @[@"MainScreenWidth", @"MainScreenHeight", @"MainScreenScale", @"MainScreenPitch",
            @"main-screen-width", @"main-screen-height", @"main-screen-scale", @"main-screen-pitch", @"main-screen-orientation", @"main-screen-class", @"DeviceClass", @"DeviceClassNumber", @"ProductType"]) {
        CFTypeRef value = answer((__bridge CFStringRef)key);
        printf("SCREEN_GESTALT %s=%s\n", key.UTF8String, value ? [[(__bridge id)value description] UTF8String] : "(none)");
        if (value) CFRelease(value);
    }
    __atomic_store_n(&screenInfoState, 2, __ATOMIC_RELEASE);
    return nullptr;
}
static void *displayBootstrapWorker(void *) {
    auto bootstrap = reinterpret_cast<mach_port_t *>(dlsym(RTLD_DEFAULT, "bootstrap_port"));
    auto lookup = reinterpret_cast<kern_return_t (*)(mach_port_t, const char *, uid_t, mach_port_t *)>(dlsym(RTLD_DEFAULT, "bootstrap_look_up_per_user"));
    auto releasePort = reinterpret_cast<kern_return_t (*)(mach_port_t, mach_port_name_t)>(dlsym(RTLD_DEFAULT, "mach_port_deallocate"));
    auto sessions = reinterpret_cast<bool (*)()>(dlsym(RTLD_DEFAULT, "xpc_user_sessions_enabled"));
    auto foreground = reinterpret_cast<uid_t (*)(int *)>(dlsym(RTLD_DEFAULT, "xpc_user_sessions_get_foreground_uid"));
    if (bootstrap && lookup && releasePort) {
        int error = 0;
        uid_t uid = foreground ? foreground(&error) : uid_t(-1);
        printf("CA_SESSION enabled=%d foreground_uid=%u error=%d\n", sessions ? sessions() : -1, uid, error);
        // The original backboardd launch job runs as mobile (UID 501). Probe
        // service visibility without changing this process's uid or bootstrap.
        uid_t users[] = {0, 501, uid};
        for (unsigned i = 0; i < 3; ++i) {
            if (users[i] == uid_t(-1) || (i == 2 && (uid == 0 || uid == 501))) continue;
            mach_port_t port = 0;
            kern_return_t result = lookup(*bootstrap, "com.apple.CARenderServer", users[i], &port);
            printf("CA_USER_LOOKUP uid=%u result=0x%x port=0x%x\n", users[i], result, port);
            if (port) { displayBootstrapResult = 0; releasePort(mach_task_self(), port); }
        }
    }
    __atomic_store_n(&displayBootstrapState, 2, __ATOMIC_RELEASE);
    return nullptr;
}
static void displayModes(id display) {
    for (NSString *key in @[@"name", @"deviceName", @"displayId", @"nativeOrientation", @"currentOrientation", @"pointScale", @"tag", @"scale", @"nativeScale", @"bounds", @"physicalBounds", @"isConnected", @"isMainDisplay", @"isActive", @"isEnabled", @"isBlanked", @"isSleeping", @"isValid", @"status"]) {
        if (![display respondsToSelector:NSSelectorFromString(key)]) continue;
        @try { printf("CADISPLAY_VALUE %s=%s\n", key.UTF8String, [[[display valueForKey:key] description] UTF8String]); }
        @catch (NSException *) { }
    }
    for (NSString *name in @[@"currentMode", @"availableModes"]) {
        SEL selector = NSSelectorFromString(name);
        if (![display respondsToSelector:selector]) continue;
        id value = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(display, selector);
        printf("CADISPLAY_MODES %s=%s\n", name.UTF8String, [[value description] UTF8String]);
        NSArray *modes = [value isKindOfClass:[NSArray class]] ? value : value ? @[value] : @[];
        for (id mode in modes) {
            unsigned long long dimensions[3] = {};
            unsigned i = 0;
            for (NSString *key in @[@"width", @"height", @"preferredScale"]) {
                SEL getter = NSSelectorFromString(key);
                if ([mode respondsToSelector:getter]) dimensions[i] = reinterpret_cast<unsigned long long (*)(id, SEL)>(objc_msgSend)(mode, getter);
                ++i;
            }
            SEL refresh = NSSelectorFromString(@"refreshRate");
            double hz = [mode respondsToSelector:refresh] ? reinterpret_cast<double (*)(id, SEL)>(objc_msgSend)(mode, refresh) : 0;
            printf("CADISPLAY_MODE width=%llu height=%llu preferredScale=%llu refreshRate=%.9g\n", dimensions[0], dimensions[1], dimensions[2], hz);
        }
    }
}
static int displays(bool systemSession = false) {
    if (systemSession) {
        // This firmware's native QuartzCore gate reads this before its first
        // server-port lookup. Keep policy, lookup and MIG handling native.
        auto nativeSetenv = reinterpret_cast<int (*)(const char *, const char *, int)>(dlsym(RTLD_DEFAULT, "setenv"));
        if (!nativeSetenv || nativeSetenv("CA_SYSTEM_SESSION_CLIENT", "1", 1)) return 1;
    }
    auto bootstrap = reinterpret_cast<mach_port_t *>(dlsym(RTLD_DEFAULT, "bootstrap_port"));
    auto lookup = reinterpret_cast<kern_return_t (*)(mach_port_t, const char *, mach_port_t *)>(dlsym(RTLD_DEFAULT, "bootstrap_look_up"));
    auto releasePort = reinterpret_cast<kern_return_t (*)(mach_port_t, mach_port_name_t)>(dlsym(RTLD_DEFAULT, "mach_port_deallocate"));
    if (bootstrap && lookup && releasePort) {
        mach_port_t port = 0;
        kern_return_t result = lookup(*bootstrap, "com.apple.CARenderServer", &port);
        printf("CA_SERVER_LOOKUP bootstrap=0x%x result=0x%x port=0x%x\n", *bootstrap, result, port);
        if (port) releasePort(mach_task_self(), port);
    }
    Class cls = NSClassFromString(@"CADisplay");
    if (!cls) {
        // Loading QuartzCore preserves its own display discovery implementation.
        NSBundle *bundle = [NSBundle bundleWithPath:@"/System/Library/Frameworks/QuartzCore.framework"];
        [bundle load]; cls = NSClassFromString(@"CADisplay");
    }
    SEL selector = NSSelectorFromString(@"displays");
    if (!cls || ![cls respondsToSelector:selector]) { puts("CADISPLAY unavailable"); return 1; }
    @try {
        SEL refresh = NSSelectorFromString(@"updateDisplays");
        if ([cls respondsToSelector:refresh])
            reinterpret_cast<void (*)(id, SEL)>(objc_msgSend)(cls, refresh);
        id array = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(cls, selector);
        printf("CADISPLAY_DISPLAYS %s\n", [[array description] UTF8String]);
        SEL main = NSSelectorFromString(@"mainDisplay");
        if ([cls respondsToSelector:main]) {
            id display = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(cls, main);
            printf("CADISPLAY_MAIN %s\n", [[display description] UTF8String]);
            displayModes(display);
            SEL immutable = NSSelectorFromString(@"immutableCopy");
            if ([display respondsToSelector:immutable]) {
                id copy = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(display, immutable);
                printf("CADISPLAY_IMMUTABLE %s\n", [[copy description] UTF8String]);
                displayModes(copy);
            }
        }
        if (![array isKindOfClass:[NSArray class]]) return 1;
        printf("CADISPLAY_COUNT %lu\n", (unsigned long)[array count]);
        for (id display in array) {
            printf("CADISPLAY_OBJECT %s\n", [[display description] UTF8String]);
            displayModes(display);
        }
        return [array count] ? 0 : 1;
    } @catch (NSException *error) {
        printf("CADISPLAY_EXCEPTION %s %s\n", error.name.UTF8String, error.reason.UTF8String); return 1;
    }
}
static void *framebufferLibrary;
static int framebufferNative() {
    if (!framebufferLibrary) framebufferLibrary = dlopen("/System/Library/PrivateFrameworks/IOMobileFramebuffer.framework/IOMobileFramebuffer", RTLD_LAZY);
    void *library = framebufferLibrary;
    if (!library) { printf("IOMFB_LIBRARY_ERROR %s\n", dlerror()); return 1; }
    using GetMain = kern_return_t (*)(CFTypeRef *);
    using GetSize = kern_return_t (*)(CFTypeRef, CGSize *);
    auto getMain = reinterpret_cast<GetMain>(dlsym(library, "IOMobileFramebufferGetMainDisplay"));
    auto getSize = reinterpret_cast<GetSize>(dlsym(library, "IOMobileFramebufferGetDisplaySize"));
    if (!getMain || !getSize) { puts("IOMFB_NATIVE_API unavailable"); return 1; }
    CFTypeRef display = nullptr;
    kern_return_t result = getMain(&display);
    printf("IOMFB_MAIN result=0x%x display=%p\n", result, display);
    if (result || !display) return 1;
    CGSize size = {};
    result = getSize(display, &size);
    printf("IOMFB_SIZE result=0x%x width=%.0f height=%.0f\n", result, size.width, size.height);
    auto getPitch = reinterpret_cast<kern_return_t (*)(CFTypeRef, uint32_t *)>(dlsym(library, "IOMobileFramebufferGetDotPitch"));
    if (getPitch) {
        uint32_t pitch = 0;
        kern_return_t status = getPitch(display, &pitch);
        printf("IOMFB_DOT_PITCH result=0x%x ppi=%u\n", status, pitch);
    }
    return result ? 1 : 0;
}
using NativeSwapWait = kern_return_t (*)(CFTypeRef, uint32_t, uint32_t, double);
struct SwapRetirementWait {
    NativeSwapWait wait;
    CFTypeRef display;
    uint32_t token;
    int entered;
    kern_return_t result;
    double milliseconds;
};
static void *waitForSwapRetirement(void *opaque) {
    auto context = static_cast<SwapRetirementWait *>(opaque);
    double start = CACurrentMediaTime();
    __atomic_store_n(&context->entered, 1, __ATOMIC_RELEASE);
    context->result = context->wait(context->display, context->token, 0, 2.0);
    context->milliseconds = (CACurrentMediaTime() - start) * 1000.0;
    return nullptr;
}
static IOSurfaceRef copySwapSurface(IOSurfaceRef source, CFDictionaryRef properties, size_t width, size_t height) {
    IOSurfaceRef copy = IOSurfaceCreate(properties);
    if (!copy) return nullptr;
    size_t row = IOSurfaceGetBytesPerRow(copy);
    if (IOSurfaceGetWidth(copy) != width || IOSurfaceGetHeight(copy) != height ||
            IOSurfaceGetPixelFormat(copy) != 0x42475241u || IOSurfaceGetPlaneCount(copy) > 1 ||
            row < width * 4 || row > 65536 || IOSurfaceGetAllocSize(copy) < row * height ||
            IOSurfaceGetID(copy) == IOSurfaceGetID(source)) { CFRelease(copy); return nullptr; }
    if (IOSurfaceLock(source, kIOSurfaceLockReadOnly, nullptr)) { CFRelease(copy); return nullptr; }
    if (IOSurfaceLock(copy, 0, nullptr)) {
        IOSurfaceUnlock(source, kIOSurfaceLockReadOnly, nullptr); CFRelease(copy); return nullptr;
    }
    auto from = static_cast<const uint8_t *>(IOSurfaceGetBaseAddress(source));
    auto to = static_cast<uint8_t *>(IOSurfaceGetBaseAddress(copy));
    if (from && to) for (size_t y = 0; y < height; ++y)
        memcpy(to + y * row, from + y * IOSurfaceGetBytesPerRow(source), width * 4);
    kern_return_t copied = IOSurfaceUnlock(copy, 0, nullptr);
    kern_return_t read = IOSurfaceUnlock(source, kIOSurfaceLockReadOnly, nullptr);
    if (!from || !to || copied || read) { CFRelease(copy); return nullptr; }
    return copy;
}
static bool renderSwapSurface(IOSurfaceRef surface, size_t width, size_t height) {
    // Render directly into the surface submitted below. No CPU pixel upload
    // or intermediate software image can satisfy this diagnostic.
    @try {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        auto queue = [device newCommandQueue];
        auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
            width:width height:height mipmapped:NO];
        descriptor.storageMode = MTLStorageModeShared;
        descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        auto texture = [device newTextureWithDescriptor:descriptor iosurface:surface plane:0];
        if (!device || !queue || !texture) { puts("SWAP_TEST_GPU_DRAW allocation_failed=1"); return false; }
        auto renderer = [CARenderer rendererWithMTLTexture:texture options:@{kCARendererMetalCommandQueue:queue}];
        if (!renderer) return false;
        CGColorSpaceRef colors = CGColorSpaceCreateDeviceRGB();
        const CGFloat background[] = {0.8, 0.05, 0.05, 1}, foreground[] = {1, 1, 1, 1};
        CGColorRef red = colors ? CGColorCreate(colors, background) : nullptr;
        CGColorRef white = colors ? CGColorCreate(colors, foreground) : nullptr;
        if (colors) CGColorSpaceRelease(colors);
        if (!red || !white) { if (red) CGColorRelease(red); if (white) CGColorRelease(white); return false; }
        auto root = [CALayer layer], bar = [CALayer layer];
        [CATransaction begin]; [CATransaction setDisableActions:YES];
        root.frame = CGRectMake(0, 0, width, height); root.opaque = YES; root.backgroundColor = red;
        bar.frame = CGRectMake(width / 4.0, height / 2.0 - 20, width / 2.0, 40);
        bar.opaque = YES; bar.backgroundColor = white; [root addSublayer:bar];
        renderer.layer = root; renderer.bounds = root.bounds;
        [CATransaction commit]; [CATransaction flush];
        CGColorRelease(red); CGColorRelease(white);
        [renderer beginFrameAtTime:CACurrentMediaTime() timeStamp:nullptr];
        [renderer addUpdateRect:renderer.bounds]; [renderer render]; [renderer endFrame];
        // The supplied serial queue places this marker after CARenderer's
        // committed work and waits for real native completion/publication.
        auto completion = [queue commandBuffer];
        if (!completion) return false;
        [completion commit]; [completion waitUntilCompleted];
        printf("SWAP_TEST_GPU_DRAW device=%s registry=%llu status=%lu surface_id=%u row=%zu error=%s system_ui_unverified=1\n",
            device.name.UTF8String, (unsigned long long)device.registryID, (unsigned long)completion.status,
            IOSurfaceGetID(surface), IOSurfaceGetBytesPerRow(surface),
            completion.error ? completion.error.description.UTF8String : "none");
        return completion.status == MTLCommandBufferStatusCompleted;
    } @catch (NSException *exception) {
        printf("SWAP_TEST_GPU_DRAW exception=%s reason=%s\n", exception.name.UTF8String, exception.reason.UTF8String);
        return false;
    }
}
static int swapTest(const char *capturePrefix = nullptr, bool retirement = false, bool gpu = false) {
    if (!framebufferLibrary) framebufferLibrary = dlopen("/System/Library/PrivateFrameworks/IOMobileFramebuffer.framework/IOMobileFramebuffer", RTLD_LAZY);
    const uint8_t uuid[] = {0xe2,0x31,0x18,0x84,0x56,0xcd,0x33,0x79,0x88,0x2d,0xc8,0xc6,0xa6,0xcd,0x0a,0x98};
    struct Entry { const char *name; uintptr_t offset; void *symbol; } apis[] = {
        {"IOMobileFramebufferGetMainDisplay", 0xa17c, nullptr},
        {"IOMobileFramebufferGetDisplaySize", 0x5010, nullptr},
        {"IOMobileFramebufferGetLayerDefaultSurface", 0x4fd8, nullptr},
        {"IOMobileFramebufferSwapBegin", 0x1cdc, nullptr},
        {"IOMobileFramebufferSwapSetLayer", 0x1cf8, nullptr},
        {"IOMobileFramebufferSwapEnd", 0x1d14, nullptr},
        {"IOMobileFramebufferSwapWaitWithTimeout", 0x5464, nullptr}};
    // These original export offsets and their legacy implementations were
    // audited against the same UUID; do not send a guessed raw IOKit request.
    for (auto &api : apis) {
        api.symbol = framebufferLibrary ? dlsym(framebufferLibrary, api.name) : nullptr;
        Dl_info image = {};
        if (!api.symbol || !dladdr(api.symbol, &image) || !pinnedImage(image.dli_fbase, uuid) ||
                reinterpret_cast<uintptr_t>(api.symbol) - reinterpret_cast<uintptr_t>(image.dli_fbase) != api.offset) {
            printf("SWAP_TEST_ABI unavailable=%s\n", api.name); return 1;
        }
    }
    auto getMain = reinterpret_cast<kern_return_t (*)(CFTypeRef *)>(apis[0].symbol);
    auto getSize = reinterpret_cast<kern_return_t (*)(CFTypeRef, CGSize *)>(apis[1].symbol);
    auto getDefault = reinterpret_cast<kern_return_t (*)(CFTypeRef, uint32_t, IOSurfaceRef *)>(apis[2].symbol);
    auto begin = reinterpret_cast<kern_return_t (*)(CFTypeRef, uint32_t *)>(apis[3].symbol);
    auto setLayer = reinterpret_cast<kern_return_t (*)(CFTypeRef, uint32_t, IOSurfaceRef, CGRect, CGRect, uint32_t)>(apis[4].symbol);
    auto end = reinterpret_cast<kern_return_t (*)(CFTypeRef)>(apis[5].symbol);
    auto wait = reinterpret_cast<NativeSwapWait>(apis[6].symbol);
    CFTypeRef display = nullptr;
    kern_return_t status = getMain(&display);
    printf("SWAP_TEST_OPEN result=0x%x display=%p\n", status, display);
    if (status || !display) return 1;
    CGSize size = {};
    status = getSize(display, &size);
    printf("SWAP_TEST_SIZE result=0x%x width=%.9g height=%.9g\n", status, size.width, size.height);
    if (status || !isfinite(size.width) || !isfinite(size.height) || size.width < 1 || size.height < 1 ||
            size.width > 4096 || size.height > 4096 || floor(size.width) != size.width || floor(size.height) != size.height) {
        CFRelease(display); return 1;
    }
    IOSurfaceRef initial = nullptr; // Borrowed from the original display object.
    status = getDefault(display, 0, &initial);
    printf("SWAP_TEST_DEFAULT result=0x%x surface=%p id=%u\n", status, initial, initial ? IOSurfaceGetID(initial) : 0);
    size_t width = size.width, height = size.height, stride = (width * 4 + 63) & ~size_t(63);
    if (stride * height > 32 * 1024 * 1024) { CFRelease(display); return 1; }
    NSDictionary *properties = @{
        (__bridge NSString *)kIOSurfaceWidth: @(width),
        (__bridge NSString *)kIOSurfaceHeight: @(height),
        (__bridge NSString *)kIOSurfaceBytesPerElement: @4,
        (__bridge NSString *)kIOSurfaceBytesPerRow: @(stride),
        (__bridge NSString *)kIOSurfaceAllocSize: @(stride * height),
        (__bridge NSString *)kIOSurfacePixelFormat: @(0x42475241u)};
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
    if (!surface) { puts("SWAP_TEST_SURFACE unavailable"); CFRelease(display); return 1; }
    stride = IOSurfaceGetBytesPerRow(surface);
    if (IOSurfaceGetWidth(surface) != width || IOSurfaceGetHeight(surface) != height ||
            IOSurfaceGetPixelFormat(surface) != 0x42475241u || IOSurfaceGetPlaneCount(surface) > 1 ||
            stride < width * 4 || stride > 65536 || IOSurfaceGetAllocSize(surface) < stride * height) {
        puts("SWAP_TEST_SURFACE unexpected layout"); CFRelease(surface); CFRelease(display); return 1;
    }
    if (gpu) {
        if (!renderSwapSurface(surface, width, height)) { CFRelease(surface); CFRelease(display); return 1; }
    } else {
        status = IOSurfaceLock(surface, 0, nullptr);
        printf("SWAP_TEST_LOCK result=0x%x\n", status);
        if (status) { CFRelease(surface); CFRelease(display); return 1; }
        void *pixels = IOSurfaceGetBaseAddress(surface);
        CGColorSpaceRef colors = CGColorSpaceCreateDeviceRGB();
        CGContextRef bitmap = colors && pixels ? CGBitmapContextCreate(pixels, width, height, 8, stride,
            colors, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little) : nullptr;
        if (bitmap) {
            CGContextSetRGBFillColor(bitmap, 0.8, 0.05, 0.05, 1);
            CGContextFillRect(bitmap, CGRectMake(0, 0, width, height));
            CGContextSetRGBFillColor(bitmap, 1, 1, 1, 1);
            CGContextFillRect(bitmap, CGRectMake(width / 4.0, height / 2.0 - 20, width / 2.0, 40));
            CGContextFlush(bitmap); CGContextRelease(bitmap);
        }
        if (colors) CGColorSpaceRelease(colors);
        if (bitmap) {
            // Keep the exact software pixels for a row-by-row comparison against
            // the DPU readback; its allocation has a different row alignment.
            NSData *reference = [NSData dataWithBytesNoCopy:pixels length:stride * height freeWhenDone:NO];
            NSError *error = nil;
            NSString *referencePath = capturePrefix ? [[NSString stringWithUTF8String:capturePrefix] stringByAppendingString:@"-source.bgra"] :
                @"/private/var/tmp/displayctl-swap-source.bgra";
            bool saved = [reference writeToFile:referencePath options:NSDataWritingAtomic error:&error];
            printf("SWAP_TEST_REFERENCE saved=%u bytes=%zu row=%zu error=%s\n", saved, stride * height, stride,
                error ? error.description.UTF8String : "none");
        }
        status = IOSurfaceUnlock(surface, 0, nullptr);
        printf("SWAP_TEST_SOFTWARE_DRAW context_created=%u unlock_result=0x%x surface_id=%u row=%zu\n",
            bitmap != nullptr, status, IOSurfaceGetID(surface), stride);
        if (!bitmap || status) { CFRelease(surface); CFRelease(display); return 1; }
    }
    // Finish rendering/file export first, then retain the original native
    // display control through submission and GPU-frame capture.
    if (capturePrefix || gpu) displayState(true);
    uint32_t token = 0;
    status = begin(display, &token);
    printf("SWAP_TEST_BEGIN result=0x%x token=%u\n", status, token);
    CGRect full = CGRectMake(0, 0, width, height);
    if (!status) { status = setLayer(display, 0, surface, full, full, 0); printf("SWAP_TEST_SET_LAYER result=0x%x\n", status); }
    if (!status) { status = end(display); printf("SWAP_TEST_END result=0x%x\n", status); }
    if (!status) {
        // Original swap_wait_gated option1 waits for queued/active work.
        // Option0 also waits while this token remains the current scanout.
        status = wait(display, token, 1, 2.0);
        printf("SWAP_TEST_WAIT result=0x%x flags=1 timeout_seconds=2 physical_presentation_unverified=1\n", status);
        fflush(stdout);
    }
    if (retirement && !status) {
        IOSurfaceRef replacement = copySwapSurface(surface, (__bridge CFDictionaryRef)properties, width, height);
        if (!replacement) { puts("SWAP_RETIREMENT_COPY unavailable"); status = 1; }
        else {
            // Both surfaces stay retained through their native waits. A second
            // real submission must retire the first; no synthetic wake is sent.
            SwapRetirementWait context = {wait, display, token, 0, 0, 0};
            pthread_t worker;
            int created = pthread_create(&worker, nullptr, waitForSwapRetirement, &context);
            printf("SWAP_RETIREMENT_WORKER created=%d first_token=%u first_surface=%u replacement_surface=%u\n",
                created, token, IOSurfaceGetID(surface), IOSurfaceGetID(replacement));
            if (created) status = created;
            else {
                while (!__atomic_load_n(&context.entered, __ATOMIC_ACQUIRE)) usleep(1000);
                usleep(100000);
                uint32_t next = 0;
                status = begin(display, &next);
                kern_return_t started = status, layer = KERN_FAILURE, ended = KERN_FAILURE, completed = KERN_FAILURE;
                if (!status) status = layer = setLayer(display, 0, replacement, full, full, 0);
                if (!status) status = ended = end(display);
                if (!status) status = completed = wait(display, next, 1, 2.0);
                printf("SWAP_RETIREMENT_SUBMIT begin=0x%x layer=0x%x end=0x%x completion=0x%x flags=1 token=%u\n",
                    started, layer, ended, completed, next);
                pthread_join(worker, nullptr);
                printf("SWAP_RETIREMENT_WAIT result=0x%x flags=0 first_token=%u replacement_token=%u elapsed_ms=%.3f\n",
                    context.result, token, next, context.milliseconds);
                if (!status) status = context.result;
            }
            CFRelease(replacement);
        }
    }
    int captureResult = 0;
    if (gpu && !status) {
        printf("SWAP_TEST_GPU_CAPTURE token=%u source=CARenderer/Metal/IOSurface\n", token);
        captureResult = captureScanout("/private/var/tmp/display-gpu", true);
    }
    if (capturePrefix) {
        NSString *after = [[NSString stringWithUTF8String:capturePrefix] stringByAppendingString:@"-after"];
        captureResult = captureScanout(after.UTF8String);
        stacks(false, true);
    }
    if (!status) [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:60]];
    CFRelease(surface); CFRelease(display);
    return status || captureResult ? 1 : 0;
}
static int validateScanout(const char *prefix, bool retirement = false) {
    NSString *path = prefix ? [NSString stringWithUTF8String:prefix] : nil;
    if (!path || ![path hasPrefix:@"/private/var/tmp/"] || [path containsString:@".."] || path.length > 220) return 2;
    int before = captureScanout([path stringByAppendingString:@"-before"].UTF8String);
    int power = displayState(true);
    int on = captureScanout([path stringByAppendingString:@"-on"].UTF8String);
    int swap = swapTest(prefix, retirement);
    if (activeDisplayControl) describeNativeObject("state-control-validation-end", activeDisplayControl,
        @[@"displayState", @"powerState", @"targetDisplayState", @"targetPowerState"]);
    activeDisplayControl = nil;
    printf("SCANOUT_VALIDATION before=%d native_power=%d on=%d software_swap=%d optical_verification=0\n", before, power, on, swap);
    return before || power || on || swap ? 1 : 0;
}
static int validateWindow(const char *prefix) {
    NSString *path = prefix ? [NSString stringWithUTF8String:prefix] : nil;
    if (!path || ![path hasPrefix:@"/private/var/tmp/"] || [path containsString:@".."] || path.length > 220) return 2;
    puts("NATIVE_WINDOW_BEGIN native_content_only=1 hold_seconds=60");
    int before = captureScanout([path stringByAppendingString:@"-before"].UTF8String);
    int power = displayState(true);
    // Native on-state can precede the compositor's real transfer completion.
    // Keep the state control alive while the original window finishes drawing.
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:3]];
    int on = captureScanout([path stringByAppendingString:@"-on"].UTF8String);
    int snapshot = stacks(false, true);
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:60]];
    int after = captureScanout([path stringByAppendingString:@"-after"].UTF8String);
    if (activeDisplayControl) describeNativeObject("state-control-validation-end", activeDisplayControl,
        @[@"displayState", @"powerState", @"targetDisplayState", @"targetPowerState"]);
    activeDisplayControl = nil;
    printf("NATIVE_WINDOW_VALIDATION before=%d native_power=%d on=%d stackshot=%d after=%d optical_verification=0\n",
        before, power, on, snapshot, after);
    return before || power || on || snapshot || after ? 1 : 0;
}
static int framebufferState;
static int framebufferResult;
static void *framebufferWorker(void *) {
    framebufferResult = framebufferNative();
    __atomic_store_n(&framebufferState, 2, __ATOMIC_RELEASE);
    return nullptr;
}
static int keybagState;
static int keybagResult = 1;
static void *keybagWorker(void *) {
    // These are the original read-only checks used by the 22H374 migrator.
    // Preserve raw results, including errors; never synthesize an unlocked bag.
    puts("KEYBAG_STATE_BEGIN read_only=1");
    void *library = dlopen("/System/Library/PrivateFrameworks/MobileKeyBag.framework/MobileKeyBag", RTLD_LAZY);
    auto lockState = reinterpret_cast<int (*)(int)>(library ? dlsym(library, "MKBGetDeviceLockState") : nullptr);
    auto unlockedSinceBoot = reinterpret_cast<int (*)()>(library ? dlsym(library, "MKBDeviceUnlockedSinceBoot") : nullptr);
    if (lockState && unlockedSinceBoot) {
        printf("KEYBAG_LOCK_STATE raw=%d\n", lockState(0));
        printf("KEYBAG_UNLOCKED_SINCE_BOOT raw=%d\n", unlockedSinceBoot());
        keybagResult = 0; // Query completed; this does not assert unlocked state.
    } else puts("KEYBAG_STATE native APIs unavailable");
    auto lockInfo = reinterpret_cast<CFTypeRef (*)(CFDictionaryRef)>(library ? dlsym(library, "MKBGetDeviceLockStateInfo") : nullptr);
    if (lockInfo) {
        CFTypeRef info = lockInfo(nullptr);
        printf("KEYBAG_LOCK_INFO %s\n", info ? [(__bridge id)info description].UTF8String : "(none)");
        if (info) CFRelease(info);
    }
    dlopen("/System/Library/PrivateFrameworks/ManagedConfiguration.framework/ManagedConfiguration", RTLD_LAZY);
    Class connectionClass = NSClassFromString(@"MCProfileConnection");
    SEL shared = sel_registerName("sharedConnection"), passcode = sel_registerName("isPasscodeSet");
    if ([connectionClass respondsToSelector:shared]) {
        id connection = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(connectionClass, shared);
        Method method = class_getInstanceMethod(object_getClass(connection), passcode);
        if (method && (!strcmp(method_getTypeEncoding(method), "B16@0:8") || !strcmp(method_getTypeEncoding(method), "c16@0:8")))
            printf("KEYBAG_MC_PASSCODE_SET raw=%u\n", reinterpret_cast<BOOL (*)(id, SEL)>(objc_msgSend)(connection, passcode));
    }
    void *springboard = dlopen("/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices", RTLD_LAZY);
    auto server = reinterpret_cast<mach_port_t (*)()>(springboard ? dlsym(springboard, "SBSSpringBoardServerPort") : nullptr);
    auto screenLock = reinterpret_cast<int (*)(mach_port_t, uint8_t *, uint8_t *)>(springboard ? dlsym(springboard, "SBGetScreenLockStatus") : nullptr);
    // Native MIG reply uses two one-byte booleans; retain its result code so
    // an unavailable service cannot be mistaken for an unlocked screen.
    uint8_t locked = 0xff, protectedScreen = 0xff;
    if (server && screenLock) {
        int status = screenLock(server(), &locked, &protectedScreen);
        printf("KEYBAG_SCREEN_LOCK result=0x%x valid=%u locked=%u passcode=%u\n", status, !status, locked, protectedScreen);
    }
    dlopen("/System/Library/PrivateFrameworks/SpringBoardFoundation.framework/SpringBoardFoundation", RTLD_LAZY);
    Class bagClass = NSClassFromString(@"SBFMobileKeyBag");
    SEL instance = sel_registerName("sharedInstance");
    if ([bagClass respondsToSelector:instance]) {
        id bag = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(bagClass, instance);
        describeNativeObject("keybag", bag, @[@"state", @"extendedState", @"hasPasscodeSet", @"hasBeenUnlockedSinceBoot"]);
        if ([bag respondsToSelector:sel_registerName("state")]) {
            id state = [bag valueForKey:@"state"];
            describeNativeObject("keybag-state", state, @[@"lockState", @"isEffectivelyLocked", @"permanentlyBlocked", @"failedAttemptCount"]);
        }
    }
    struct statfs mount = {};
    int mountResult = statfs("/private/var", &mount);
    int mountError = mountResult ? errno : 0;
    printf("KEYBAG_DATA_MOUNT result=%d errno=%d flags=0x%x content_protection=%d\n",
        mountResult, mountError, mount.f_flags, !mountResult && !!(mount.f_flags & MNT_CPROTECT));
    void *gestalt = dlopen("/usr/lib/libMobileGestalt.dylib", RTLD_LAZY);
    auto answer = reinterpret_cast<CFTypeRef (*)(CFStringRef)>(gestalt ? dlsym(gestalt, "MGCopyAnswer") : nullptr);
    CFTypeRef hasSEP = answer ? answer(CFSTR("HasSEP")) : nullptr;
    printf("KEYBAG_HARDWARE has_sep_answer=%s\n", hasSEP ? [(__bridge id)hasSEP description].UTF8String : "unavailable");
    if (hasSEP) CFRelease(hasSEP);
    void *keystore = dlopen("/System/Library/PrivateFrameworks/AppleKeyStore.framework/AppleKeyStore", RTLD_LAZY);
    auto getSystem = reinterpret_cast<int (*)(int, int *)>(keystore ? dlsym(keystore, "aks_get_system") : nullptr);
    auto getLock = reinterpret_cast<int (*)(int, unsigned int *)>(keystore ? dlsym(keystore, "aks_get_lock_state") : nullptr);
    if (getSystem && getLock) {
        int handle = -1;
        int result = getSystem(0, &handle);
        printf("KEYBAG_AKS_SYSTEM result=0x%08x handle_valid=%d\n", (unsigned)result, !result);
        const int bags[] = {0, -3};
        for (int bag : bags) {
            unsigned int state = 0;
            result = getLock(bag, &state);
            printf("KEYBAG_AKS_LOCK bag=%d result=0x%08x state_valid=%d state=0x%x\n",
                bag, (unsigned)result, !result, state);
        }
    } else puts("KEYBAG_AKS native APIs unavailable");
    @autoreleasepool {
        Protocol *protocol = objc_getProtocol("KBXPCProtocol");
        SEL query = sel_registerName("getDeviceLockState:needsExtended:withReply:");
        auto method = protocol ? protocol_getMethodDescription(protocol, query, YES, YES) : objc_method_description{};
        printf("KEYBAG_XPC_PROTOCOL native=%d types=%s\n", !!protocol, method.types ?: "unavailable");
        if (protocol && (!method.types || strcmp(method.types, "v32@0:8i16B20@?24"))) {
            __atomic_store_n(&keybagState, 2, __ATOMIC_RELEASE);
            return nullptr;
        }
        auto initialize = reinterpret_cast<id (*)(id, SEL, NSString *, NSUInteger)>(objc_msgSend);
        const NSUInteger lookupOptions[] = {0, 1UL << 12};
        for (NSUInteger options : lookupOptions) {
            NSXPCConnection *connection = initialize([NSXPCConnection alloc],
                sel_registerName("initWithMachServiceName:options:"), @"com.apple.mobile.keybagd.xpc", options);
            connection.remoteObjectInterface = [NSXPCInterface interfaceWithProtocol:protocol ?: @protocol(KeybagStateDiagnostics)];
            dispatch_semaphore_t reply = dispatch_semaphore_create(0);
            [connection resume];
            id<KeybagStateDiagnostics> remote = [connection remoteObjectProxyWithErrorHandler:^(NSError *error) {
                printf("KEYBAG_XPC_ERROR options=0x%lx domain=%s code=%ld description=%s\n",
                    (unsigned long)options, error.domain.UTF8String, (long)error.code, error.description.UTF8String);
                dispatch_semaphore_signal(reply);
            }];
            [remote getDeviceLockState:-1 needsExtended:NO withReply:^(unsigned int state, NSError *error) {
                printf("KEYBAG_XPC_REPLY options=0x%lx state_valid=%d state=0x%x error_domain=%s error_code=%ld\n",
                    (unsigned long)options, !error, state, error ? error.domain.UTF8String : "none", (long)error.code);
                dispatch_semaphore_signal(reply);
            }];
            long pending = dispatch_semaphore_wait(reply, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC));
            printf("KEYBAG_XPC_QUERY options=0x%lx pending=%d timeout_ms=1000\n", (unsigned long)options, !!pending);
            [connection invalidate];
        }
    }
    __atomic_store_n(&keybagState, 2, __ATOMIC_RELEASE);
    return nullptr;
}
static int boundedProbe(void *(*worker)(void *), int *state, int *result, const char *name) {
    auto create = reinterpret_cast<decltype(&pthread_create)>(dlsym(RTLD_DEFAULT, "pthread_create"));
    auto detach = reinterpret_cast<decltype(&pthread_detach)>(dlsym(RTLD_DEFAULT, "pthread_detach"));
    auto sleep = reinterpret_cast<decltype(&usleep)>(dlsym(RTLD_DEFAULT, "usleep"));
    if (!create || !detach || !sleep) return 1;
    int expected = 0;
    if (__atomic_compare_exchange_n(state, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        pthread_t thread;
        int error = create(&thread, nullptr, worker, nullptr);
        if (error) { __atomic_store_n(state, 0, __ATOMIC_RELEASE); printf("%s_THREAD_ERROR %d\n", name, error); return 1; }
        detach(thread);
    }
    // Keep the development console usable when a native framework waits. The
    // worker owns only static storage and remains valid after this call returns.
    for (unsigned i = 0; i < 30 && __atomic_load_n(state, __ATOMIC_ACQUIRE) != 2; ++i) sleep(100000);
    if (__atomic_load_n(state, __ATOMIC_ACQUIRE) == 2) return *result;
    printf("%s_PENDING\n", name);
    stacks(true);
    return 1;
}
// The diagnostic personality opts in at boot. Reports enter the same native
// IOHIDDevice as the physical buttons; GPIO and secure sampling are bypassed.
class SetupInput {
    mach_port_t home=0,power=0;
    static mach_port_t find(const char *name) {
        mach_port_t iterator=0;
        if (IOServiceGetMatchingServices(0,IOServiceMatching(name),&iterator)) return 0;
        mach_port_t service=IOIteratorNext(iterator),extra=IOIteratorNext(iterator);
        IOObjectRelease(iterator);
        if (extra) {IOObjectRelease(extra);if(service)IOObjectRelease(service);return 0;}
        return service;
    }
    static uint64_t counter(mach_port_t service,const char *name) {
        NSString *key=[NSString stringWithUTF8String:name];
        CFTypeRef value=IORegistryEntryCreateCFProperty(service,(__bridge CFStringRef)key,nullptr,0);
        uint64_t count=value && CFGetTypeID(value)==CFNumberGetTypeID() ? [(__bridge NSNumber *)value unsignedLongLongValue] : 0;
        if(value)CFRelease(value);return count;
    }
public:
    ~SetupInput(){if(home)IOObjectRelease(home);if(power)IOObjectRelease(power);}
    bool open() {
        if(geteuid())return false;
        home=find("FingerprintHID");power=find("QcomButtons");
        printf("SETUP_INPUT_TRANSPORT mode=driver created=%u physical_input=0\n",home && power);return home && power;
    }
    bool pulse(bool isHome,unsigned cycle,const char *phase) {
        mach_port_t service=isHome?home:power;
        const char *key=isHome?"HomeInputReports":"ButtonInputReports";
        uint64_t before=counter(service,key),oldPulses=counter(service,"HIDDiagnosticPulses");
        printf("SETUP_ACTION cycle=%u phase=%s event=begin absolute=%llu continuous=%llu mode=driver usage_page=12 usage=%u physical_input=0\n",
            cycle,phase,mach_absolute_time(),mach_continuous_time(),isHome?0x40:0x30);
        IOReturn status=IORegistryEntrySetCFProperties(service,(__bridge CFTypeRef)@{@"HIDDiagnosticPulse":@YES});
        bool released=false;
        for(unsigned attempt=0;!status && attempt<100;++attempt) {
            usleep(10000);
            CFTypeRef value=IORegistryEntryCreateCFProperty(service,CFSTR("HIDDiagnosticLastReleased"),nullptr,0);
            released=value==kCFBooleanTrue;
            if(value)CFRelease(value);
            if(released)break;
        }
        uint64_t after=counter(service,key),pulses=counter(service,"HIDDiagnosticPulses");
        bool delivered=!status && released && after>=before+2 && pulses==oldPulses+1;
        printf("SETUP_ACTION cycle=%u phase=%s event=released absolute=%llu down_status=0x%x reports_before=%llu reports_after=%llu pulse_count=%llu automatic_release=%u delivery_verified=%u\n",
            cycle,phase,mach_absolute_time(),status,before,after,pulses,released,delivered);
        return delivered;
    }
};
static void setupInputState(unsigned cycle,const char *phase) {
    void *library=dlopen("/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices",RTLD_LAZY);
    auto locked=reinterpret_cast<Boolean (*)(Boolean *)>(library?dlsym(library,"SBSGetScreenLockStatus"):nullptr);
    Dl_info image{};const uint8_t uuid[]={0xa6,0xaa,0x78,0xc3,0x67,0xeb,0x37,0x6b,0xa3,0x8a,0x44,0x99,0x5f,0x7e,0x38,0xa0};
    bool pinned=locked && dladdr(reinterpret_cast<void *>(locked),&image) && pinnedImage(image.dli_fbase,uuid) &&
        reinterpret_cast<uintptr_t>(locked)-reinterpret_cast<uintptr_t>(image.dli_fbase)==0x3d00;
    // Audited wrapper returns byte15 (UI lock); its single optional Boolean*
    // receives byte14 (passcode), after the original native MIG request.
    Boolean passcode=false;const bool state=pinned?locked(&passcode):false;
    printf("SETUP_INPUT_STATE cycle=%u phase=%s absolute=%llu lock_api_valid=%u ui_locked=%u passcode=%u\n",
        cycle,phase,mach_absolute_time(),pinned,state,passcode!=0);
    if(activeDisplayControl) {
        auto read=reinterpret_cast<NSInteger (*)(id,SEL)>(objc_msgSend);
        printf("SETUP_DISPLAY_STATE cycle=%u phase=%s absolute=%llu display=%ld power=%ld target_display=%ld target_power=%ld\n",
            cycle,phase,mach_absolute_time(),long(read(activeDisplayControl,sel_registerName("displayState"))),
            long(read(activeDisplayControl,sel_registerName("powerState"))),long(read(activeDisplayControl,sel_registerName("targetDisplayState"))),
            long(read(activeDisplayControl,sel_registerName("targetPowerState"))));
    }
    if(library)dlclose(library);
}
static bool setupDisplayMatches(bool on) {
    if(!activeDisplayControl)return false;
    auto read=reinterpret_cast<NSInteger (*)(id,SEL)>(objc_msgSend);
    for(const char *name:{"displayState","powerState","targetDisplayState","targetPowerState"})
        if(read(activeDisplayControl,sel_registerName(name))!=(on?1:0))return false;
    return true;
}
static int setupCycles(unsigned count,const char *prefix) {
    if(!count || count>6 || geteuid() || !prefix || strncmp(prefix,"setup-",6) || strlen(prefix)>80)return 2;
    for(const char *at=prefix;*at;++at)if(!((*at>='a' && *at<='z') || (*at>='A' && *at<='Z') || (*at>='0' && *at<='9') || *at=='-'))return 2;
    SetupInput input;if(!input.open())return 1;
    printf("SETUP_CYCLES_BEGIN count=%u absolute=%llu physical_input=0 optical_verified=0\n",count,mach_absolute_time());
    if (displayState(false,true)) return 1;
    auto stateValue=reinterpret_cast<NSInteger (*)(id,SEL)>(objc_msgSend);
    if (stateValue(activeDisplayControl,sel_registerName("displayState"))!=1 && !input.pulse(false,0,"initial-wake")) return 1;
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:3]];
    for(unsigned cycle=0;cycle<count;++cycle) @autoreleasepool {
        if(!input.pulse(false,cycle,"lock"))return 1;
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:2]];
        // A deferred idle-power transition can race the initial observation
        // and invert the first toggle. Converge once before recording wake;
        // never label an off screen Hello just because HID delivery succeeded.
        if(!setupDisplayMatches(false)) {
            if(!input.pulse(false,cycle,"lock-repair"))return 1;
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:2]];
        }
        setupInputState(cycle,"locked");
        if(!setupDisplayMatches(false)){printf("SETUP_CYCLE_INVALID cycle=%u phase=locked\n",cycle);return 1;}
        if(!input.pulse(false,cycle,"wake"))return 1;
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:6]];
        setupInputState(cycle,"hello");char image[160];
        if(!setupDisplayMatches(true)){printf("SETUP_CYCLE_INVALID cycle=%u phase=hello\n",cycle);return 1;}
        snprintf(image,sizeof(image),"/private/var/tmp/%s-%u-hello.png",prefix,cycle);
        const int hello=captureLogicalDisplay(image);
        if(!input.pulse(true,cycle,"home"))return 1;
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:8]];
        setupInputState(cycle,"next");
        if(!setupDisplayMatches(true)){printf("SETUP_CYCLE_INVALID cycle=%u phase=next\n",cycle);return 1;}
        snprintf(image,sizeof(image),"/private/var/tmp/%s-%u-next.png",prefix,cycle);
        const int next=captureLogicalDisplay(image);
        printf("SETUP_CYCLE_END cycle=%u absolute=%llu hello_image_status=%d next_image_status=%d\n",cycle,mach_absolute_time(),hello,next);
    }
    printf("SETUP_CYCLES_END count=%u absolute=%llu\n",count,mach_absolute_time());return 0;
}
static const char displayUsage[] = "displayctl timeline-binary | timeline | sample-display-state SECONDS | hold-display-on SECONDS | lease-history | frame-stats [raw] | wake-ui | display-on | setup-cycles COUNT driver [setup-NAME] | setup-button home|power | logical-image /private/var/tmp/setup-NAME.png | diagnostic-file /private/var/tmp/setup-NAME.txt | diagnostic-image /private/var/tmp/setup-NAME.png | scanout|validate-scanout|validate-window|validate-retirement /private/var/tmp/PREFIX | stacks-pid PID | scanout-live | scanout-samples COUNT INTERVAL_MS | native-exception-window | registry | framebuffer | swap-test | swap-gpu | displays | displays-system | display-link | progress-ui | layer-test | layer-animation | identity | display-identities | display-apis | render-info | display-state | prepare-executable | bootstrap | screen-info | device-identity | invalidate-device-identity-cache | keybag-state | invalidate-gestalt-cache | tasks | stacks | stacks-all | jobs | containers | initialize-persona | reboot\n";

int main(int argc, char **argv) {
    setbuf(stdout, nullptr);
    @autoreleasepool {
        if(argc==3 && !strcmp(argv[1],"diagnostic-file"))return diagnosticFile(argv[2]);
        if(argc==3 && !strcmp(argv[1],"diagnostic-image"))return diagnosticFile(argv[2],true);
        if(argc==3 && !strcmp(argv[1],"diagnostic-thumbnail"))return diagnosticFile(argv[2],true,true);
        if(argc==3 && !strcmp(argv[1],"logical-image"))return captureLogicalDisplay(argv[2]);
        if(argc==3 && !strcmp(argv[1],"setup-button") && (!strcmp(argv[2],"home") || !strcmp(argv[2],"power"))) {
            SetupInput input;return input.open() && input.pulse(!strcmp(argv[2],"home"),0,argv[2]) ? 0 : 1;
        }
        if((argc==4 || argc==5) && !strcmp(argv[1],"setup-cycles") && !strcmp(argv[3],"driver")) {
            char *end=nullptr;errno=0;unsigned long count=strtoul(argv[2],&end,10);
            if(errno || !argv[2][0] || !end || *end || !count || count>6)return 2;
            return setupCycles(unsigned(count),argc==5?argv[4]:"setup-auto-driver");
        }
        if(argc==3 && !strcmp(argv[1],"surface-compression-policy"))return surfaceCompressionPolicy(argv[2]);
        if(argc==3 && !strcmp(argv[1],"metal-drawable-check") && !strcmp(argv[2],"linear"))return metalDrawableCheck(true);
        if(argc==4 && !strcmp(argv[1],"scanout-samples")) {
            char *end=nullptr;errno=0;unsigned long count=strtoul(argv[2],&end,10);
            if(errno || !argv[2][0] || !end || *end || !count || count>64)return 2;
            errno=0;unsigned long interval=strtoul(argv[3],&end,10);
            if(errno || !argv[3][0] || !end || *end || interval<100 || interval>5000 || count*interval>60000)return 2;
            return sampleScanout(unsigned(count),unsigned(interval));
        }
        if (argc == 3 && !strcmp(argv[1], "scanout")) return captureScanout(argv[2]);
        if (argc == 3 && !strcmp(argv[1], "validate-scanout")) return validateScanout(argv[2]);
        if (argc == 3 && !strcmp(argv[1], "validate-window")) return validateWindow(argv[2]);
        if (argc == 3 && !strcmp(argv[1], "validate-retirement")) return validateScanout(argv[2], true);
        if (argc == 3 && !strcmp(argv[1], "stacks-pid")) {
            char *end = nullptr;
            errno = 0;
            long pid = strtol(argv[2], &end, 10);
            if (errno || !argv[2][0] || !end || *end || pid <= 0 || pid > INT32_MAX) return 2;
            return stacks(false, false, int(pid));
        }
        if(argc==3 && (!strcmp(argv[1],"hold-display-on") || !strcmp(argv[1],"sample-display-state"))) {
            const bool requestOn=!strcmp(argv[1],"hold-display-on");
            char *end=nullptr;errno=0;unsigned long seconds=strtoul(argv[2],&end,10);
            if(errno || !argv[2][0] || !end || *end || !seconds || seconds>(requestOn?60u:180u))return 2;
            return sampleDisplayPower(unsigned(seconds),requestOn);
        }
        if(argc==3 && !strcmp(argv[1],"frame-stats") && !strcmp(argv[2],"raw"))return frameStatistics(true);
        if (argc != 2) { fputs(displayUsage, stderr); return 2; }
        if(!strcmp(argv[1],"metal-drawable-check"))return metalDrawableCheck(false);
        if (!strcmp(argv[1], "scanout-live")) return captureScanout("/private/var/tmp/display-live", true);
        if (!strcmp(argv[1], "native-exception-window")) return nativeExceptionWindow();
        if (!strcmp(argv[1], "registry")) { registry(); return 0; }
        if (!strcmp(argv[1], "lease-history")) return leaseHistory();
        if (!strcmp(argv[1], "frame-stats")) return frameStatistics(false);
        if (!strcmp(argv[1], "timeline")) return displayTimeline();
        if (!strcmp(argv[1], "timeline-binary")) return displayTimeline(true);
        if (!strcmp(argv[1], "displays")) return displays();
        if (!strcmp(argv[1], "displays-system")) return displays(true);
        if (!strcmp(argv[1], "display-link")) return displayLinkClock();
        if (!strcmp(argv[1], "progress-ui")) return progressUI();
        if (!strcmp(argv[1], "layer-test")) return layerTest();
        if (!strcmp(argv[1], "layer-animation")) return layerTest(true);
        if (!strcmp(argv[1], "identity")) return displayIdentity();
        if (!strcmp(argv[1], "display-identities")) return displayIdentities();
        if (!strcmp(argv[1], "display-apis")) return displayAPIs();
        if (!strcmp(argv[1], "render-info")) return boundedProbe(renderInfoWorker,&renderInfoState,&renderInfoResult,"RENDER_INFO");
        if (!strcmp(argv[1], "display-state")) return displayState();
        if (!strcmp(argv[1], "display-on")) return displayState(true);
        if (!strcmp(argv[1], "wake-ui")) return boundedProbe(uiWakeWorker,&uiWakeState,&uiWakeResult,"NATIVE_UI_WAKE");
        if (!strcmp(argv[1], "prepare-executable")) return prepareExecutable();
        if (!strcmp(argv[1], "bootstrap")) return boundedProbe(displayBootstrapWorker, &displayBootstrapState, &displayBootstrapResult, "CA_BOOTSTRAP");
        if (!strcmp(argv[1], "screen-info")) return boundedProbe(screenInfoWorker, &screenInfoState, &screenInfoResult, "SCREEN_INFO");
        if (!strcmp(argv[1], "device-identity")) return deviceIdentity();
        if (!strcmp(argv[1], "invalidate-device-identity-cache")) return deviceIdentity(true);
        if (!strcmp(argv[1], "keybag-state")) return boundedProbe(keybagWorker, &keybagState, &keybagResult, "KEYBAG_STATE");
        if (!strcmp(argv[1], "invalidate-gestalt-cache")) return invalidateGestaltCache();
        if (!strcmp(argv[1], "framebuffer")) return boundedProbe(framebufferWorker, &framebufferState, &framebufferResult, "IOMFB");
        if (!strcmp(argv[1], "swap-test")) return swapTest();
        if (!strcmp(argv[1], "swap-gpu")) return swapTest(nullptr, false, true);
        if (!strcmp(argv[1], "containers")) return boundedProbe(containerQueryWorker, &containerQueryState, &containerQueryResult, "CONTAINER_QUERY");
        if (!strcmp(argv[1], "tasks") || !strcmp(argv[1], "processes")) return stacks();
        if (!strcmp(argv[1], "stacks")) return stacks();
        if (!strcmp(argv[1], "stacks-all")) return stacks(false, true);
        if (!strcmp(argv[1], "jobs")) return jobs();
        if (!strcmp(argv[1], "initialize-persona")) return initializePersona();
        if (!strcmp(argv[1], "reboot")) return rebootDevice();
        return 2;
    }
}

// Bash's documented loadable-builtin ABI lets the development console run the
// same diagnostics before a standalone executable has been installed.
struct DisplayWord { char *word; int flags; };
struct DisplayWordList { DisplayWordList *next; DisplayWord *word; };
static int displayBuiltin(DisplayWordList *words) {
    char name[] = "displayctl";
    char *args[3] = {name, nullptr, nullptr};
    if (!words || words->next || !words->word) return 2;
    args[1] = words->word->word;
    return main(2, args);
}
static const char *displayDocumentation[] = {"Read native display, process and launchd state.", nullptr};
extern "C" {
__attribute__((visibility("default"))) struct {
    const char *name;
    int (*function)(DisplayWordList *);
    int flags;
    const char **documentation;
    const char *usage;
    void *handle;
} displayctl_struct = {"displayctl", displayBuiltin, 1, displayDocumentation,
    displayUsage, nullptr};
}

