#ifndef XNU_COPYFILE_H
#define XNU_COPYFILE_H
#include <errno.h>
#include <unistd.h>
#define COPYFILE_DATA 1
/* SETUP/installfile uses only COPYFILE_DATA. Preserve partial-write semantics. */
static inline int fcopyfile(int source, int destination, void *state, unsigned flags)
{
    char buffer[65536];
    ssize_t n;
    (void)state;
    if (flags != COPYFILE_DATA) { errno = EINVAL; return -1; }
    for (;;) {
        n = read(source, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return n < 0 ? -1 : 0;
        for (ssize_t offset = 0; offset < n;) {
            ssize_t written = write(destination, buffer + offset, n - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) { if (!written) errno = EIO; return -1; }
            offset += written;
        }
    }
}
#endif
