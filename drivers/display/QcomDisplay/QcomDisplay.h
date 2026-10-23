/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef _QCOM_DISPLAY_H
#define _QCOM_DISPLAY_H
#include <IOKit/graphics/IOMobileFramebuffer.h>
#include <IOKit/graphics/IOSurface.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IODeviceMemory.h>
#include "DisplayProtocol.h"
#include <DisplayBacklight.h>
#include <DisplayLeaseHistory.h>
#include <DisplayTimeline.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include "QcomDisplaySurface.h"
#include "DisplayPrepareSlot.h"
#include "DisplayInterruptDispatch.h"
#include "DisplayIdleMappings.h"
class QcomDisplayMapper;

class QcomDisplay : public IOMobileFramebufferLegacy {
    OSDeclareDefaultStructors(QcomDisplay);
public:
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    IOReturn setProperties(OSObject *) APPLE_KEXT_OVERRIDE;
    IOReturn setPowerState(unsigned long, IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    bool requestTerminate(IOService *, IOOptionBits) APPLE_KEXT_OVERRIDE;
    void displayMapperFault();
protected:
    bool start_hardware() APPLE_KEXT_OVERRIDE;
    void stop_hardware_gated() APPLE_KEXT_OVERRIDE;
    void swap_begin_gated() APPLE_KEXT_OVERRIDE;
    IOReturn do_power_state_change_gated() APPLE_KEXT_OVERRIDE;
    void create_default_fb_surface() APPLE_KEXT_OVERRIDE;
    UInt32 default_fb_surface_gated(UInt32, UInt32) APPLE_KEXT_OVERRIDE;
    IOReturn deallocate_default_fb_surface_gated() APPLE_KEXT_OVERRIDE;
    bool is_main_display() APPLE_KEXT_OVERRIDE;
    IOReturn set_display_device(UInt32) APPLE_KEXT_OVERRIDE;
    bool get_timing_attributes_gated(UInt64 *, bool) const APPLE_KEXT_OVERRIDE;
    bool verify_swap(void *) APPLE_KEXT_OVERRIDE;
    bool verify_swap_gated(void *) APPLE_KEXT_OVERRIDE;
    void prepare_swap(void *) APPLE_KEXT_OVERRIDE;
    IOReturn set_notify_tag_mask_gated(UInt32, void *, UInt32) APPLE_KEXT_OVERRIDE;
    IOReturn supported_frame_info(UInt32, UInt32 *, char (*)[64], UInt32 *) APPLE_KEXT_OVERRIDE;
    IOReturn factory_portal(void *, task_t) APPLE_KEXT_OVERRIDE;
    IOReturn executeFactoryConfigurations_gated() APPLE_KEXT_OVERRIDE;
    IOReturn set_frame_info_notification_state_gated(void *, bool) APPLE_KEXT_OVERRIDE;
    void do_swap_info_notify_gated(void *, void *) APPLE_KEXT_OVERRIDE;
    void notify_swap_done_gated(UInt32) APPLE_KEXT_OVERRIDE;
    void notify_crc_ready_gated(UInt32) APPLE_KEXT_OVERRIDE;
    bool timing_enabled() const APPLE_KEXT_OVERRIDE;
private:
    struct Registers {
        volatile uint8_t *base;
        uint32_t read(uint32_t) const;
        void write(uint32_t, uint32_t);
    } regs;
    Display::Mode mode;
    Display::PlatformContract contract;
    IOMemoryMap *mdpMap, *scanoutMap;
    IODeviceMemory *scanoutMemory;
    IOSurface *defaultSurface;
    IOService *panel;
    IOService *backlight;
    uint32_t backlightIdentifier;
    bool backlightClaimed, displayVisible;
    IOInterruptEventSource *interrupt;
    IOTimerEventSource *timeout;
    IOCommandGate *gate;
    IOWorkLoop *frameworkLoop;
    uint32_t savedInterruptEnable;
    uint32_t pixelsPerInch;
    UInt32 swapValidationLogs, swapAcceptedLogs, surfaceValidationLogs, swapBeginLogs, powerStateLogs;
    UInt32 preparePhysicalLogs, prepareOtherLogs;
    UInt32 copyLogs, completionLogs, transferCompletionLogs;
    UInt32 scanoutCaptureGeneration;
    Display::SwapSample swapSamples[Display::SwapSampleCapacity]{};
    uint32_t swapSampleHead=0,swapSampleCount=0;
    uint64_t displayedSwapCount=0,failedSwapCount=0;
    static IOReturn swapStatisticsAction(OSObject *,void *,void *,void *,void *);
    Display::LeaseSample leaseSamples[Display::LeaseHistoryCapacity]{};
    uint64_t leaseSampleTotal=0;
    uint32_t leaseSampleHead=0,leaseSampleCount=0;
    uint64_t leaseWaitRequest=0,leaseWaitStarted=0,leaseFrozenAt=0;
    uint32_t leaseWaitToken=0;
    void recordLease(Display::LeasePhase,void *request=nullptr);
    static IOReturn leaseHistoryAction(OSObject *,void *,void *,void *,void *);
    bool scanoutCaptureEnabled, nativeTraceEnabled;
    bool nativeStarted, resourcesReady, opened, panelOpened;
    bool pmStarted;
    bool interruptAdded, timerAdded, gateAdded, irqEnabled;
    bool swapPending, swapDisplayed, pipelineFaulted, doubleBuffered;
    bool commandAutoRefresh;
    bool commandSingleTransfer = false;
    uint64_t commandReadRetired = 0;
    uint64_t frames, lastFrameTime;
    uint64_t scanoutDeviceAddress;
    uint64_t frontOffset, stagedOffset;
    IOMapper *displayMapper = nullptr;
    QcomDisplayMapper *ownedMapper = nullptr;
    QcomDisplaySurface *frontSurface = nullptr, *stagedSurface = nullptr;
    QcomDisplaySurface *quarantinedSurface = nullptr;
    Display::PreparedSwapSlot<QcomDisplaySurface,IOFence> preparedSwap;
    Display::PrepareStatistics preparation;
    Display::IdleMappings<QcomDisplaySurface> idleMappings;
    uint64_t mappingCacheHits=0,mappingCacheMisses=0,mappingCacheEvictions=0;
    Display::TimelineRing *timeline=nullptr;
    void recordTimeline(Display::TimelineKind,uint32_t status=0,void *request=nullptr,uint64_t auxiliary=0);
    static bool interruptFilter(OSObject *,IOFilterInterruptEventSource *);
    static IOReturn timelineAction(OSObject *,void *,void *,void *,void *);
    uint64_t preparedAt=0;
    bool preparingSwap=false;
    uint64_t stagedAddress = 0, directFrames = 0;
    uint32_t stagedRowBytes = 0;
    bool directRequested = false, directReady = false, stagedExposed = false, directQuarantined = false;
    bool configure(IOService *);
    bool publishDisplayProperties();
    bool configureBacklight(IOService *);
    IOReturn backlightRequest(DisplayBacklight::Operation, uint32_t = 0);
    void setDisplayVisible(bool);
    bool retainedRasterValid() const;
    static IOReturn brightnessAction(OSObject *, void *, void *, void *, void *);
    bool validateSurface(IOSurface *) const;
    IOReturn validateSwap(void *, bool);
    IOReturn verify_swap_surfaces(void *, bool);
    IOReturn traceSwapVerification(void *, IOReturn, bool);
    bool copySurface(IOSurface *, uint64_t offset);
    QcomDisplaySurface *prepareScanoutSurface(IOSurface *,bool ahead=false);
    bool preparationKey(void *,Display::PreparedSwapKey &,IOFence *&) const;
    bool discardPreparedSwap();
    bool retirePreparedMapping(QcomDisplaySurface *);
    void preparePendingSwap();
    void finishScanout(bool displayed);
    bool clearScanout(uint64_t offset);
    IOReturn captureScanoutGated();
    static IOReturn captureScanoutAction(OSObject *, void *, void *, void *, void *);
    void releaseResources();
    void stopPowerManagement();
    void stopFramework(IOService *);
    void onVBL(const Display::VBLObservation &);
    void completeSwap(bool);
    bool retireCommandRead(void *);
    static void interruptAction(OSObject *, IOInterruptEventSource *, int);
    static void timeoutAction(OSObject *, IOTimerEventSource *);
    template <class T> T &nativeState(size_t offset) {
        return *reinterpret_cast<T *>(reinterpret_cast<UInt8 *>(this) + offset);
    }
    enum : size_t {
        // Only iOS 18 offsets, independently recovered from the pinned image.
        NativePMInitialized = 0x8c,
        NativeVBLCount = 0x100, NativeVBLTime = 0x108, NativeVBLDelta = 0x110,
        RequestedControllerPower = 0x17e4, RequestedDisplayPower = 0x17e8,
        CurrentControllerPower = 0x17ec, CurrentDisplayPower = 0x17f0,
        LastCompletedSwap = 0x17d4, LastDisplayedSwap = 0x17d8,
        ActiveRequest = 0x1668, PendingRequest = 0x1670, PendingQueueLast = 0x1678,
        // Pinned native VBL notifier walks this list under the framework gate.
        NativeVBLSubscribers = 0x16e0,
        SurfaceRoot = 0x1718, DefaultSurface = 0x1730,
        NativeWidth = 0x17c8, NativeHeight = 0x17cc,
        NativePixelFormat = 0x1818, DisplayReady = 0x1898, MainDisplay = 0x189a,
        SurfaceArray = 0xa48, EffectiveSurfaces = 0xa68, RequestIntent = 0x32c,
        RequestBackground = 0x320, DefaultTransaction = 0x130, EmptyTransaction = 0x644,
        // These are changed/ready masks, not a count of visible layers.
        ChangedLayers = 0x318, ReadyLayers = 0x31c, RequestLinks = 0xc00,
        SourceRects = 0x11, DestinationRects = 0x51
    };
};
#endif
