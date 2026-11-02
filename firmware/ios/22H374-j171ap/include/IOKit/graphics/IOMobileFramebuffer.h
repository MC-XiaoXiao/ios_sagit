/* iOS 18 arm64 private subclass ABI. See docs/display/ios18-layout.json.
 * Unknown virtuals are relocation-only aliases, not callable prototypes.
 * All aliases are bound to native slots after image hash/layout verification. */
#ifndef _IO_MOBILE_FRAMEBUFFER_H
#define _IO_MOBILE_FRAMEBUFFER_H
#include <IOKit/IOService.h>
class IOMobileFramebuffer : public IOService {
    OSDeclareAbstractStructors(IOMobileFramebuffer);
    UInt8 nativeInterfaceState[8];
protected:
    virtual void nativeSlot168() = 0; // 168
    virtual void nativeSlot169() = 0; // 169
    virtual void nativeSlot170() = 0; // 170
    virtual void nativeSlot171() = 0; // 171
    virtual void nativeSlot172() = 0; // 172
    virtual void nativeSlot173() = 0; // 173
    virtual UInt32 default_fb_surface(UInt32, UInt32) = 0; // 174
    virtual UInt32 get_framebuffer_id() = 0; // 175
    virtual void nativeSlot176() = 0; // 176
    virtual void nativeSlot177() = 0; // 177
    virtual void nativeSlot178() = 0; // 178
    virtual void nativeSlot179() = 0; // 179
    virtual void nativeSlot180() = 0; // 180
    virtual void nativeSlot181() = 0; // 181
    virtual IOReturn set_display_device(UInt32) = 0; // 182
    virtual void nativeSlot183() = 0; // 183
    virtual bool is_main_display() = 0; // 184
    virtual void nativeSlot185() = 0; // 185
    virtual void nativeSlot186() = 0; // 186
    virtual void nativeSlot187() = 0; // 187
    virtual void nativeSlot188() = 0; // 188
    virtual void nativeSlot189() = 0; // 189
    virtual void nativeSlot190() = 0; // 190
    virtual void nativeSlot191() = 0; // 191
    virtual void nativeSlot192() = 0; // 192
    virtual void nativeSlot193() = 0; // 193
    virtual void nativeSlot194() = 0; // 194
    virtual void nativeSlot195() = 0; // 195
    virtual void nativeSlot196() = 0; // 196
    virtual void nativeSlot197() = 0; // 197
    virtual void nativeSlot198() = 0; // 198
    virtual void nativeSlot199() = 0; // 199
    virtual void nativeSlot200() = 0; // 200
    virtual void nativeSlot201() = 0; // 201
    virtual void nativeSlot202() = 0; // 202
    virtual void nativeSlot203() = 0; // 203
    virtual void nativeSlot204() = 0; // 204
    virtual void nativeSlot205() = 0; // 205
    virtual void nativeSlot206() = 0; // 206
    virtual void nativeSlot207() = 0; // 207
    virtual void nativeSlot208() = 0; // 208
    virtual void nativeSlot209() = 0; // 209
    virtual void nativeSlot210() = 0; // 210
    virtual void nativeSlot211() = 0; // 211
    virtual IOReturn set_notify_tag_mask_gated(UInt32, void *, UInt32) = 0; // 212
    virtual void nativeSlot213() = 0; // 213
    virtual IOReturn supported_frame_info(UInt32, UInt32 *, char (*)[64], UInt32 *) = 0; // 214
    virtual void nativeSlot215() = 0; // 215
    virtual void nativeSlot216() = 0; // 216
    virtual void nativeSlot217() = 0; // 217
    virtual void nativeSlot218() = 0; // 218
    virtual void nativeSlot219() = 0; // 219
    virtual void nativeSlot220() = 0; // 220
    virtual void nativeSlot221() = 0; // 221
    virtual void nativeSlot222() = 0; // 222
    virtual void nativeSlot223() = 0; // 223
    virtual void nativeSlot224() = 0; // 224
    virtual void nativeSlot225() = 0; // 225
    virtual void nativeSlot226() = 0; // 226
    virtual void nativeSlot227() = 0; // 227
    virtual void nativeSlot228() = 0; // 228
    virtual void nativeSlot229() = 0; // 229
    virtual void nativeSlot230() = 0; // 230
    virtual void nativeSlot231() = 0; // 231
    virtual void nativeSlot232() = 0; // 232
    virtual void nativeSlot233() = 0; // 233
    virtual void nativeSlot234() = 0; // 234
    virtual void nativeSlot235() = 0; // 235
    virtual void nativeSlot236() = 0; // 236
    virtual void nativeSlot237() = 0; // 237
    virtual void nativeSlot238() = 0; // 238
    virtual void nativeSlot239() = 0; // 239
    virtual void nativeSlot240() = 0; // 240
    virtual void nativeSlot241() = 0; // 241
    virtual void nativeSlot242() = 0; // 242
    virtual void nativeSlot243() = 0; // 243
    virtual void nativeSlot244() = 0; // 244
    virtual void nativeSlot245() = 0; // 245
    virtual void nativeSlot246() = 0; // 246
    virtual IOReturn factory_portal(void *, task_t) = 0; // 247
    virtual IOReturn executeFactoryConfigurations_gated() = 0; // 248
    virtual void nativeSlot249() = 0; // 249
    virtual void nativeSlot250() = 0; // 250
    virtual void nativeSlot251() = 0; // 251
    virtual void nativeSlot252() = 0; // 252
    virtual void nativeSlot253() = 0; // 253
    virtual void nativeSlot254() = 0; // 254
    virtual void nativeSlot255() = 0; // 255
    virtual void nativeSlot256() = 0; // 256
    virtual void nativeSlot257() = 0; // 257
    virtual void nativeSlot258() = 0; // 258
    virtual void nativeSlot259() = 0; // 259
    virtual void nativeSlot260() = 0; // 260
    virtual void nativeSlot261() = 0; // 261
    virtual void nativeSlot262() = 0; // 262
    virtual void nativeSlot263() = 0; // 263
    virtual void nativeSlot264() = 0; // 264
    virtual void nativeSlot265() = 0; // 265
};
class IOMobileFramebufferService : public IOMobileFramebuffer {
    OSDeclareAbstractStructors(IOMobileFramebufferService);
};
class IOMobileFramebufferLegacy : public IOMobileFramebufferService {
    OSDeclareAbstractStructors(IOMobileFramebufferLegacy);
    UInt8 nativeLegacyState[6592 - sizeof(IOMobileFramebufferService)];
public:
    void swap_complete_gated(bool displayed);
    void note_vbl_ISR(UInt32 fallbackIntervalNS);
    void nativeNotifyVBL(UInt64 count, UInt64 time, UInt64 delta);
    // Audited relocation aliases for the original iOS 18 queue helpers.
    // Names describe local usage; they are not claimed Apple export names.
    IOReturn nativePrunePendingSwaps();
    bool nativeDropRedundantPendingSwap();
    IOReturn nativeDropCompletedPendingSwaps();
    void nativeDropSupersededPendingSwaps();
    void nativeRecordSwapState(void *, UInt32);
    void nativeNotifyPowerState(UInt32, UInt32);
    bool init(OSDictionary * = nullptr) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    IOReturn setProperties(OSObject *) APPLE_KEXT_OVERRIDE;
    void systemWillShutdown(IOOptionBits) APPLE_KEXT_OVERRIDE;
    IOService *probe(IOService *, SInt32 *) APPLE_KEXT_OVERRIDE;
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    IOWorkLoop *getWorkLoop() const APPLE_KEXT_OVERRIDE;
    IOReturn newUserClient(task_t, void *, UInt32, IOUserClient **) APPLE_KEXT_OVERRIDE;
    IOReturn setPowerState(unsigned long, IOService *) APPLE_KEXT_OVERRIDE;
protected:
    virtual void nativeSlot168() APPLE_KEXT_OVERRIDE; // 168
    virtual void nativeSlot169() APPLE_KEXT_OVERRIDE; // 169
    virtual void nativeSlot170() APPLE_KEXT_OVERRIDE; // 170
    virtual void nativeSlot171() APPLE_KEXT_OVERRIDE; // 171
    virtual void nativeSlot172() APPLE_KEXT_OVERRIDE; // 172
    virtual void nativeSlot173() APPLE_KEXT_OVERRIDE; // 173
    virtual UInt32 default_fb_surface(UInt32, UInt32) APPLE_KEXT_OVERRIDE; // 174
    virtual UInt32 get_framebuffer_id() APPLE_KEXT_OVERRIDE; // 175
    virtual void nativeSlot176() APPLE_KEXT_OVERRIDE; // 176
    virtual void nativeSlot177() APPLE_KEXT_OVERRIDE; // 177
    virtual void nativeSlot178() APPLE_KEXT_OVERRIDE; // 178
    virtual void nativeSlot179() APPLE_KEXT_OVERRIDE; // 179
    virtual void nativeSlot180() APPLE_KEXT_OVERRIDE; // 180
    virtual void nativeSlot181() APPLE_KEXT_OVERRIDE; // 181
    virtual IOReturn set_display_device(UInt32) APPLE_KEXT_OVERRIDE; // 182
    virtual void nativeSlot183() APPLE_KEXT_OVERRIDE; // 183
    virtual bool is_main_display() APPLE_KEXT_OVERRIDE; // 184
    virtual void nativeSlot185() APPLE_KEXT_OVERRIDE; // 185
    virtual void nativeSlot186() APPLE_KEXT_OVERRIDE; // 186
    virtual void nativeSlot187() APPLE_KEXT_OVERRIDE; // 187
    virtual void nativeSlot188() APPLE_KEXT_OVERRIDE; // 188
    virtual void nativeSlot189() APPLE_KEXT_OVERRIDE; // 189
    virtual void nativeSlot190() APPLE_KEXT_OVERRIDE; // 190
    virtual void nativeSlot191() APPLE_KEXT_OVERRIDE; // 191
    virtual void nativeSlot192() APPLE_KEXT_OVERRIDE; // 192
    virtual void nativeSlot193() APPLE_KEXT_OVERRIDE; // 193
    virtual void nativeSlot194() APPLE_KEXT_OVERRIDE; // 194
    virtual void nativeSlot195() APPLE_KEXT_OVERRIDE; // 195
    virtual void nativeSlot196() APPLE_KEXT_OVERRIDE; // 196
    virtual void nativeSlot197() APPLE_KEXT_OVERRIDE; // 197
    virtual void nativeSlot198() APPLE_KEXT_OVERRIDE; // 198
    virtual void nativeSlot199() APPLE_KEXT_OVERRIDE; // 199
    virtual void nativeSlot200() APPLE_KEXT_OVERRIDE; // 200
    virtual void nativeSlot201() APPLE_KEXT_OVERRIDE; // 201
    virtual void nativeSlot202() APPLE_KEXT_OVERRIDE; // 202
    virtual void nativeSlot203() APPLE_KEXT_OVERRIDE; // 203
    virtual void nativeSlot204() APPLE_KEXT_OVERRIDE; // 204
    virtual void nativeSlot205() APPLE_KEXT_OVERRIDE; // 205
    virtual void nativeSlot206() APPLE_KEXT_OVERRIDE; // 206
    virtual void nativeSlot207() APPLE_KEXT_OVERRIDE; // 207
    virtual void nativeSlot208() APPLE_KEXT_OVERRIDE; // 208
    virtual void nativeSlot209() APPLE_KEXT_OVERRIDE; // 209
    virtual void nativeSlot210() APPLE_KEXT_OVERRIDE; // 210
    virtual void nativeSlot211() APPLE_KEXT_OVERRIDE; // 211
    virtual IOReturn set_notify_tag_mask_gated(UInt32, void *, UInt32) APPLE_KEXT_OVERRIDE = 0; // 212
    virtual void nativeSlot213() APPLE_KEXT_OVERRIDE; // 213
    virtual IOReturn supported_frame_info(UInt32, UInt32 *, char (*)[64], UInt32 *) APPLE_KEXT_OVERRIDE = 0; // 214
    virtual void nativeSlot215() APPLE_KEXT_OVERRIDE; // 215
    virtual void nativeSlot216() APPLE_KEXT_OVERRIDE; // 216
    virtual void nativeSlot217() APPLE_KEXT_OVERRIDE; // 217
    virtual void nativeSlot218() APPLE_KEXT_OVERRIDE; // 218
    virtual void nativeSlot219() APPLE_KEXT_OVERRIDE; // 219
    virtual void nativeSlot220() APPLE_KEXT_OVERRIDE; // 220
    virtual void nativeSlot221() APPLE_KEXT_OVERRIDE; // 221
    virtual void nativeSlot222() APPLE_KEXT_OVERRIDE; // 222
    virtual void nativeSlot223() APPLE_KEXT_OVERRIDE; // 223
    virtual void nativeSlot224() APPLE_KEXT_OVERRIDE; // 224
    virtual void nativeSlot225() APPLE_KEXT_OVERRIDE; // 225
    virtual void nativeSlot226() APPLE_KEXT_OVERRIDE; // 226
    virtual void nativeSlot227() APPLE_KEXT_OVERRIDE; // 227
    virtual void nativeSlot228() APPLE_KEXT_OVERRIDE; // 228
    virtual void nativeSlot229() APPLE_KEXT_OVERRIDE; // 229
    virtual void nativeSlot230() APPLE_KEXT_OVERRIDE; // 230
    virtual void nativeSlot231() APPLE_KEXT_OVERRIDE; // 231
    virtual void nativeSlot232() APPLE_KEXT_OVERRIDE; // 232
    virtual void nativeSlot233() APPLE_KEXT_OVERRIDE; // 233
    virtual void nativeSlot234() APPLE_KEXT_OVERRIDE; // 234
    virtual void nativeSlot235() APPLE_KEXT_OVERRIDE; // 235
    virtual void nativeSlot236() APPLE_KEXT_OVERRIDE; // 236
    virtual void nativeSlot237() APPLE_KEXT_OVERRIDE; // 237
    virtual void nativeSlot238() APPLE_KEXT_OVERRIDE; // 238
    virtual void nativeSlot239() APPLE_KEXT_OVERRIDE; // 239
    virtual void nativeSlot240() APPLE_KEXT_OVERRIDE; // 240
    virtual void nativeSlot241() APPLE_KEXT_OVERRIDE; // 241
    virtual void nativeSlot242() APPLE_KEXT_OVERRIDE; // 242
    virtual void nativeSlot243() APPLE_KEXT_OVERRIDE; // 243
    virtual void nativeSlot244() APPLE_KEXT_OVERRIDE; // 244
    virtual void nativeSlot245() APPLE_KEXT_OVERRIDE; // 245
    virtual void nativeSlot246() APPLE_KEXT_OVERRIDE; // 246
    virtual IOReturn factory_portal(void *, task_t) APPLE_KEXT_OVERRIDE = 0; // 247
    virtual IOReturn executeFactoryConfigurations_gated() APPLE_KEXT_OVERRIDE = 0; // 248
    virtual void nativeSlot249() APPLE_KEXT_OVERRIDE; // 249
    virtual void nativeSlot250() APPLE_KEXT_OVERRIDE; // 250
    virtual void nativeSlot251() APPLE_KEXT_OVERRIDE; // 251
    virtual void nativeSlot252() APPLE_KEXT_OVERRIDE; // 252
    virtual void nativeSlot253() APPLE_KEXT_OVERRIDE; // 253
    virtual void nativeSlot254() APPLE_KEXT_OVERRIDE; // 254
    virtual void nativeSlot255() APPLE_KEXT_OVERRIDE; // 255
    virtual void nativeSlot256() APPLE_KEXT_OVERRIDE; // 256
    virtual void nativeSlot257() APPLE_KEXT_OVERRIDE; // 257
    virtual void nativeSlot258() APPLE_KEXT_OVERRIDE; // 258
    virtual void nativeSlot259() APPLE_KEXT_OVERRIDE; // 259
    virtual void nativeSlot260() APPLE_KEXT_OVERRIDE; // 260
    virtual void nativeSlot261() APPLE_KEXT_OVERRIDE; // 261
    virtual void nativeSlot262() APPLE_KEXT_OVERRIDE; // 262
    virtual void nativeSlot263() APPLE_KEXT_OVERRIDE; // 263
    virtual void nativeSlot264() APPLE_KEXT_OVERRIDE; // 264
    virtual void nativeSlot265() APPLE_KEXT_OVERRIDE; // 265
    virtual void nativeSlot266(); // 266
    virtual void nativeSlot267(); // 267
    virtual void nativeSlot268(); // 268
    virtual void nativeSlot269(); // 269
    virtual void nativeSlot270(); // 270
    virtual void nativeSlot271(); // 271
    virtual void nativeSlot272(); // 272
    virtual void nativeSlot273(); // 273
    virtual void nativeSlot274(); // 274
    virtual void nativeSlot275(); // 275
    virtual void nativeSlot276(); // 276
    virtual void nativeSlot277(); // 277
    virtual void nativeSlot278(); // 278
    virtual void nativeSlot279(); // 279
    virtual void nativeSlot280(); // 280
    virtual void nativeSlot281(); // 281
    virtual void nativeSlot282(); // 282
    virtual void nativeSlot283(); // 283
    virtual void nativeSlot284(); // 284
    virtual void nativeSlot285(); // 285
    virtual void nativeSlot286(); // 286
    virtual void nativeSlot287(); // 287
    virtual void nativeSlot288(); // 288
    virtual void nativeSlot289(); // 289
    virtual void nativeSlot290(); // 290
    virtual void nativeSlot291(); // 291
    virtual void nativeSlot292(); // 292
    virtual void nativeSlot293(); // 293
    virtual void nativeSlot294(); // 294
    virtual void nativeSlot295(); // 295
    virtual bool start_hardware(); // 296
    virtual void stop_hardware_gated(); // 297
    virtual void swap_begin_gated(); // 298
    virtual void swap_complete_gated(); // 299
    virtual void nativeSlot300(); // 300; unused relocation alias
    virtual void nativeSlot301(); // 301; unused relocation alias
    virtual void nativeSlot302(); // 302
    virtual void nativeSlot303(); // 303
    virtual bool verify_swap(void *); // 304, first native submit check
    virtual void nativeSlot305(); // 305
    virtual IOReturn do_power_state_change_gated(); // 306, original native name/ABI
    virtual void create_default_fb_surface(); // 307
    virtual void nativeSlot308(); // 308
    virtual bool get_timing_attributes_gated(UInt64 *, bool) const; // 309
    virtual void nativeSlot310(); // 310
    virtual void nativeSlot311(); // 311
    virtual void nativeSlot312(); // 312
    virtual void nativeSlot313(); // 313
    virtual IOReturn set_frame_info_notification_state_gated(void *, bool) = 0; // 314
    virtual void do_swap_info_notify_gated(void *, void *) = 0; // 315
    virtual void nativeSlot316(); // 316
    virtual void nativeSlot317(); // 317
    virtual void nativeSlot318(); // 318
    virtual void nativeSlot319(); // 319
    virtual void nativeSlot320(); // 320
    virtual void nativeSlot321(); // 321
    virtual void nativeSlot322(); // 322
    virtual void nativeSlot323(); // 323
    virtual void nativeSlot324(); // 324
    virtual void nativeSlot325(); // 325
    virtual void nativeSlot326(); // 326
    virtual void nativeSlot327(); // 327
    virtual void nativeSlot328(); // 328
    // Audited local alias: native gated submit passes its request before
    // generic layer/KeepOnScreen checks; the Legacy implementation is RET.
    virtual void prepare_swap(void *); // 329
    virtual void nativeSlot330(); // 330
    virtual void nativeSlot331(); // 331
    virtual void nativeSlot332(); // 332
    virtual void nativeSlot333(); // 333
    virtual void nativeSlot334(); // 334
    virtual void nativeSlot335(); // 335
    virtual void nativeSlot336(); // 336
    virtual void nativeSlot337(); // 337
    virtual void nativeSlot338(); // 338
    virtual void nativeSlot339(); // 339
    virtual void nativeSlot340(); // 340
    virtual void nativeSlot341(); // 341
    virtual void nativeSlot342(); // 342
    virtual UInt32 default_fb_surface_gated(UInt32, UInt32); // 343
    virtual void nativeSlot344(); // 344
    virtual IOReturn deallocate_default_fb_surface_gated(); // 345
    virtual void nativeSlot346(); // 346
    virtual void notify_swap_done_gated(UInt32) = 0; // 347
    virtual void notify_crc_ready_gated(UInt32) = 0; // 348
    virtual void nativeSlot349(); // 349
    virtual void nativeSlot350(); // 350
    virtual void nativeSlot351(); // 351
    // Second Boolean check inside the native command gate. Audited local
    // alias; the earlier submit path separately calls verify_swap at304.
    virtual bool verify_swap_gated(void *); // 352
    virtual void nativeSlot353(); // 353
    virtual void nativeSlot354(); // 354
    // Slot 355 feeds the native "TimingEnable" diagnostic and reads the
    // Apple H7 timing-enabled byte. Local audited name, not an export claim.
    virtual bool timing_enabled() const = 0; // 355
    virtual void nativeSlot356(); // 356
    virtual void nativeSlot357(); // 357
    virtual void nativeSlot358(); // 358
    virtual void nativeSlot359(); // 359
    virtual void nativeSlot360(); // 360
    virtual void nativeSlot361(); // 361
    virtual void nativeSlot362(); // 362
    virtual void nativeSlot363(); // 363
    virtual void nativeSlot364(); // 364
    virtual void nativeSlot365(); // 365
    virtual void nativeSlot366(); // 366
    virtual void nativeSlot367(); // 367
};
static_assert(sizeof(IOMobileFramebuffer) == 144, "iOS 18 IOMFB interface ABI mismatch");
static_assert(sizeof(IOMobileFramebufferService) == 144, "iOS 18 IOMFB service ABI mismatch");
static_assert(sizeof(IOMobileFramebufferLegacy) == 6592, "iOS 18 IOMFB legacy ABI mismatch");
#endif
