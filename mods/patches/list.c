/* See list.h. The file comes from the page or a download, so nothing in it is trusted: every line
 * is checked, and edits may only touch the two files Nickel runs from. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "list.h"

#define FORMAT "1"
#define MAX_BYTES (1 << 20)
#define MAX_PATCHES 256
#define MAX_EDITS 4096
#define MAX_HEX (1 << 16)

static const char *const allowed_files[] = {"/usr/local/Kobo/libnickel.so.1.0.0", "/usr/local/Kobo/nickel"};

/* Everything the list points into: the file's text, split in place, and the arrays. */
typedef struct {
    Firmware firmware;
    char *text;
    Patch *patches;
    Edit *edits;
} Owned;

static int fail(char *error, size_t size, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(error, size, fmt, args);
    va_end(args);
    return 0;
}

static int split(char *line, char **fields, int most) {
    int count = 0;
    while (count < most) {
        fields[count++] = line;
        char *tab = strchr(line, '\t');
        if (!tab) break;
        *tab = 0;
        line = tab + 1;
    }
    return strchr(fields[count - 1], '\t') ? most + 1 : count;
}

static int is_hex(const char *text) {
    size_t length = strlen(text);
    if (!length || length % 2 || length > MAX_HEX) return 0;
    for (size_t i = 0; i < length; i++)
        if (!strchr("0123456789abcdef", text[i])) return 0;
    return 1;
}

static int allowed(const char *file) {
    for (size_t i = 0; i < sizeof allowed_files / sizeof *allowed_files; i++)
        if (!strcmp(file, allowed_files[i])) return 1;
    return 0;
}

static int parse(Owned *list, char *error, size_t size) {
    char *fields[8], *next = list->text;
    int line_number = 0, patches = 0, edits = 0;
    Patch *patch = 0;
    while (next && *next) {
        char *line = next;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        size_t length = strlen(line);
        if (length && line[length - 1] == '\r') line[length - 1] = 0;
        line_number++;
        int count = split(line, fields, 7);
        if (line_number == 1) {
            if (count != 2 || strcmp(fields[0], "kobo-patcher-list") || strcmp(fields[1], FORMAT))
                return fail(error, size, "it isn't a patch list this mod can read");
        } else if (line_number == 2) {
            if (count != 4 || strcmp(fields[0], "firmware")) return fail(error, size, "line 2 should name the firmware");
            list->firmware.model = fields[1];
            list->firmware.version = fields[2];
            list->firmware.name = fields[3];
        } else if (!strcmp(fields[0], "patch")) {
            if (count != 7 || patches == MAX_PATCHES) return fail(error, size, "line %d is a patch that can't be read", line_number);
            if (patch && !patch->edit_count) return fail(error, size, "a patch before line %d has no edits", line_number);
            patch = &list->patches[patches++];
            *patch = (Patch){fields[1], fields[5], fields[2], strcmp(fields[3], "-") ? fields[3] : 0, *fields[6] ? fields[6] : 0,
                             !strcmp(fields[4], "restart"), &list->edits[edits], 0};
        } else if (!strcmp(fields[0], "edit")) {
            char *end;
            unsigned long at = count == 5 ? strtoul(fields[2], &end, 10) : 0;
            if (count != 5 || !patch || edits == MAX_EDITS || *end || !allowed(fields[1]) || !is_hex(fields[3]) || !is_hex(fields[4]) ||
                strlen(fields[3]) != strlen(fields[4]) || at > 0x7fffffff)
                return fail(error, size, "line %d is an edit that can't be read", line_number);
            list->edits[edits++] = (Edit){fields[1], (unsigned)at, fields[3], fields[4]};
            patch->edit_count++;
        } else if (*fields[0]) {
            return fail(error, size, "line %d isn't something this mod knows", line_number);
        }
    }
    if (line_number < 2) return fail(error, size, "it's empty");
    if (patch && !patch->edit_count) return fail(error, size, "the last patch has no edits");
    list->firmware.patches = list->patches;
    list->firmware.patch_count = patches;
    return 1;
}

Firmware *list_load(const char *path, char *error, size_t size) {
    FILE *file = fopen(path, "r");
    if (!file) {
        fail(error, size, "there's no patch list for this firmware");
        return 0;
    }
    Owned *list = calloc(1, sizeof *list);
    list->text = malloc(MAX_BYTES + 1);
    list->patches = calloc(MAX_PATCHES, sizeof *list->patches);
    list->edits = calloc(MAX_EDITS, sizeof *list->edits);
    size_t length = fread(list->text, 1, MAX_BYTES + 1, file);
    fclose(file);
    list->text[length < MAX_BYTES ? length : MAX_BYTES] = 0;
    if (length > MAX_BYTES ? fail(error, size, "the patch list is too big") : !parse(list, error, size)) {
        list_free(&list->firmware);
        return 0;
    }
    return &list->firmware;
}

void list_free(Firmware *firmware) {
    if (!firmware) return;
    Owned *list = (Owned *)firmware;
    free(list->text);
    free(list->patches);
    free(list->edits);
    free(list);
}
