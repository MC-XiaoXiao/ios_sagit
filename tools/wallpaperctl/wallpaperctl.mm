// SPDX-License-Identifier: BSD-2-Clause
// Read-only queries against the firmware's original WallpaperKit selection.
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#include <dlfcn.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

@interface NSObject (WallpaperDiagnostics)
+ (id)defaultManager;
+ (id)defaultWallpaperManager;
- (NSUInteger)numberOfWallpaperCollections;
- (id)wallpaperCollectionAtIndex:(NSUInteger)index;
- (NSUInteger)numberOfWallpapers;
- (id)wallpaperBundleAtIndex:(NSUInteger)index;
- (id)defaultWallpaperBundle;
- (id)deviceProductType;
- (id)deviceLogicalScreenClass;
@end

static id field(id object, NSString *key) {
    @try {
        id value = [object valueForKey:key];
        if (!value) return [NSNull null];
        if ([value isKindOfClass:NSString.class] || [value isKindOfClass:NSNumber.class]) return value;
        return [value description];
    } @catch (NSException *e) { return @{ @"exception": e.reason ?: e.name }; }
}
static NSDictionary *bundleInfo(id bundle) {
    NSMutableDictionary *result = [NSMutableDictionary dictionary];
    for (NSString *key in @[@"identifier", @"name", @"family", @"logicalScreenClass", @"bundleURL", @"adjustmentTraits"])
        result[key] = field(bundle, key);
    result[@"class"] = NSStringFromClass([bundle class]) ?: @"nil";
    return result;
}
int main(int argc, char **argv) {
    signal(SIGALRM, +[](int) { _exit(124); }); alarm(60);
    bool mobile=false,requireScreen=false;
    for(int i=1;i<argc;i++) {
        if(!strcmp(argv[i],"--mobile")) mobile=true;
        else if(!strcmp(argv[i],"--require-screen")) requireScreen=true;
        else return 64;
    }
    if (mobile && (setgid(501) || setuid(501))) return 9;
    @autoreleasepool {
        void *gestalt = dlopen("/usr/lib/libMobileGestalt.dylib", RTLD_LAZY);
        auto answer = reinterpret_cast<CFTypeRef (*)(CFStringRef)>(gestalt ? dlsym(gestalt,"MGCopyAnswer") : nullptr);
        id device = answer ? CFBridgingRelease(answer(CFSTR("GSDeviceName"))) : nil;
        if (![device isKindOfClass:NSString.class]) return 2;
        NSString *path = [NSString stringWithFormat:@"/Library/Wallpaper/Collections/Collections~%@.plist", [device lowercaseString]];
        if (![NSFileManager.defaultManager fileExistsAtPath:path]) path = @"/Library/Wallpaper/Collections/Collections.plist";
        // The native manager raises inside dispatch_once for a missing order;
        // diagnose it before entering that non-recoverable initialization path.
        NSDictionary *lookup = [NSDictionary dictionaryWithContentsOfFile:path];
        if (![lookup[@"order"] isKindOfClass:NSArray.class]) {
            fprintf(stderr,"WALLPAPER_MISSING_COLLECTION_ORDER device=%s path=%s\n",
                [device UTF8String],path.UTF8String); return 7;
        }
        // A command-line Foundation process has no UIKit display context yet.
        // Use native initialization, as WebKit's non-UIApplicationMain clients
        // do; never substitute a fabricated UIScreen or wallpaper screen class.
        auto initialize = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT,"UIApplicationInitialize"));
        if (!initialize) return 10;
        puts("WALLPAPER_UIKIT_INITIALIZE_BEGIN");fflush(stdout);
        initialize();
        puts("WALLPAPER_UIKIT_INITIALIZE_END");fflush(stdout);
        if (!dlopen("/System/Library/PrivateFrameworks/WallpaperKit.framework/WallpaperKit", RTLD_NOW)) return 2;
        @try {
            Class managers = NSClassFromString(@"WKWallpaperRepresentingCollectionsManager");
            Class defaults = NSClassFromString(@"WKDefaultWallpaperManager");
            if (![managers respondsToSelector:@selector(defaultManager)] ||
                ![defaults respondsToSelector:@selector(defaultWallpaperManager)]) return 3;
            id manager = [managers defaultManager], defaultManager = [defaults defaultWallpaperManager];
            NSMutableArray *collections = [NSMutableArray array];
            NSUInteger count = [manager numberOfWallpaperCollections];
            if (count > 64) return 4;
            for (NSUInteger i = 0; i < count; ++i) {
                id collection = [manager wallpaperCollectionAtIndex:i];
                NSMutableArray *bundles = [NSMutableArray array];
                NSUInteger wallpapers = [collection numberOfWallpapers];
                if (wallpapers > 256) return 4;
                for (NSUInteger j = 0; j < wallpapers; ++j)
                    [bundles addObject:bundleInfo([collection wallpaperBundleAtIndex:j])];
                [collections addObject:@{@"identifier":field(collection,@"wallpaperCollectionIdentifier"),
                    @"name":field(collection,@"displayName"), @"wallpapers":bundles}];
            }
            UIScreen *screen = UIScreen.mainScreen;
            NSDictionary *report = @{@"product":[defaultManager deviceProductType] ?: @"",
                @"logicalScreenClass":[defaultManager deviceLogicalScreenClass] ?: @"",
                @"screen":@{@"width":@(screen.bounds.size.width),@"height":@(screen.bounds.size.height),@"scale":@(screen.scale)},
                @"default":bundleInfo([defaultManager defaultWallpaperBundle]),@"collections":collections};
            NSData *json = [NSJSONSerialization dataWithJSONObject:report options:0 error:nil];
            if (!json) return 5;
            puts("WALLPAPER_NATIVE_JSON_BEGIN");
            fwrite(json.bytes, 1, json.length, stdout);
            puts("\nWALLPAPER_NATIVE_JSON_END");
            NSMutableDictionary *ids = [NSMutableDictionary dictionary];
            for (NSDictionary *collection in collections) {
                NSMutableArray *values=[NSMutableArray array];
                for (NSDictionary *bundle in collection[@"wallpapers"]) [values addObject:bundle[@"identifier"]];
                if ([collection[@"identifier"] isKindOfClass:NSString.class]) ids[collection[@"identifier"]]=values;
            }
            NSDictionary *summary=@{@"product":report[@"product"],@"logicalScreenClass":report[@"logicalScreenClass"],
                @"screen":report[@"screen"],@"default":report[@"default"][@"identifier"],@"collections":ids};
            NSData *compact=[NSJSONSerialization dataWithJSONObject:summary options:0 error:nil];
            NSMutableData *line=[NSMutableData dataWithData:[@"WALLPAPER_NATIVE_SUMMARY " dataUsingEncoding:NSUTF8StringEncoding]];
            [line appendData:compact];[line appendBytes:"\n" length:1];fflush(stdout);
            if (write(STDOUT_FILENO,line.bytes,line.length)!=(ssize_t)line.length) return 11;
            if (screen.bounds.size.width<=0 || screen.bounds.size.height<=0) {
                fputs("WALLPAPER_SCREEN_UNAVAILABLE\n",stderr);
                if(requireScreen) return 8;
            }
        } @catch (NSException *e) {
            fprintf(stderr,"WALLPAPER_EXCEPTION %s %s\n",e.name.UTF8String,e.reason.UTF8String); return 6;
        }
    }
    return 0;
}
