/* "Patches" on the More page: every patch for this firmware and whether it's on, worked out from
 * the bytes in the firmware's files. Tick what you want and Apply writes a KoboRoot.tgz with those
 * files changed, then restarts, and Kobo's own startup script installs it. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include "../kit/nk.h"
#include "koboroot.h"
#include "patches_data.h"

#define PACKAGE "/mnt/onboard/.kobo/KoboRoot.tgz"
#define PACKAGE_TEMP "/mnt/onboard/.kobo/KoboRoot.tgz.tmp"
#define MAX_PATCHES 64
#define MAX_FILES 8

enum { OFF, ON, UNKNOWN };

static const Firmware *firmware;
static struct {
    int state;
    NkToggle *toggle;
} rows[MAX_PATCHES];
static NkLabel *summary;
static int updating;

static int hex_matches(int fd, unsigned at, const char *hex) {
    size_t length = strlen(hex) / 2;
    unsigned char actual[length];
    if (pread(fd, actual, length, at) != (ssize_t)length) return 0;
    for (size_t i = 0; i < length; i++) {
        unsigned byte;
        sscanf(hex + i * 2, "%2x", &byte);
        if (actual[i] != byte) return 0;
    }
    return 1;
}

static int patch_state(const Patch *patch) {
    int on = 1, off = 1;
    for (int i = 0; i < patch->edit_count; i++) {
        const Edit *edit = &patch->edits[i];
        int fd = open(edit->file, O_RDONLY);
        if (fd < 0) return UNKNOWN;
        on = on && hex_matches(fd, edit->at, edit->replace);
        off = off && hex_matches(fd, edit->at, edit->find);
        close(fd);
    }
    return on ? ON : off ? OFF : UNKNOWN;
}

static int same_group(const Patch *a, const Patch *b) { return a != b && a->group && b->group && !strcmp(a->group, b->group); }

/* Patches in a group can edit the same bytes, so one being on makes the others read as unknown
 * when really they're just off. */
static void read_states(void) {
    for (int i = 0; i < firmware->patch_count; i++) rows[i].state = patch_state(&firmware->patches[i]);
    for (int i = 0; i < firmware->patch_count; i++)
        for (int k = 0; k < firmware->patch_count; k++)
            if (rows[i].state == UNKNOWN && rows[k].state == ON && same_group(&firmware->patches[i], &firmware->patches[k])) rows[i].state = OFF;
}

/* .kobo/version is comma-separated: serial, kernel, firmware, ... The serial starts with the model.
 * The model's name is found even when its firmware isn't, so a Kobo after an update is still named. */
static const Firmware *this_firmware(char *version, size_t size, const char **name) {
    char line[256] = "";
    FILE *file = fopen("/mnt/onboard/.kobo/version", "r");
    if (!file) return 0;
    fgets(line, sizeof line, file);
    fclose(file);
    char *kernel = strchr(line, ','), *field = kernel ? strchr(kernel + 1, ',') : 0;
    if (!field) return 0;
    char *end = strchr(++field, ',');
    if (end) *end = 0;
    snprintf(version, size, "%s", field);
    for (size_t i = 0; i < sizeof firmwares / sizeof *firmwares; i++) {
        if (strncmp(line, firmwares[i].model, 4)) continue;
        *name = firmwares[i].name;
        if (!strcmp(field, firmwares[i].version)) return &firmwares[i];
    }
    return 0;
}

static int wants_change(int i) {
    return rows[i].toggle && nk_toggle_on(rows[i].toggle) != (rows[i].state == ON);
}

static int change_count(void) {
    int count = 0;
    for (int i = 0; i < firmware->patch_count; i++) count += wants_change(i);
    return count;
}

static void show_summary(void) {
    char text[64];
    int count = change_count();
    if (count) snprintf(text, sizeof text, count == 1 ? "%d change to apply" : "%d changes to apply", count);
    else snprintf(text, sizeof text, "No changes to apply");
    nk_label_set(summary, text);
}

static void set_quietly(int i, int on) {
    updating++;
    nk_toggle_set(rows[i].toggle, on);
    updating--;
}

static void warning_declined(void *row) { set_quietly((int)(long)row, 0); show_summary(); }

static void toggled(void *row) {
    int i = (int)(long)row;
    if (updating) return;
    const Patch *patch = &firmware->patches[i];
    if (nk_toggle_on(rows[i].toggle)) {
        for (int k = 0; k < firmware->patch_count; k++)
            if (rows[k].toggle && same_group(patch, &firmware->patches[k])) set_quietly(k, 0);
        if (patch->warning && rows[i].state != ON)
            nk_confirm(patch->title, patch->warning, "Turn on", "Cancel", 0, warning_declined, row);
    }
    show_summary();
}

/* Every file touched, with what has to change in it. Patches being turned off are listed first, so
 * that one turned on later wins if they share bytes. */
static int write_package(void) {
    const char *files[MAX_FILES];
    int file_count = 0;
    for (int i = 0; i < firmware->patch_count; i++)
        if (wants_change(i))
            for (int e = 0; e < firmware->patches[i].edit_count; e++) {
                const char *file = firmware->patches[i].edits[e].file;
                int known = 0;
                for (int f = 0; f < file_count; f++) known = known || !strcmp(files[f], file);
                if (!known && file_count < MAX_FILES) files[file_count++] = file;
            }
    KoboRoot *root = koboroot_open(PACKAGE_TEMP);
    if (!root) return 0;
    int ok = 1;
    for (int f = 0; f < file_count; f++) {
        Overlay overlays[256];
        int count = 0;
        for (int turning_on = 0; turning_on < 2; turning_on++)
            for (int i = 0; i < firmware->patch_count; i++) {
                if (!wants_change(i) || (rows[i].state == OFF) != turning_on) continue;
                const Patch *patch = &firmware->patches[i];
                for (int e = 0; e < patch->edit_count && count < 256; e++)
                    if (!strcmp(patch->edits[e].file, files[f]))
                        overlays[count++] = (Overlay){patch->edits[e].at, strlen(patch->edits[e].find) / 2,
                                                      turning_on ? patch->edits[e].replace : patch->edits[e].find};
            }
        ok = koboroot_add(root, files[f], overlays, count) && ok;
    }
    ok = koboroot_close(root) && ok;
    if (ok && rename(PACKAGE_TEMP, PACKAGE)) ok = 0;
    if (!ok) unlink(PACKAGE_TEMP);
    sync();
    return ok;
}

static unsigned long long bytes_needed(void) {
    unsigned long long total = 1 << 20;
    const char *counted[MAX_FILES];
    int count = 0;
    for (int i = 0; i < firmware->patch_count; i++)
        if (wants_change(i))
            for (int e = 0; e < firmware->patches[i].edit_count; e++) {
                const char *file = firmware->patches[i].edits[e].file;
                int known = 0;
                for (int f = 0; f < count; f++) known = known || !strcmp(counted[f], file);
                struct stat info;
                if (known || count >= MAX_FILES || stat(file, &info)) continue;
                counted[count++] = file;
                total += info.st_size + info.st_size / 1000;
            }
    return total;
}

static void apply_confirmed(void *unused) {
    /* Check the files again: they're what the package is built from. */
    int was[MAX_PATCHES];
    for (int i = 0; i < firmware->patch_count; i++) was[i] = rows[i].state;
    read_states();
    for (int i = 0; i < firmware->patch_count; i++)
        if (was[i] != rows[i].state) {
            nk_error("Nothing was changed", "This Kobo's files changed after this page opened. Close it and try again.");
            return;
        }
    if (!write_package()) {
        nk_error("Nothing was changed", "The update file couldn't be written. Check there's free space on your Kobo and try again.");
        return;
    }
    nk_log("package written, restarting");
    nk_reboot();
}

static void apply_tapped(void *unused) {
    if (!change_count()) return;
    if (access(PACKAGE, F_OK) == 0) {
        nk_error("An update is waiting", "Restart your Kobo to install it, then come back here.");
        return;
    }
    struct statvfs disk;
    if (statvfs("/mnt/onboard", &disk) || (unsigned long long)disk.f_bavail * disk.f_frsize < bytes_needed()) {
        nk_error("Not enough free space", "Applying patches needs room for a temporary copy of the files they change. Remove a few books and try again.");
        return;
    }
    nk_confirm("Apply and restart?",
               "Your Kobo will restart and install the changes. Keep it switched on until your home screen is back. "
               "These patches aren't made by Kobo, so use them at your own risk.",
               "Restart", "Cancel", apply_confirmed, 0, 0);
}

static void build(NkScreen *screen, void *unused) {
    char version[32] = "unknown";
    const char *name = "Unknown model";
    firmware = this_firmware(version, sizeof version, &name);
    memset(rows, 0, sizeof rows);
    nk_section(screen, "THIS KOBO");
    nk_value_row(screen, name, version);
    if (!firmware || firmware->patch_count > MAX_PATCHES) {
        nk_text(screen, "There are no patches for this firmware yet.");
        return;
    }
    summary = nk_button_row(screen, "", "Apply", apply_tapped, 0);
    read_states();
    const char *category = 0;
    updating++;
    for (int i = 0; i < firmware->patch_count; i++) {
        const Patch *patch = &firmware->patches[i];
        if (!category || strcmp(category, patch->category)) {
            category = patch->category;
            nk_section(screen, category);
        }
        /* Bytes that match neither side belong to something else, so they're left alone. */
        if (rows[i].state == UNKNOWN) nk_value_row(screen, patch->title, "Changed");
        else rows[i].toggle = nk_toggle_row(screen, patch->title, rows[i].state == ON, toggled, (void *)(long)i);
    }
    updating--;
    show_summary();
}

static void open_patches(void *unused) { nk_open_screen("Patches", "More", build, 0); }

static const unsigned char icon[] = {
#include "icon.inc"
};

void nk_mod_init(void) { nk_more_row("Patches", icon, sizeof icon, open_patches, 0); }
