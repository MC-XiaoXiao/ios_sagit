// SPDX-License-Identifier: MIT
// Read-only virtual container: original file plus APFSCOW1 changed blocks.
// No full-sized output image is created. libfuse3 is the only dependency.
#define FUSE_USE_VERSION 31
#define _FILE_OFFSET_BITS 64
#include <fuse3/fuse.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int source_fd;
static const unsigned char *delta;
static uint64_t size_bytes, count, base_offset;
static uint64_t le64(const unsigned char *p) {
    uint64_t n = 0;
    for (int i = 7; i >= 0; --i) n = (n << 8) | p[i];
    return n;
}
static int view_getattr(const char *path, struct stat *st, struct fuse_file_info *fi) {
    (void)fi;
    memset(st, 0, sizeof(*st));
    st->st_uid = getuid(); st->st_gid = getgid();
    if (!strcmp(path, "/")) {st->st_mode = S_IFDIR | 0500; st->st_nlink = 2; return 0;}
    if (strcmp(path, "/container.apfs")) return -ENOENT;
    st->st_mode = S_IFREG | 0400; st->st_nlink = 1;
    st->st_size = size_bytes; st->st_blksize = 4096;
    return 0;
}
static int view_readdir(const char *path, void *buf, fuse_fill_dir_t fill,
                        off_t off, struct fuse_file_info *fi, enum fuse_readdir_flags flags) {
    (void)off; (void)fi; (void)flags;
    if (strcmp(path, "/")) return -ENOENT;
    fill(buf, ".", NULL, 0, 0); fill(buf, "..", NULL, 0, 0);
    fill(buf, "container.apfs", NULL, 0, 0);
    return 0;
}
static int view_open(const char *path, struct fuse_file_info *fi) {
    if (strcmp(path, "/container.apfs")) return -ENOENT;
    if ((fi->flags & O_ACCMODE) != O_RDONLY) return -EROFS;
    return 0;
}
static const unsigned char *lookup(uint64_t block) {
    uint64_t lo = 0, hi = count;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        const unsigned char *r = delta + 24 + mid * 4104;
        uint64_t index = le64(r);
        if (index < block) lo = mid + 1; else hi = mid;
    }
    if (lo < count && le64(delta + 24 + lo * 4104) == block)
        return delta + 32 + lo * 4104;
    return NULL;
}
static int view_read(const char *path, char *buf, size_t length, off_t offset,
                     struct fuse_file_info *fi) {
    (void)fi;
    if (strcmp(path, "/container.apfs")) return -ENOENT;
    if (offset < 0) return -EINVAL;
    if ((uint64_t)offset >= size_bytes) return 0;
    if (length > size_bytes - offset) length = size_bytes - offset;
    size_t done = 0;
    while (done < length) {
        uint64_t pos = (uint64_t)offset + done;
        size_t within = pos % 4096, n = 4096 - within;
        if (n > length - done) n = length - done;
        const unsigned char *block = lookup(pos / 4096);
        if (block) memcpy(buf + done, block + within, n);
        else {
            ssize_t got = pread(source_fd, buf + done, n, base_offset + pos);
            if (got < 0) return -errno;
            if ((size_t)got != n) return -EIO;
        }
        done += n;
    }
    return done;
}
int main(int argc, char **argv) {
    if (argc != 4 && argc != 5) {
        fprintf(stderr, "Usage: %s SOURCE DELTA MOUNTPOINT [SOURCE_BYTE_OFFSET]\n", argv[0]);
        return 2;
    }
    if (argc == 5) {
        char *end = NULL;
        base_offset = strtoull(argv[4], &end, 10);
        if (!*argv[4] || *end || base_offset % 4096) return 2;
    }
    source_fd = open(argv[1], O_RDONLY | O_CLOEXEC);
    int patch_fd = open(argv[2], O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (source_fd < 0 || patch_fd < 0 || fstat(patch_fd, &st)) {perror("open"); return 1;}
    if (st.st_size < 24) return 2;
    delta = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, patch_fd, 0);
    if (delta == MAP_FAILED) {perror("mmap"); return 1;}
    size_bytes = le64(delta + 8); count = le64(delta + 16);
    if (memcmp(delta, "APFSCOW1", 8) || size_bytes == 0 || size_bytes % 4096 ||
        size_bytes > INT64_MAX || base_offset > INT64_MAX - size_bytes ||
        count > ((uint64_t)st.st_size - 24) / 4104 || (uint64_t)st.st_size != 24 + count * 4104)
        return 2;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t block = le64(delta + 24 + i * 4104);
        if (block >= size_bytes / 4096 || (i && block <= le64(delta + 24 + (i - 1) * 4104))) return 2;
    }
    struct fuse_operations ops = {.getattr = view_getattr, .readdir = view_readdir,
                                  .open = view_open, .read = view_read};
    char *args[] = {argv[0], "-f", "-s", "-o", "ro,default_permissions", argv[3]};
    return fuse_main(6, args, &ops, NULL);
}
