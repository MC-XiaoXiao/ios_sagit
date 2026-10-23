"""Execute cached IIG request/reply code against the current IORPC ABI."""
import importlib.util
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('iig_toolchain', ROOT/'scripts/toolchain.py')
toolchain = importlib.util.module_from_spec(spec)
spec.loader.exec_module(toolchain)


def function(text, signature):
    start = text.index('\n' + signature) + 1
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


class IIGCompatibilityTests(unittest.TestCase):
    def test_all_pinned_callers_including_oneway_are_adapted(self):
        callers = oneway = 0
        for path in (ROOT/'tools/iig').glob('*.iig.cpp'):
            text = path.read_text()
            count = len(re.findall(r'IORPC rpc = \{', text))
            callers += count
            oneway += text.count('.reply = NULL,')
            adapted = toolchain.iig_kernel_call_content(text)
            self.assertEqual(adapted.count('rpc.kernelContent = &buf.msg.content.__hdr;'), count, path)
            self.assertEqual(toolchain.iig_kernel_call_content(adapted), adapted, path)
        self.assertGreater(callers, 90)
        self.assertGreater(oneway, 0)

    def test_unknown_caller_layout_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'unrecognized cached IIG caller'):
            toolchain.iig_kernel_call_content('    IORPC rpc = { .message = external };')

    def test_generated_state_notification_roundtrip(self):
        text = (ROOT/'tools/iig/IOService.iig.cpp').read_text()
        structs = text[text.index('struct IOService_StateNotificationItemCreate_Msg_Content'):
                       text.index('struct IOService_StateNotificationItemSet_Msg_Content')]
        request = 'kern_return_t\n' + function(text, 'IOService::StateNotificationItemCreate(')
        invoke = 'kern_return_t\n' + function(text, 'IOService::StateNotificationItemCreate_Invoke(')
        prefix = r'''
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#define XNU_PLATFORM_DriverKit 1
typedef int kern_return_t;
#include "IORPC.h"
constexpr kern_return_t kIOReturnSuccess = 0;
constexpr kern_return_t kIOReturnIPCError = (int)0xe00002bf;
constexpr kern_return_t kIOReturnBadArgument = (int)0xe00002c2;
constexpr uint64_t IOService_StateNotificationItemCreate_ID = 0xb35947bdab354e9eULL;
class OSMetaClassBase { public: virtual ~OSMetaClassBase() = default; };
class OSObject : public OSMetaClassBase { public: virtual kern_return_t Invoke(IORPC); };
class OSString : public OSObject {};
class OSDictionary : public OSObject {};
#define OSDynamicCast(type, object) dynamic_cast<type *>(object)
class IOService : public OSObject {
public:
    typedef kern_return_t (*StateNotificationItemCreate_Handler)(OSMetaClassBase *, OSString *, OSDictionary *);
    kern_return_t StateNotificationItemCreate(OSString *, OSDictionary *, OSDispatchMethod = nullptr);
    static kern_return_t StateNotificationItemCreate_Invoke(IORPC, OSMetaClassBase *, StateNotificationItemCreate_Handler);
};
'''
        suffix = r'''
static OSString * expectedName;
static unsigned calls;
static kern_return_t handler(OSMetaClassBase *, OSString *name, OSDictionary *schema) {
    assert(name == expectedName && schema == nullptr);
    ++calls;
    return kIOReturnSuccess;
}
kern_return_t OSObject::Invoke(IORPC rpc) {
#if KERNEL
    // The current XNU Invoke gate requires the caller's kernel content pointer.
    IORPCMessage *message = rpc.kernelContent;
    if (!message) return kIOReturnIPCError;
    const auto *msg = reinterpret_cast<const IOService_StateNotificationItemCreate_Msg *>(rpc.message);
    assert(message == &msg->content.__hdr);
    message->flags |= kIORPCMessageKernel;
#endif
    return IOService::StateNotificationItemCreate_Invoke(rpc, this, handler);
}
int main() {
    IOService service;
    OSString name;
    expectedName = &name;
    auto ret = service.StateNotificationItemCreate(&name, nullptr);
#if EXPECT_FAILURE
    assert(ret == kIOReturnIPCError && calls == 0);
#else
    assert(ret == kIOReturnSuccess && calls == 1);
#endif
}
'''
        with tempfile.TemporaryDirectory(prefix='iig-rpc-') as directory:
            work = Path(directory)
            # Use the actual current kernel ABI header, not a model of IORPC.
            (work/'IORPC.h').write_bytes((ROOT/'kernel/iokit/DriverKit/IORPC.h').read_bytes())
            for name, kernel, failure, caller in (
                    ('baseline', 1, 1, request),
                    ('fixed-kernel', 1, 0, toolchain.iig_kernel_call_content(request)),
                    ('fixed-userspace', 0, 0, toolchain.iig_kernel_call_content(request))):
                with self.subTest(name=name):
                    source, executable = work/(name+'.cpp'), work/name
                    source.write_text(prefix + structs + caller + invoke + suffix)
                    result = subprocess.run(['clang++', '-std=c++17', '-Werror',
                                             '-Wno-address-of-packed-member', f'-DKERNEL={kernel}',
                                             f'-DEXPECT_FAILURE={failure}', str(source), '-o', str(executable)],
                                            capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    unittest.main()
