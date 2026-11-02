/* "Kobo Patcher" in Settings: every patch for this firmware, switched on and off straight away in
 * Nickel's memory. The firmware's files aren't changed. Choices are saved on the Kobo and made
 * again each time Nickel starts, before it has built anything. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../kit/nk.h"
#include "live.h"

#define CHOICES "/mnt/onboard/.adds/patches/choices.txt"
#define CHOICES_TEMP CHOICES ".tmp"
#define MAX_PATCHES 64
#define GUARD_SECONDS 20

enum { NO_CHOICE = -1 };

static const Firmware *firmware;
static int ready;
static int choices[MAX_PATCHES];
static struct {
    int state;
    NkToggle *toggle;
} rows[MAX_PATCHES];
static int updating;

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

/* ---- saved choices: one "id on" or "id off" line for each patch switched here ---- */

static void read_choices(void) {
    for (int i = 0; i < MAX_PATCHES; i++) choices[i] = NO_CHOICE;
    FILE *file = fopen(CHOICES, "r");
    if (!file) return;
    char id[64], value[8];
    while (fscanf(file, "%63s %7s", id, value) == 2)
        for (int i = 0; i < firmware->patch_count; i++)
            if (!strcmp(firmware->patches[i].id, id)) choices[i] = !strcmp(value, "on");
    fclose(file);
}

static void save_choices(void) {
    mkdir("/mnt/onboard/.adds", 0755);
    mkdir("/mnt/onboard/.adds/patches", 0755);
    FILE *file = fopen(CHOICES_TEMP, "w");
    if (!file) return nk_log("couldn't save choices");
    for (int i = 0; i < firmware->patch_count; i++)
        if (choices[i] != NO_CHOICE) fprintf(file, "%s %s\n", firmware->patches[i].id, choices[i] ? "on" : "off");
    fflush(file);
    fsync(fileno(file));
    fclose(file);
    rename(CHOICES_TEMP, CHOICES);
}

/* Patches turned off go first, because patches in a group can share bytes. */
static void apply_choices(void) {
    int restart = 0;
    for (int on = 0; on < 2; on++)
        for (int i = 0; i < firmware->patch_count; i++)
            if (choices[i] == on && live_state(&firmware->patches[i]) != (on ? ON : OFF))
                live_set(&firmware->patches[i], on, &restart);
}

/* ---- the screen ---- */

static int same_group(const Patch *a, const Patch *b) { return a != b && a->group && b->group && !strcmp(a->group, b->group); }

/* One patch in a group being on can make the others read as neither, when really they're off. */
static void read_states(void) {
    for (int i = 0; i < firmware->patch_count; i++) rows[i].state = live_state(&firmware->patches[i]);
    for (int i = 0; i < firmware->patch_count; i++)
        for (int k = 0; k < firmware->patch_count; k++)
            if (rows[i].state == UNKNOWN && rows[k].state == ON && same_group(&firmware->patches[i], &firmware->patches[k])) rows[i].state = OFF;
}

static void set_quietly(int i, int on) {
    updating++;
    nk_toggle_set(rows[i].toggle, on);
    updating--;
}

static void restart_now(void *unused) { nk_reboot(); }

static void change(int i, int on) {
    const Patch *patch = &firmware->patches[i];
    int restart = 0;
    nk_failsafe(GUARD_SECONDS);
    if (on)
        for (int k = 0; k < firmware->patch_count; k++)
            if (rows[k].state == ON && same_group(patch, &firmware->patches[k]) && live_set(&firmware->patches[k], 0, &restart)) {
                rows[k].state = OFF;
                choices[k] = 0;
                if (rows[k].toggle) set_quietly(k, 0);
            }
    if (!live_set(patch, on, &restart)) {
        set_quietly(i, !on);
        nk_error("Nothing was changed", "This patch's bytes have changed since this page opened. Close it and open it again.");
        return;
    }
    rows[i].state = on ? ON : OFF;
    choices[i] = on;
    save_choices();
    live_refresh();
    if (restart)
        nk_confirm("Restart to finish", "Your Kobo has already used what this patch changes, so it takes effect after a restart.",
                   "Restart", "Later", restart_now, 0, 0);
}

static void warning_accepted(void *row) { change((int)(long)row, 1); }
static void warning_declined(void *row) { set_quietly((int)(long)row, 0); }

static void toggled(void *row) {
    int i = (int)(long)row;
    if (updating) return;
    int on = nk_toggle_on(rows[i].toggle);
    if (on && firmware->patches[i].warning) {
        nk_confirm(firmware->patches[i].title, firmware->patches[i].warning, "Turn on", "Cancel", warning_accepted, warning_declined, row);
        return;
    }
    change(i, on);
}

static void build(NkScreen *screen, void *unused) {
    char version[32] = "unknown";
    const char *name = "Unknown model";
    this_firmware(version, sizeof version, &name);
    memset(rows, 0, sizeof rows);
    nk_section(screen, "THIS KOBO");
    nk_value_row(screen, name, version);
    if (!firmware) {
        nk_text(screen, "There are no patches for this firmware yet.");
        return;
    }
    if (!ready) {
        nk_text(screen, "This firmware is missing something these patches need, so they're left off.");
        return;
    }
    nk_text(screen, "Changes take effect straight away, and stay after a restart. These patches aren't made by Kobo, so use them at your own risk.");
    read_states();
    updating++;
    /* Each category once, in the order it first comes up, with all of its patches. */
    for (int first = 0; first < firmware->patch_count; first++) {
        const char *category = firmware->patches[first].category;
        int seen = 0;
        for (int k = 0; k < first; k++) seen = seen || !strcmp(firmware->patches[k].category, category);
        if (seen) continue;
        nk_section(screen, category);
        for (int i = first; i < firmware->patch_count; i++) {
            const Patch *patch = &firmware->patches[i];
            if (strcmp(patch->category, category)) continue;
            /* Bytes that match neither side belong to something else, so they're left alone. */
            if (rows[i].state == UNKNOWN) nk_value_row(screen, patch->title, "Changed");
            else rows[i].toggle = nk_toggle_row(screen, patch->title, rows[i].state == ON, toggled, (void *)(long)i);
        }
    }
    updating--;
}

static void open_patches(void *unused) { nk_open_screen("Kobo Patcher", "Settings", build, 0); }

void nk_mod_init(void) {
    char version[32] = "unknown";
    const char *name = "unknown model";
    firmware = this_firmware(version, sizeof version, &name);
    if (!firmware) nk_log("no patches for %s on %s", name, version);
    if (firmware && firmware->patch_count <= MAX_PATCHES && live_init(firmware)) {
        ready = 1;
        read_choices();
        apply_choices();
    }
    nk_settings_row("Kobo Patcher", open_patches, 0);
}
