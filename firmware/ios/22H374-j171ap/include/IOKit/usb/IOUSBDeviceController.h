// SPDX-License-Identifier: BSD-2-Clause
#pragma once
// iOS 18 arm64 ABI projection, image hash and all slot addresses are fixed by
// apple-usb-device-abi.json. Untyped native slots keep unconsumed Family
// operations and must not be called directly. Typed pure slots175/199 below
// are audited hardware hooks whose original C++ names remain unrecovered.
#include <IOKit/IOService.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/usb/IOUSBDeviceTypes.h>
#include <IOKit/usb/IOUSBDeviceControlRequest.h>
#include <IOKit/usb/IOUSBDeviceControllerState.h>

class IOUSBDeviceController : public IOService {
    OSDeclareAbstractStructors(IOUSBDeviceController);
protected:
    UInt8 nativeState[424];
    // Read-only view of the pinned Family's SET_SEL result and its qualified
    // U1/U2 policy. The Family owns these bytes; a hardware adapter must not
    // reconstruct the enable bits from an incoming SET_FEATURE request.
    // Absolute offsets avoid relying on reuse of IOService tail padding.
    using USB3LinkPolicy=IOUSBDeviceFamilyABI::USB3LinkPolicy;
    bool copyUSB3LinkPolicy(USB3LinkPolicy &policy) const {
        return IOUSBDeviceFamilyABI::decodeUSB3LinkPolicy(
            reinterpret_cast<const UInt8 *>(this)+IOUSBDeviceFamilyABI::USB3PolicyOffset,8,policy);
    }
    // Family prepareDefaults allocates this OSArray and its BOS builder and
    // connection-done handler consume it. Borrow only while the Family is
    // started and serialized; the adapter does not replace the array.
    OSArray *nativeDeviceCapabilities() const {
        return *reinterpret_cast<OSArray *const *>(reinterpret_cast<const UInt8 *>(this)+IOUSBDeviceFamilyABI::CapabilityListOffset);
    }
public:
    void free() APPLE_KEXT_OVERRIDE;
    IOReturn setProperties(OSObject *) APPLE_KEXT_OVERRIDE;
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    IOReturn callPlatformFunction(const OSSymbol *, bool, void *, void *, void *, void *) APPLE_KEXT_OVERRIDE;

    virtual IOUSBDevicePipe *createPipe(tCreatePipeParameters &); // 168
    virtual IOReturn activatePipe(IOUSBDevicePipe *); // 169
    virtual IOReturn deactivatePipe(IOUSBDevicePipe *); // 170
    virtual UInt16 getAddress() const; // 171
    virtual IOReturn createPipeGated(tCreatePipeParameters &, IOUSBDevicePipe *&); // 172
    virtual IOReturn activatePipeGated(IOUSBDevicePipe *); // 173
    virtual IOReturn deactivatePipeGated(IOUSBDevicePipe *); // 174
    // Typed panic hook; native name is still unrecovered. The native platform
    // panic call explicitly passes value3 in w1. No allocations/sleep here.
    virtual void nativeSlot175(UInt32) = 0; // 175
    virtual void nativeSlot176(); // 176
    virtual IOReturn goOnBus(bool) = 0; // 177
    virtual IOReturn handleUserlandCommand(OSString *, OSObject *); // 178
    virtual bool connectedWithAccessoryCable() = 0; // 179
    virtual void handleStallEndpoint(int, bool) = 0; // 180
    virtual void handleAbortEndpoint(int) = 0; // 181
    virtual void handleSetAddressPreStatusPhase(int) = 0; // 182
    virtual void handleSetAddressPostStatusPhase(int) = 0; // 183
    virtual UInt32 getConnectionSpeed() = 0; // 184
    virtual IOReturn provideEndpointIDsForConfiguration(OSArray *, UInt8 *) = 0; // 185
    virtual void prepareConfiguration(OSArray *) = 0; // 186
    virtual IOReturn activateEndpoint(int, int, int, int, tIOUSBDeviceInterfacePipeProperty) = 0; // 187
    virtual void deactivateEndpoint(int) = 0; // 188
    virtual IOBufferMemoryDescriptor *createIOBuffer(UInt32 options, UInt64 capacity) = 0; // 189
    virtual uintptr_t nativeSlot190(); // 190
    virtual uintptr_t nativeSlot191(); // 191
    virtual bool isEndpointStalled(int) = 0; // 192
    virtual void resetEndpointDataToggle(int) = 0; // 193
    virtual uintptr_t nativeSlot194(); // 194
    virtual void handleUSBSuspend(); // 195
    virtual void handleUSBReset(); // 196
    virtual void handleUSBConnectionDone(); // 197
    virtual void handleUSBResume(); // 198
    // Typed forced-disconnect hook used before configuration/personality
    // refresh. Its native C++ name is unrecovered; call/override by this slot.
    virtual void nativeSlot199() = 0; // 199
    virtual uintptr_t nativeSlot200(); // 200
    virtual uintptr_t nativeSlot201(); // 201
    virtual IOReturn doRemoteWakeGated() = 0; // 202
    virtual IOReturn nativeSlot203(UInt32); // 203: function remote wake, selected interface
    virtual void handleUSBCableConnect(); // 204
    virtual void handleUSBCableDisconnect(); // 205
    virtual uintptr_t nativeSlot206(); // 206
    virtual UInt32 getMaxSpeedCapability(); // 207
    // Minimum supported native speed: AppleSynopsysOTGDevice returns1 (FS),
    // used by pipe creation and configuration filtering before negotiation.
    virtual uintptr_t nativeSlot208(); // 208
    virtual UInt8 getDeviceAddress(); // 209
    virtual uintptr_t nativeSlot210(); // 210
    virtual uintptr_t nativeSlot211(); // 211
    virtual uintptr_t nativeSlot212(); // 212
    virtual uintptr_t nativeSlot213(); // 213
    virtual uintptr_t nativeSlot214(); // 214
    virtual uintptr_t nativeSlot215(); // 215
    virtual void handleControlRequestStateChange(IOUSBDeviceControlRequest::tControlRequestState,
        IOUSBDeviceControlRequest::tControlRequestState); // 216
    virtual uintptr_t nativeSlot217(); // 217
    virtual uintptr_t nativeSlot218(); // 218
    virtual uintptr_t nativeSlot219(); // 219
    virtual uintptr_t nativeSlot220(); // 220
    virtual uintptr_t nativeSlot221(); // 221
    virtual uintptr_t nativeSlot222(); // 222
    virtual uintptr_t nativeSlot223(); // 223
    virtual IOUSBDeviceControlRequest *createControlRequestTracker(); // 224
    virtual uintptr_t nativeSlot225(); // 225
    virtual IOReturn io(int, IOMemoryDescriptor *, IOByteCount, IOUSBDeviceCompletion *, UInt64 *, UInt32); // 226
    virtual IOReturn io(int, IOMemoryDescriptor *, UInt32, IOUSBDeviceIsocFrame *, IOUSBDeviceIsocCompletion *); // 227
    virtual IOReturn ioGated(_IOUSBDeviceIORequestTag *) = 0; // 228
    virtual _IOUSBDeviceIORequestTag *allocateRequest(); // 229
    virtual void freeRequest(_IOUSBDeviceIORequestTag *); // 230
    virtual IODMACommand *getDMACommand(); // 231
    virtual uintptr_t nativeSlot232(); // 232
    // SET/CLEAR_FEATURE, SET_SEL completion and reset apply Family policy
    // through this hook. X1 is the Native speed enum, W2 selects U1=1/U2=2.
    virtual IOReturn nativeSlot233(UInt32, UInt32); // 233
    virtual IOReturn nativeSlot234(UInt32 *); // 234

    // Native nonvirtual completion owns the Family callback and final
    // request/buffer release. Binding is verified against executable bytes.
    void completeEndpointIO(_IOUSBDeviceIORequestTag *);
    // Native creates USBStringIndex, the control tracker and default device
    // description before publishing to IOUSBDeviceConfigurator. Hardware
    // adapters must use this entry instead of registerService directly.
    void readyForHost();
};
static_assert(sizeof(IOUSBDeviceController) == IOUSBDeviceFamilyABI::ControllerBytes, "iOS 18 device controller instance ABI");
