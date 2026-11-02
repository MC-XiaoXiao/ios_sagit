// SPDX-License-Identifier: BSD-2-Clause
// Bridge menu. Firmware continues to own input, display and EFI applications.
#include "boot_menu.h"
#include <Protocol/DevicePath.h>
#include <Protocol/SimpleTextInEx.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SerialIo.h>

static EFI_GUID input_guid = EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID;
static EFI_GUID path_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID fv_guid = {0x220e73b6,0x6bdb,0x4413,{0x84,0x05,0xb9,0x74,0xb1,0x08,0x61,0x9a}};
static EFI_GUID app_guids[] = {
    {0x9588502a,0x5370,0x11e3,{0x86,0x31,0xd7,0xc5,0x95,0x13,0x64,0xc8}}, // Fastboot
    {0x7c04a583,0x9e3e,0x4f1c,{0xad,0x65,0xe0,0x52,0x68,0xd0,0xb4,0xd1}}, // Shell
    {0x6d77b2bb,0x69eb,0x42ab,{0xbe,0xcf,0x4f,0x40,0xc8,0x95,0x68,0xc3}}, // SimpleInit
};
static const struct bridge_boot_menu_option default_options[] = {
    {{(const uint16_t *)L"启动 XNU", (const uint16_t *)L"继续启动系统", MENU_ICON_BOOT}, L"Start XNU", NULL},
    {{(const uint16_t *)L"Fastboot", (const uint16_t *)L"连接电脑进行传输与调试", MENU_ICON_USB}, L"Fastboot", &app_guids[0]},
    {{(const uint16_t *)L"UEFI Shell", (const uint16_t *)L"进入固件命令行", MENU_ICON_SHELL}, L"UEFI Shell", &app_guids[1]},
    {{(const uint16_t *)L"EDK2 菜单", (const uint16_t *)L"打开原有固件菜单", MENU_ICON_FIRMWARE}, L"EDK2 menu", &app_guids[2]},
};

struct menu_graphics {
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *pixels;
    UINTN bytes;
};

struct menu_input {
    EFI_HANDLE *handles;
    UINTN count;
};

static int read_key(EFI_SYSTEM_TABLE *st, const struct menu_input *input, EFI_INPUT_KEY *key) {
    for (UINTN i = 0; i < input->count; i++) {
        EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *protocol;
        EFI_KEY_DATA data;
        if (!EFI_ERROR(st->BootServices->HandleProtocol(input->handles[i], &input_guid, (void **)&protocol)) &&
            !EFI_ERROR(protocol->ReadKeyStrokeEx(protocol, &data))) {
            *key = data.Key;
            if (key->ScanCode || key->UnicodeChar) return 1;
        }
    }
    return st->ConIn && !EFI_ERROR(st->ConIn->ReadKeyStroke(st->ConIn, key)) &&
        (key->ScanCode || key->UnicodeChar);
}

// Keep UART diagnostics separate from ConOut: writing the firmware text console
// during a graphical redraw can paint over the back buffer and cause flashing.
static void serial_log(EFI_SYSTEM_TABLE *st, const struct bridge_boot_menu_config *config,
                       unsigned selected, int remaining) {
    if (!st->BootServices->LocateProtocol) return;
    EFI_GUID guid = EFI_SERIAL_IO_PROTOCOL_GUID;
    EFI_SERIAL_IO_PROTOCOL *serial = NULL;
    if (EFI_ERROR(st->BootServices->LocateProtocol(&guid, NULL, (void **)&serial)) || !serial || !serial->Write) return;
    char line[256];
    const char *prefix = "[bridge menu] ";
    UINTN n = 0;
    while (*prefix) line[n++] = *prefix++;
    const CHAR16 *label = config->options[selected].console_title;
    if (!label) label = (const CHAR16 *)config->options[selected].label.title;
    for (UINTN i = 0; label && label[i] && n < sizeof(line) - 40; i++)
        line[n++] = label[i] < 128 ? (char)label[i] : '?';
    const char *state = remaining < 0 ? " | waiting for input\r\n" : " | automatic boot pending\r\n";
    while (*state) line[n++] = *state++;
    serial->Write(serial, &n, line);
}

static int draw(EFI_SYSTEM_TABLE *st, struct menu_graphics *graphics,
                const struct bridge_boot_menu_config *config, unsigned selected, int remaining, int failed) {
    EFI_BOOT_SERVICES *bs = st->BootServices;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_GUID guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    if (bs->LocateProtocol && !EFI_ERROR(bs->LocateProtocol(&guid, NULL, (void **)&gop)) &&
        gop && gop->Blt && gop->Mode && gop->Mode->Info) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = gop->Mode->Info;
        unsigned w = info->HorizontalResolution, h = info->VerticalResolution;
        if (w >= 320 && h >= 320 && w <= 4096 && h <= 4096 && (UINTN)w * h <= 8388608) {
            UINTN bytes = (UINTN)w * h * sizeof(*graphics->pixels);
            if (graphics->pixels && bytes != graphics->bytes) {
                bs->FreePool(graphics->pixels); graphics->pixels = NULL; graphics->bytes = 0;
            }
            if (!graphics->pixels && !EFI_ERROR(bs->AllocatePool(EfiBootServicesData, bytes, (void **)&graphics->pixels)))
                graphics->bytes = bytes;
            if (graphics->pixels) {
                struct bridge_menu_label labels[BRIDGE_MENU_MAX_ITEMS];
                for (unsigned i = 0; i < config->count; i++) labels[i] = config->options[i].label;
                struct bridge_menu_view view = {
                    (const uint16_t *)config->device, labels, config->count, selected,
                    remaining, config->seconds, failed
                };
                struct bridge_menu_surface surface = {(uint32_t *)graphics->pixels, w, h, w};
                if (!bridge_menu_render(&surface, &view) &&
                    !EFI_ERROR(gop->Blt(gop, graphics->pixels, EfiBltBufferToVideo, 0, 0, 0, 0, w, h, w * sizeof(*graphics->pixels)))) {
                    serial_log(st, config, selected, remaining);
                    return 1;
                }
            }
        }
    }
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out = st->ConOut;
    if (!out) return 0;
    out->ClearScreen(out);
    out->OutputString(out, L"XnuBridge boot menu\r\n\r\n");
    for (unsigned i = 0; i < config->count; i++) {
        out->OutputString(out, i == selected ? L" > " : L"   ");
        const CHAR16 *label = config->options[i].console_title;
        out->OutputString(out, (CHAR16 *)(label ? label : (const CHAR16 *)config->options[i].label.title));
        out->OutputString(out, L"\r\n");
    }
    out->OutputString(out, L"\r\nVolume +/-: select   Power/Enter: start\r\nNumber keys: start directly\r\n");
    if (remaining >= 0) {
        CHAR16 number[] = {(CHAR16)(L'0' + remaining / 10), (CHAR16)(L'0' + remaining % 10), 0};
        out->OutputString(out, L"Automatic boot in ");
        out->OutputString(out, number + (remaining < 10));
        out->OutputString(out, L" seconds. Any key stops the countdown.\r\n");
    }
    if (failed) out->OutputString(out, L"EFI application returned or could not start. Select an option.\r\n");
    return 1;
}

// Append a firmware file node to the installed FV device path. LoadImage gets
// the original firmware path, including its normal image authentication policy.
static EFI_STATUS app_path(EFI_BOOT_SERVICES *bs, EFI_HANDLE fv, EFI_GUID *file,
                           EFI_DEVICE_PATH_PROTOCOL **result) {
    EFI_DEVICE_PATH_PROTOCOL *source;
    EFI_STATUS status = bs->HandleProtocol(fv, &path_guid, (void **)&source);
    if (EFI_ERROR(status)) return status;
    if (!source) return EFI_COMPROMISED_DATA;
    UINTN size = 0;
    for (;;) {
        EFI_DEVICE_PATH_PROTOCOL *node = (void *)((UINT8 *)source + size);
        UINTN length = node->Length[0] | ((UINTN)node->Length[1] << 8);
        if (length < sizeof(*node) || length > 65536 - size) return EFI_COMPROMISED_DATA;
        if (node->Type == END_DEVICE_PATH_TYPE) {
            if (node->SubType != END_ENTIRE_DEVICE_PATH_SUBTYPE || length != sizeof(*node))
                return EFI_UNSUPPORTED;
            break;
        }
        size += length;
        if (size > 65536 - sizeof(*node)) return EFI_COMPROMISED_DATA;
    }
    UINTN total = size + sizeof(MEDIA_FW_VOL_FILEPATH_DEVICE_PATH) + sizeof(*source);
    UINT8 *path = NULL;
    status = bs->AllocatePool(EfiBootServicesData, total, (void **)&path);
    if (EFI_ERROR(status)) return status;
    for (UINTN i = 0; i < size; i++) path[i] = ((UINT8 *)source)[i];
    MEDIA_FW_VOL_FILEPATH_DEVICE_PATH *node = (void *)(path + size);
    node->Header.Type = MEDIA_DEVICE_PATH;
    node->Header.SubType = MEDIA_PIWG_FW_FILE_DP;
    node->Header.Length[0] = sizeof(*node);
    node->Header.Length[1] = 0;
    node->FvFileName = *file;
    EFI_DEVICE_PATH_PROTOCOL *end = (void *)(node + 1);
    end->Type = END_DEVICE_PATH_TYPE;
    end->SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
    end->Length[0] = sizeof(*end);
    end->Length[1] = 0;
    *result = (void *)path;
    return EFI_SUCCESS;
}

static EFI_STATUS start_app(EFI_HANDLE parent, EFI_SYSTEM_TABLE *st, const EFI_GUID *file) {
    EFI_BOOT_SERVICES *bs = st->BootServices;
    EFI_HANDLE *handles = NULL, child = NULL;
    UINTN count = 0;
    EFI_STATUS status = bs->LocateHandleBuffer(ByProtocol, &fv_guid, NULL, &count, &handles);
    if (EFI_ERROR(status)) return status;
    status = EFI_NOT_FOUND;
    for (UINTN i = 0; i < count; i++) {
        EFI_DEVICE_PATH_PROTOCOL *path = NULL;
        status = app_path(bs, handles[i], (EFI_GUID *)file, &path);
        if (status == EFI_NOT_FOUND || status == EFI_UNSUPPORTED) continue;
        if (EFI_ERROR(status)) break;
        status = bs->LoadImage(FALSE, parent, path, NULL, 0, &child);
        bs->FreePool(path);
        if (!EFI_ERROR(status)) break;
        // LoadImage can return a handle for an image rejected by Secure Boot.
        if (child) { bs->UnloadImage(child); child = NULL; }
        if (status != EFI_NOT_FOUND) break;
    }
    bs->FreePool(handles);
    if (!EFI_ERROR(status) && child) {
        status = bs->StartImage(child, NULL, NULL);
        bs->UnloadImage(child);
    }
    return status;
}

EFI_STATUS bridge_boot_menu_configured(EFI_HANDLE image, EFI_SYSTEM_TABLE *st,
                                      const struct bridge_boot_menu_config *config) {
    if (!st || !st->BootServices || !config || !config->options || !config->count ||
        config->count > BRIDGE_MENU_MAX_ITEMS || config->initial_selection >= config->count ||
        config->seconds > 30 || config->escape_selection < -1 ||
        config->escape_selection >= (int)config->count) return EFI_INVALID_PARAMETER;
    for (unsigned i = 0; i < config->count; i++)
        if (!config->options[i].label.title) return EFI_INVALID_PARAMETER;
    unsigned seconds = config->seconds;
    struct menu_graphics graphics = {NULL, 0};
    EFI_BOOT_SERVICES *bs = st->BootServices;
    struct menu_input input = {NULL, 0};
    EFI_STATUS status = bs->LocateHandleBuffer(ByProtocol, &input_guid, NULL, &input.count, &input.handles);
    if (EFI_ERROR(status)) { input.handles = NULL; input.count = 0; }
    // Headless or input-less boot must never wait in an inaccessible menu.
    if (!input.count && !st->ConIn) {
        if (input.handles) bs->FreePool(input.handles);
        return EFI_SUCCESS;
    }
    unsigned selected = config->initial_selection, tick = 0;
    int countdown = 1, redraw = 1, failed = 0;
    for (;;) {
        int host_action = config->host && config->host->dispatch ?
            config->host->dispatch(config->host->context) : 0;
        if (host_action && countdown) { countdown = 0; redraw = 1; }
        if (host_action == 2) {
            status = config->host->leave ? config->host->leave(config->host->context, 0) : EFI_SUCCESS;
            if (!EFI_ERROR(status)) break;
            failed = redraw = 1;
        }
        if (redraw) {
            if (!draw(st, &graphics, config, selected, countdown ? (int)seconds - (int)(tick / 10) : -1, failed)) break;
            redraw = 0;
        }
        EFI_INPUT_KEY key = {0, 0};
        int activate = 0;
        if (read_key(st, &input, &key)) {
            countdown = 0;
            redraw = 1;
            failed = 0;
            if (key.ScanCode == SCAN_UP || key.ScanCode == SCAN_VOLUME_UP)
                selected = (selected + config->count - 1) % config->count;
            else if (key.ScanCode == SCAN_DOWN || key.ScanCode == SCAN_VOLUME_DOWN)
                selected = (selected + 1) % config->count;
            else if (key.ScanCode == SCAN_ESC && config->escape_selection >= 0) { selected = (unsigned)config->escape_selection; activate = 1; }
            else if (key.UnicodeChar >= L'1' && key.UnicodeChar < L'1' + config->count) {
                selected = key.UnicodeChar - L'1'; activate = 1;
            } else if (key.ScanCode == SCAN_SUSPEND || key.UnicodeChar == L'\r' ||
                       key.UnicodeChar == L'\n' || key.UnicodeChar == L' ') activate = 1;
        }
        // Even zero delay samples queued/held keys once.
        if (countdown && tick >= seconds * 10) { countdown = 0; activate = 1; }
        if (activate) {
            if (host_action < 0) continue;
            if (config->host && config->host->leave) {
                status = config->host->leave(config->host->context,
                    config->options[selected].firmware_file != NULL);
                if (EFI_ERROR(status)) { failed = redraw = 1; continue; }
            }
            if (!config->options[selected].firmware_file) break;
            status = start_app(image, st, config->options[selected].firmware_file);
            if (config->host && config->host->returned) config->host->returned(config->host->context);
            // Apps can leave an EFI watchdog armed when they return.
            EFI_STATUS watchdog = bs->SetWatchdogTimer(0, 0, 0, NULL);
            if (EFI_ERROR(watchdog)) { status = watchdog; goto done; }
            if (st->ConOut) st->ConOut->OutputString(st->ConOut, EFI_ERROR(status) ? L"\r\nEFI application failed.\r\n" : L"\r\nEFI application exited.\r\n");
            failed = redraw = 1;
        }
        status = bs->Stall(100000);
        if (EFI_ERROR(status)) goto done;
        if (countdown && ++tick % 10 == 0) redraw = 1;
    }
    status = EFI_SUCCESS;
done:
    if (graphics.pixels) bs->FreePool(graphics.pixels);
    if (input.handles) bs->FreePool(input.handles);
    return status;
}

EFI_STATUS bridge_boot_menu(EFI_HANDLE image, EFI_SYSTEM_TABLE *st, unsigned seconds) {
    return bridge_boot_menu_hosted(image, st, seconds, NULL);
}
EFI_STATUS bridge_boot_menu_hosted(EFI_HANDLE image, EFI_SYSTEM_TABLE *st, unsigned seconds,
                                   const struct bridge_menu_host *host) {
    const struct bridge_boot_menu_config config = {
        L"UEFI BOOT MANAGER", default_options,
        sizeof(default_options) / sizeof(default_options[0]), 0, seconds, 1, host
    };
    return bridge_boot_menu_configured(image, st, &config);
}
