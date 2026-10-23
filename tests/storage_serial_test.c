#include "../boot/storage/serial-command.h"
#include <assert.h>

static enum storage_command feed(struct storage_line *line, const char *s)
{
    enum storage_command result = STORAGE_NONE;
    for (; *s; s++) {
        enum storage_command next = storage_command_feed(line, (unsigned char)*s);
        if (next != STORAGE_NONE) result = next;
    }
    return result;
}
int main(void)
{
    struct storage_line line = {0};
    assert(feed(&line, "rebo") == STORAGE_NONE);
    assert(feed(&line, "ot\r\n") == STORAGE_REBOOT);
    assert(feed(&line, "\t status \r") == STORAGE_STATUS);
    assert(feed(&line, "help\n") == STORAGE_HELP);
    assert(feed(&line, "reboot-now\n") == STORAGE_INVALID);
    assert(feed(&line, "rebox\bot\n") == STORAGE_REBOOT);
    assert(feed(&line, "\n\r") == STORAGE_NONE);
    for (int i = 0; i < 200; i++) storage_command_feed(&line, 'x');
    assert(feed(&line, "reboot\n") == STORAGE_INVALID);
    assert(feed(&line, "status\n") == STORAGE_STATUS);
    assert(feed(&line, "re\001boot\n") == STORAGE_INVALID);
    assert(feed(&line, "reboot\n") == STORAGE_REBOOT);
    return 0;
}
