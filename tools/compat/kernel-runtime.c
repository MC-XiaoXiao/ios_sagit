#include <stdint.h>
/* The built-in kernel has process lifetime and no C++ destructor unloading. */
int __cxa_atexit(void (*destructor)(void *), void *argument, void *dso)
{
    (void)destructor;
    (void)argument;
    (void)dso;
    return 0;
}

/* OSKext.cpp's private profile ABI has bitmap/vtable arguments absent from
 * the public compiler-rt snapshot used for integer builtins. No PGO export
 * provider is installed: report zero payload and a writer error (EIO at the
 * OSKext API), instead of producing a misleading older raw-profile format.
 */
uint64_t __llvm_profile_get_size_for_buffer_internal(
    const char *data_begin, const char *data_end,
    const char *counters_begin, const char *counters_end,
    const char *bitmap_begin, const char *bitmap_end,
    const char *names_begin, const char *names_end,
    const char *vtable_begin, const char *vtable_end,
    const char *vnames_begin, const char *vnames_end)
{
    (void)data_begin; (void)data_end;
    (void)counters_begin; (void)counters_end;
    (void)bitmap_begin; (void)bitmap_end;
    (void)names_begin; (void)names_end;
    (void)vtable_begin; (void)vtable_end;
    (void)vnames_begin; (void)vnames_end;
    return 0;
}

int __llvm_profile_write_buffer_internal(char *buffer,
    const char *data_begin, const char *data_end,
    const char *counters_begin, const char *counters_end,
    const char *bitmap_begin, const char *bitmap_end,
    const char *names_begin, const char *names_end)
{
    (void)buffer;
    (void)data_begin; (void)data_end;
    (void)counters_begin; (void)counters_end;
    (void)bitmap_begin; (void)bitmap_end;
    (void)names_begin; (void)names_end;
    return 1;
}
