/* SPDX-License-Identifier: MIT */
#ifndef MI6_STORAGE_SERIAL_COMMAND_H
#define MI6_STORAGE_SERIAL_COMMAND_H
#include <stddef.h>
#include <string.h>

enum storage_command { STORAGE_NONE, STORAGE_STATUS, STORAGE_HELP, STORAGE_REBOOT, STORAGE_INVALID };
struct storage_line { char text[80]; size_t length; int overflow; };

static inline enum storage_command storage_command_feed(struct storage_line *line, unsigned char c)
{
    if (c == '\r' || c == '\n') {
        enum storage_command result = STORAGE_NONE;
        line->text[line->length] = 0;
        size_t first = 0, end = line->length;
        while (first < end && (line->text[first] == ' ' || line->text[first] == '\t')) first++;
        while (end > first && (line->text[end-1] == ' ' || line->text[end-1] == '\t')) end--;
        line->text[end] = 0;
        if (line->overflow) result = STORAGE_INVALID;
        else if (end > first) {
            const char *s = line->text + first;
            if (!strcmp(s, "reboot")) result = STORAGE_REBOOT;
            else if (!strcmp(s, "status")) result = STORAGE_STATUS;
            else if (!strcmp(s, "help")) result = STORAGE_HELP;
            else result = STORAGE_INVALID;
        }
        line->length = 0; line->overflow = 0;
        return result;
    }
    if (line->overflow) return STORAGE_NONE;
    if (c == 8 || c == 127) { if (line->length) line->length--; return STORAGE_NONE; }
    if (c != '\t' && (c < 32 || c > 126)) { line->overflow = 1; return STORAGE_NONE; }
    if (line->length == sizeof(line->text)-1) line->overflow = 1;
    else line->text[line->length++] = (char)c;
    return STORAGE_NONE;
}
#endif
