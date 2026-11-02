/* "Kobo Patcher" in Settings: every patch for this firmware, switched on and off straight away in
 * Nickel's memory. The firmware's files aren't changed. The patch list is a file in .adds, so a new
 * one arrives without reinstalling the mod, and it's read again whenever it changes. Choices are
 * saved on the Kobo and made again each time Nickel starts, before it has built anything. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../kit/nk.h"
#include "live.h"
#include "update.h"

#define FOLDER "/mnt/onboard/.adds/patches"
#define CHOICES FOLDER "/choices.txt"
#define CHOICES_TEMP CHOICES ".tmp"
#define MAX_PATCHES 256
#define MAX_CHOICES 256
#define GUARD_SECONDS 20

static char model[8] = "", version[32] = "unknown";
static Firmware *firmware;
static char list_error[160];
static struct stat list_seen;
static int ready;
static struct {
    int state;
    NkToggle *toggle;
} rows[MAX_PATCHES];
static int updating;

/* .kobo/version is comma-separated: serial, kernel, firmware, ... The serial starts with the model. */
static void this_kobo(void) {
    char line[256] = "";
    FILE *file = fopen("/mnt/onboard/.kobo/version", "r");
    if (!file) return;
    fgets(line, sizeof line, file);
    fclose(file);
    char *kernel = strchr(line, ','), *field = kernel ? strchr(kernel + 1, ',') : 0;
    if (!field) return;
    char *end = strchr(++field, ',');
    if (end) *end = 0;
    snprintf(model, sizeof model, "%.4s", line);
    snprintf(version, sizeof version, "%s", field);
}

/* ---- saved choices: an "id on" or "id off" line for each patch switched here ---- */

/* Kept by id, apart from the list, so a new list that drops or reorders patches loses nothing. */
static struct {
    char id[64];
    int on;
} choices[MAX_CHOICES];
static int choice_count;

static void read_choices(void) {
    choice_count = 0;
    FILE *file = fopen(CHOICES, "r");
    if (!file) return;
    char value[8];
    while (choice_count < MAX_CHOICES && fscanf(file, "%63s %7s", choices[choice_count].id, value) == 2) {
        choices[choice_count].on = !strcmp(value, "on");
        choice_count++;
    }
    fclose(file);
}

/* 1 or 0 for a patch switched here, -1 for one that never was. */
static int choice(const Patch *patch) {
    for (int i = 0; i < choice_count; i++)
        if (!strcmp(choices[i].id, patch->id)) return choices[i].on;
    return -1;
}

static void choose(const Patch *patch, int on) {
    int i = 0;
    while (i < choice_count && strcmp(choices[i].id, patch->id)) i++;
    if (i == MAX_CHOICES) return;
    if (i == choice_count) snprintf(choices[choice_count++].id, sizeof choices[i].id, "%s", patch->id);
    choices[i].on = on;
}

static void save_choices(void) {
    mkdir("/mnt/onboard/.adds", 0755);
    mkdir(FOLDER, 0755);
    FILE *file = fopen(CHOICES_TEMP, "w");
    if (!file) return nk_log("couldn't save choices");
    for (int i = 0; i < choice_count; i++) fprintf(file, "%s %s\n", choices[i].id, choices[i].on ? "on" : "off");
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
            if (choice(&firmware->patches[i]) == on && live_state(&firmware->patches[i]) != (on ? ON : OFF))
                live_set(&firmware->patches[i], on, &restart);
}

/* ---- the patch list ---- */

static void list_path(char *path, size_t size) { snprintf(path, size, FOLDER "/lists/%s-%s.txt", model, version); }

/* Reads the list again if the file has changed since it was last read, and makes the saved
 * choices for any patches that are new to it. A list that can't be read leaves the last one. */
static void load_list(void) {
    char path[128];
    struct stat now;
    list_path(path, sizeof path);
    if (stat(path, &now)) {
        if (!firmware) snprintf(list_error, sizeof list_error, "there's no patch list for this firmware");
        return;
    }
    if (firmware && now.st_mtime == list_seen.st_mtime && now.st_size == list_seen.st_size) return;
    list_seen = now;
    Firmware *loaded = list_load(path, list_error, sizeof list_error);
    if (loaded && (strcmp(loaded->model, model) || strcmp(loaded->version, version) || loaded->patch_count > MAX_PATCHES)) {
        snprintf(list_error, sizeof list_error, "the patch list is for another Kobo or firmware");
        list_free(loaded);
        loaded = 0;
    }
    if (!loaded) return nk_log("patch list not read: %s", list_error);
    list_free(firmware);
    firmware = loaded;
    list_error[0] = 0;
    nk_log("read %d patches for %s %s", firmware->patch_count, model, version);
    if (ready) apply_choices();
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
                choose(&firmware->patches[k], 0);
                if (rows[k].toggle) set_quietly(k, 0);
            }
    if (!live_set(patch, on, &restart)) {
        set_quietly(i, !on);
        nk_error("Nothing was changed", "This patch's bytes have changed since this page opened. Close it and open it again.");
        return;
    }
    rows[i].state = on ? ON : OFF;
    choose(patch, on);
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

static NkLabel *update_status;

static void check_tapped(void *unused) { update_check(update_status, model, version, load_list); }

static void build(NkScreen *screen, void *unused) {
    load_list();
    memset(rows, 0, sizeof rows);
    nk_section(screen, "THIS KOBO");
    nk_value_row(screen, firmware ? firmware->name : *model ? model : "Unknown model", version);
    update_status = nk_button_row(screen, "New patches and Kobo Patcher", "Check", check_tapped, 0);
    if (!firmware) {
        char text[256];
        snprintf(text, sizeof text, "No patches yet: %s. Connect to Wi-Fi and tap Check, or add them from kobo.thesynt4x.dev.", list_error);
        nk_text(screen, text);
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
    this_kobo();
    read_choices();
    ready = live_init(version);
    load_list();
    nk_settings_row("Kobo Patcher", open_patches, 0);
}
