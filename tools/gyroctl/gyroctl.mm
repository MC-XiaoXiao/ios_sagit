// SPDX-License-Identifier: BSD-2-Clause
// Offline-built native CoreMotion probe. Run only when device testing is intended.
#import <Foundation/Foundation.h>
#import <CoreMotion/CoreMotion.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc != 1 && (argc != 2 || strcmp(argv[1], "--sample"))) {
            fprintf(stderr, "usage: gyroctl [--sample]\n");
            return 64;
        }
        CMMotionManager *motion = [[CMMotionManager alloc] init];
        printf("CoreMotion gyroAvailable=%s\n", motion.gyroAvailable ? "true" : "false");
        if (!motion.gyroAvailable) return 2;
        if (argc == 1) return 0;
        motion.gyroUpdateInterval = 0.01;
        [motion startGyroUpdates];
        double deadline = [NSProcessInfo processInfo].systemUptime + 5.0;
        NSTimeInterval previous = -1;
        unsigned samples = 0;
        while ([NSProcessInfo processInfo].systemUptime < deadline && samples < 100) {
            CMGyroData *data = motion.gyroData;
            if (data && data.timestamp != previous) {
                CMRotationRate rate = data.rotationRate;
                if (!isfinite(rate.x) || !isfinite(rate.y) || !isfinite(rate.z) ||
                    !isfinite(data.timestamp) || data.timestamp <= previous) {
                    fprintf(stderr, "invalid CoreMotion sample\n");
                    [motion stopGyroUpdates]; return 3;
                }
                previous = data.timestamp; ++samples;
                printf("%.9f %.9f %.9f %.9f rad/s\n", data.timestamp, rate.x, rate.y, rate.z);
            }
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.005]];
        }
        [motion stopGyroUpdates];
        fprintf(stderr, "CoreMotion samples=%u\n", samples);
        return samples == 100 ? 0 : 4;
    }
}
