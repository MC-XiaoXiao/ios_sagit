/* SPDX-License-Identifier: MIT */
/* Linked into LinuxSimpleMassStorage's PID 1 after the USB gadget starts. */
#include "init.h"
#include "serial-command.h"
#include <poll.h>
#include <sys/reboot.h>
#include <termios.h>

static int open_console(void)
{
    // The shipped DT also enables a Bluetooth UART. Resolve the physical debug
    // port instead of relying on driver probe order for ttyMSM0/ttyMSM1.
    char link[PATH_MAX], resolved[PATH_MAX], node[64];
    for (unsigned int i = 0; i < 8; i++) {
        snprintf(link, sizeof(link), "/sys/class/tty/ttyMSM%u/device/of_node", i);
        if (!realpath(link, resolved) || !strstr(resolved, "/serial@c1b0000")) continue;
        snprintf(node, sizeof(node), "/dev/ttyMSM%u", i);
        return open(node, O_RDWR | O_NOCTTY | O_CLOEXEC);
    }
    return -1;
}

static void console_message(int fd, const char *text)
{
    size_t left = strlen(text);
    while (left) {
        ssize_t n = write(fd, text, left);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        text += n; left -= (size_t)n;
    }
}

static int flush_backends(void)
{
    list *item = list_first(gadget_blocks);
    sync();
    for (; item; item = item->next) {
        struct use_block *block = LIST_DATA(item, struct use_block *);
        int fd = open(block->path, O_RDWR | O_CLOEXEC);
        if (fd < 0) return -1;
        int rc = fsync(fd), saved = errno;
        close(fd);
        if (rc < 0) { errno = saved; return -1; }
    }
    return 0;
}

static void console_reboot(int fd)
{
    console_message(fd, "\r\n[mi6-storage] reboot: disconnecting USB and flushing storage\r\n");
    if (gadget_stop() != 0) {
        console_message(fd, "[mi6-storage] ERROR: USB disconnect failed; reboot cancelled\r\n");
        return;
    }
    if (flush_backends() != 0) {
        console_message(fd, "[mi6-storage] ERROR: storage flush failed; reboot cancelled\r\n");
        gadget_start();
        return;
    }
    console_message(fd, "[mi6-storage] flushed; rebooting to firmware\r\n");
    tcdrain(fd);
    if (reboot(RB_AUTOBOOT) < 0) {
        console_message(fd, "[mi6-storage] ERROR: reboot syscall failed\r\n");
        gadget_start();
    }
}

void serial_control_run(void)
{
    for (;;) {
        int fd = open_console();
        if (fd < 0) { sleep(2); continue; }
        struct termios attr;
        if (tcgetattr(fd, &attr) < 0) { close(fd); sleep(2); continue; }
        cfmakeraw(&attr);
        cfsetispeed(&attr, B115200); cfsetospeed(&attr, B115200);
        attr.c_cflag |= CLOCAL | CREAD;
        if (tcsetattr(fd, TCSANOW, &attr) < 0) { close(fd); sleep(2); continue; }
        struct storage_line line = {0};
        console_message(fd, "\r\n[mi6-storage] ready; commands: status, help, reboot\r\n");
        for (;;) {
            struct pollfd wait = { .fd = fd, .events = POLLIN };
            int rc = poll(&wait, 1, 1000);
            if (rc < 0 && errno == EINTR) continue;
            if (rc < 0 || wait.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
            if (!(wait.revents & POLLIN)) continue;
            unsigned char input[64];
            ssize_t count = read(fd, input, sizeof(input));
            if (count <= 0) { if (count < 0 && errno == EINTR) continue; break; }
            for (ssize_t i = 0; i < count; i++) {
                switch (storage_command_feed(&line, input[i])) {
                case STORAGE_STATUS: console_message(fd, "\r\n[mi6-storage] status: exporting UFS; serial-control=v1\r\n"); break;
                case STORAGE_HELP: console_message(fd, "\r\n[mi6-storage] commands: status, help, reboot\r\n"); break;
                case STORAGE_REBOOT: console_reboot(fd); break;
                case STORAGE_INVALID: console_message(fd, "\r\n[mi6-storage] unknown command; use help\r\n"); break;
                case STORAGE_NONE: break;
                }
            }
        }
        close(fd); sleep(1);
    }
}
