// SPDX-License-Identifier: BSD-2-Clause
#include "GenericAES.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/crypto/aes.h>
#include <libkern/OSAtomic.h>
#include <pexpert/pexpert.h>

OSDefineMetaClassAndStructors(GenericAES, IOAESAccelerator)

static bool traceAES;
static volatile SInt32 tracedRequests;

static bool cbcSelfTest(GenericAES *service)
{
    // NIST SP 800-38A F.2.1, AES-128-CBC. Check both the provider's key
    // schedule and the direction/IV convention before publishing the service.
    const UInt8 key[16] = {0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c};
    UInt8 iv[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
    const UInt8 plain[16] = {0x6b,0xc1,0xbe,0xe2,0x2e,0x40,0x9f,0x96,0xe9,0x3d,0x7e,0x11,0x73,0x93,0x17,0x2a};
    const UInt8 cipher[16] = {0x76,0x49,0xab,0xac,0x81,0x19,0xb2,0x46,0xce,0xe9,0x8e,0x9b,0x12,0xe9,0x19,0x7d};
    IOBufferMemoryDescriptor *in = IOBufferMemoryDescriptor::withCapacity(16, kIODirectionInOut);
    IOBufferMemoryDescriptor *out = IOBufferMemoryDescriptor::withCapacity(16, kIODirectionInOut);
    if (!in || !out) {
        OSSafeReleaseNULL(in); OSSafeReleaseNULL(out);
        return false;
    }
    IOAESKey aesKey = {}; aesKey.bits = 128; memcpy(aesKey.bytes, key, 16);
    UInt8 result[16];
    bool ok = in->writeBytes(0, plain, 16) == 16 &&
        service->performAES(in, out, 16, iv, 0, &aesKey, nullptr, nullptr, nullptr, nullptr) == kIOReturnSuccess &&
        out->readBytes(0, result, 16) == 16 && !memcmp(result, cipher, 16) &&
        service->performAES(out, in, 16, iv, 1, &aesKey, nullptr, nullptr, nullptr, nullptr) == kIOReturnSuccess &&
        in->readBytes(0, result, 16) == 16 && !memcmp(result, plain, 16);
    aesKey.selector = 1000;
    ok = ok && service->performAES(in, out, 16, iv, 0, &aesKey, nullptr, nullptr, nullptr, nullptr) == kIOReturnUnsupported;
    bzero(&aesKey, sizeof(aesKey)); bzero(result, sizeof(result));
    in->release(); out->release();
    return ok;
}

bool GenericAES::start(IOService *provider)
{
    int trace = 0;
    traceAES = PE_parse_boot_argn("aes-compat-trace", &trace, sizeof(trace)) && trace != 0;
    // CBC runs synchronously in corecrypto; the native hardware command queues
    // and Apple key aliases are not used by this provider.
    if (!IOService::start(provider) || !cbcSelfTest(this)) return false;
    setProperty("AESSoftwareKeysOnly", true);
    IOLog("GenericAES: corecrypto AES-CBC self-test passed; software keys only\n");
    registerService();
    return true;
}

IOReturn GenericAES::performAES(IOMemoryDescriptor *input, IOMemoryDescriptor *output,
    IOByteCount length, void *iv, UInt32 operation, IOAESKey *key,
    void *inputOffset, void *outputOffset, void *completion, void *context)
{
    // Bounded, opt-in ABI diagnostics contain sizes and capabilities only.
    // Never log keys, IV contents, plaintext, ciphertext or kernel addresses.
    const bool log = traceAES && OSIncrementAtomic(&tracedRequests) < 32;
    auto finish = [&](IOReturn status, const char *reason) {
        if (log) IOLog("GenericAES: request op=%u bits=%u selector=%u bytes=%llu iv=%u in_place=%u offsets=%u completion=%u status=0x%x reason=%s\n",
            operation, key ? key->bits : 0, key ? key->selector : 0,
            static_cast<unsigned long long>(length), iv != nullptr, input == output,
            inputOffset != nullptr || outputOffset != nullptr,
            completion != nullptr || context != nullptr, status, reason);
        return status;
    };
    if (!input || !output || !key || !length || length > 0x80000 || (length & 15) ||
        inputOffset || outputOffset || completion || context) return finish(kIOReturnBadArgument, "arguments");
    if (operation > 1 || key->selector) return finish(kIOReturnUnsupported, "hardware-key-or-mode");
    if (key->bits != 128 && key->bits != 192 && key->bits != 256) return finish(kIOReturnBadArgument, "key-size");
    if (input->getLength() < length || output->getLength() < length) return finish(kIOReturnBadArgument, "descriptor-size");
    UInt8 *plain = static_cast<UInt8 *>(IOMalloc(length));
    UInt8 *cipher = static_cast<UInt8 *>(IOMalloc(length));
    if (!plain || !cipher) {
        if (plain) IOFree(plain, length);
        if (cipher) IOFree(cipher, length);
        return finish(kIOReturnNoMemory, "allocation");
    }
    IOReturn status = kIOReturnIOError;
    if (input->readBytes(0, plain, length) == length) {
        int error;
        if (operation == 0) {
            aes_encrypt_ctx schedule;
            error = aes_encrypt_key(key->bytes, key->bits / 8, &schedule);
            if (!error) error = aes_encrypt_cbc(plain, static_cast<const UInt8 *>(iv), length / 16, cipher, &schedule);
            bzero(&schedule, sizeof(schedule));
        } else {
            aes_decrypt_ctx schedule;
            error = aes_decrypt_key(key->bytes, key->bits / 8, &schedule);
            if (!error) error = aes_decrypt_cbc(plain, static_cast<const UInt8 *>(iv), length / 16, cipher, &schedule);
            bzero(&schedule, sizeof(schedule));
        }
        if (!error && output->writeBytes(0, cipher, length) == length) status = kIOReturnSuccess;
    }
    bzero(plain, length); bzero(cipher, length);
    IOFree(plain, length); IOFree(cipher, length);
    return finish(status, status == kIOReturnSuccess ? "software-key" : "software-operation");
}
