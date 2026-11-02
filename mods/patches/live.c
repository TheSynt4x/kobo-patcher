/* See live.h. A patch is a list of edits to libnickel or the nickel program, both mapped into
 * Nickel's memory, so each edit's file offset is found in a loaded segment and written there.
 * Most patches change code Nickel runs again each time it's needed. The rest need a nudge, and
 * live_refresh and the fixups below give it, the way Nickel itself would. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "../kit/nk.h"
#include "live.h"

#define CALL(type, name) ((type)nk_sym(name))

/* ---- finding and changing bytes ---- */

typedef struct {
    const char *file;
    unsigned offset;
    unsigned char *address;
    int prot;
} Lookup;

static int is_nickel(const char *name) {
    size_t length = strlen(name);
    return !*name || (length >= 7 && !strcmp(name + length - 7, "/nickel"));
}

static int find_segment(struct dl_phdr_info *info, size_t size, void *data) {
    Lookup *l = data;
    if (strstr(l->file, "libnickel.so") ? !strstr(info->dlpi_name, "libnickel.so") : !is_nickel(info->dlpi_name)) return 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_LOAD || l->offset < ph->p_offset || l->offset >= ph->p_offset + ph->p_filesz) continue;
        l->address = (unsigned char *)(info->dlpi_addr + ph->p_vaddr + (l->offset - ph->p_offset));
        l->prot = (ph->p_flags & PF_R ? PROT_READ : 0) | (ph->p_flags & PF_W ? PROT_WRITE : 0) | (ph->p_flags & PF_X ? PROT_EXEC : 0);
    }
    return 1;
}

static unsigned char *address_of(const Edit *edit, int *prot) {
    Lookup l = {edit->file, edit->at, 0, 0};
    dl_iterate_phdr(find_segment, &l);
    if (prot) *prot = l.prot;
    return l.address;
}

static size_t edit_length(const Edit *edit) { return strlen(edit->find) / 2; }

static void unhex(const char *hex, unsigned char *out) {
    for (size_t i = 0; hex[i * 2]; i++) sscanf(hex + i * 2, "%2hhx", &out[i]);
}

static int matches(const unsigned char *at, const char *hex) {
    size_t length = strlen(hex) / 2;
    unsigned char want[length];
    unhex(hex, want);
    return !memcmp(at, want, length);
}

int live_state(const Patch *patch) {
    int on = 1, off = 1;
    for (int i = 0; i < patch->edit_count; i++) {
        const unsigned char *at = address_of(&patch->edits[i], 0);
        if (!at) return UNKNOWN;
        on = on && matches(at, patch->edits[i].replace);
        off = off && matches(at, patch->edits[i].find);
    }
    return on ? ON : off ? OFF : UNKNOWN;
}

static int write_bytes(unsigned char *at, const unsigned char *bytes, size_t length, int prot) {
    uintptr_t start = (uintptr_t)at & ~(uintptr_t)0xfff, end = ((uintptr_t)at + length + 0xfff) & ~(uintptr_t)0xfff;
    if (mprotect((void *)start, end - start, prot | PROT_WRITE)) return 0;
    memcpy(at, bytes, length);
    __builtin___clear_cache((char *)at, (char *)at + length);
    mprotect((void *)start, end - start, prot);
    return 1;
}

/* ---- Qt values ---- */

/* QString, QByteArray and QList share one header: ref, size, alloc, offset. A ref of -1 is static. */
static void release(void *data, int element_size) {
    int *ref = data;
    if (*ref == -1 || __atomic_sub_fetch(ref, 1, __ATOMIC_SEQ_CST)) return;
    if (element_size) CALL(void (*)(void *, unsigned, unsigned), "_ZN10QArrayData10deallocateEPS_jj")(data, element_size, 4);
    else CALL(void (*)(void *), "_ZN9QListData7disposeEPNS_4DataE")(data);
}

static const char *class_name(void *object) {
    void **vtable = *(void ***)object;
    const void *meta = ((const void *(*)(void *))vtable[0])(object);
    return CALL(const char *(*)(const void *), "_ZNK11QMetaObject9classNameEv")(meta);
}

/* Every widget, as a QList's data: {ref, alloc, begin, end, items...}. Release it with release(d, 0). */
static int *all_widgets(void) {
    int *list;
    CALL(void (*)(int **), "_ZN12QApplication10allWidgetsEv")(&list);
    return list;
}

#define EACH_WIDGET(list, widget) for (int _i = (list)[2]; _i < (list)[3] && ((widget) = ((void **)((list) + 4))[_i], 1); _i++)

/* ---- stylesheets ---- */

/* A stylesheet edit replaces a Qt resource's compressed bytes. They follow a 4-byte length, and
 * unpacking them the way Qt does gives exactly the text that setStyleSheetFile gave each widget. */
static int is_stylesheet(const Edit *edit) { return is_nickel(edit->file) && !strncmp(edit->find, "789c", 4); }

typedef struct {
    void *bytes, *text; /* a QByteArray's data, and a QString's */
} Sheet;

static Sheet unpack(const unsigned char *zlib) {
    Sheet sheet;
    int length = zlib[-8] << 24 | zlib[-7] << 16 | zlib[-6] << 8 | zlib[-5];
    CALL(void (*)(void **, const unsigned char *, int), "_Z11qUncompressPKhi")(&sheet.bytes, zlib - 4, length);
    const char *raw = (const char *)sheet.bytes + ((int *)sheet.bytes)[3];
    CALL(void (*)(void **, const char *, int), "_ZN7QString15fromUtf8_helperEPKci")(&sheet.text, raw, strlen(raw));
    return sheet;
}

static const char *raw_text(Sheet sheet) { return (const char *)sheet.bytes + ((int *)sheet.bytes)[3]; }

static void free_sheet(Sheet sheet) {
    release(sheet.bytes, 1);
    release(sheet.text, 2);
}

/* A stylesheet can hide a widget with "#name { qproperty-visible: false; }", but Qt doesn't show it
 * again when that rule goes. */
static int hides(const char *sheet, const char *name) {
    char rule[96];
    snprintf(rule, sizeof rule, "#%s {\n  qproperty-visible: false;", name);
    return strstr(sheet, rule) != 0;
}

/* Kobo's Qt adds QWidget::setStyleSheetFile, which reads the file each time with no cache and
 * doesn't keep its name. So the widgets showing the old stylesheet are found by its text. */
static void restyle(Sheet before, Sheet after) {
    int (*equal)(void **, void **) = CALL(int (*)(void **, void **), "_ZeqRK7QStringS1_");
    int *list = all_widgets(), count = 0;
    void *widget;
    EACH_WIDGET(list, widget) {
        void *current, *name;
        CALL(void (*)(void **, void *), "_ZNK7QWidget10styleSheetEv")(&current, widget);
        if (equal(&current, &before.text)) {
            CALL(void (*)(void *, void **), "_ZN7QWidget13setStyleSheetERK7QString")(widget, &after.text);
            count++;
        }
        release(current, 2);
        CALL(void (*)(void **, void *), "_ZNK7QObject10objectNameEv")(&name, widget);
        int *header = name;
        char ascii[64];
        int n = 0;
        for (; n < header[1] && n < 63; n++) ascii[n] = (char)((unsigned short *)((char *)name + header[3]))[n];
        ascii[n] = 0;
        if (n && hides(raw_text(before), ascii) && !hides(raw_text(after), ascii)) CALL(void (*)(void *), "_ZN7QWidget4showEv")(widget);
        release(name, 2);
    }
    release(list, 0);
    nk_log("restyled %d widgets", count);
}

/* ---- fixups that need one firmware's addresses ---- */

/* Each is checked against the running libnickel before it's used: the function's offset has to
 * match, so another build of the same version can't be mistaken for this one. */
static const struct {
    const char *version;
    /* ReadingSettings::lineHeightScalars keeps its list in a function-local static: the guard, then
     * the QList. */
    unsigned scalars, scalars_guard, scalars_list;
    /* configureTopRight only ever hides HomePageView's top-right QStackedWidget, which sits here. */
    unsigned home_top_right;
} known[] = {
    {"4.38.23852", 0xa1a409, 0x16f977c, 0x16f9780, 0x38},
};

static const char *firmware_version;
static char *libnickel_base;

static int find_libnickel(struct dl_phdr_info *info, size_t size, void *data) {
    if (!strstr(info->dlpi_name, "libnickel.so")) return 0;
    libnickel_base = (char *)info->dlpi_addr;
    return 1;
}

static int fixups(void) {
    for (size_t i = 0; i < sizeof known / sizeof *known; i++) {
        if (strcmp(known[i].version, firmware_version)) continue;
        char *scalars = dlsym(RTLD_DEFAULT, "_ZNK15ReadingSettings17lineHeightScalarsEv");
        if (scalars && libnickel_base && scalars - libnickel_base == (long)known[i].scalars) return (int)i;
    }
    return -1;
}

/* Whether any of the patch's edits fall inside a function. */
static int edits_function(const Patch *patch, const char *symbol) {
    void *start = dlsym(RTLD_DEFAULT, symbol);
    Dl_info info;
    const ElfW(Sym) *entry = 0;
    if (!start || !dladdr1(start, &info, (void **)&entry, RTLD_DL_SYMENT) || !entry) return 0;
    uintptr_t from = (uintptr_t)start & ~(uintptr_t)1, to = from + entry->st_size;
    for (int i = 0; i < patch->edit_count; i++) {
        uintptr_t at = (uintptr_t)address_of(&patch->edits[i], 0);
        if (at >= from && at < to) return 1;
    }
    return 0;
}

/* Clears the cached list so the next call builds it from the changed code. Every rebuild registers
 * one more destructor to run at exit, so the new list gets an extra reference for each rebuild so
 * far, and is freed by the last of them. The old list is released, and kept if it has extra
 * references from an earlier rebuild. Returns 0 if a restart is needed instead. */
static int rebuild_line_spacings(void) {
    int known_at = fixups();
    if (known_at < 0) return 0;
    int *guard = (int *)(libnickel_base + known[known_at].scalars_guard);
    int **list = (int **)(libnickel_base + known[known_at].scalars_list);
    if (!(*guard & 1)) return 1; /* not built yet, so it'll be built from the new code */
    void (*scalars)(int **, void *) = nk_sym("_ZNK15ReadingSettings17lineHeightScalarsEv");
    void (*destroy)(int **) = nk_sym("_ZN5QListIdED1Ev"); /* QList<double>, which also frees the doubles */
    static int rebuilds;
    if (**list < 1 || !destroy) return 0;
    destroy(list);
    rebuilds++;
    *guard = 0;
    int *copy;
    scalars(&copy, 0);
    __atomic_add_fetch(*list, rebuilds, __ATOMIC_SEQ_CST);
    destroy(&copy);
    nk_log("rebuilt the line spacing list");
    return 1;
}

/* ---- applying ---- */

int live_set(const Patch *patch, int on, int *restart) {
    for (int i = 0; i < patch->edit_count; i++) {
        const unsigned char *at = address_of(&patch->edits[i], 0);
        if (!at || !matches(at, on ? patch->edits[i].find : patch->edits[i].replace)) {
            nk_log("%s: bytes aren't as expected, not changing it", patch->id);
            return 0;
        }
    }
    Sheet before[patch->edit_count];
    for (int i = 0; i < patch->edit_count; i++)
        if (is_stylesheet(&patch->edits[i])) before[i] = unpack(address_of(&patch->edits[i], 0));
    for (int i = 0; i < patch->edit_count; i++) {
        const Edit *edit = &patch->edits[i];
        int prot;
        unsigned char *at = address_of(edit, &prot);
        unsigned char bytes[edit_length(edit)];
        unhex(on ? edit->replace : edit->find, bytes);
        if (!write_bytes(at, bytes, sizeof bytes, prot)) nk_log("%s: couldn't write an edit", patch->id);
    }
    for (int i = 0; i < patch->edit_count; i++) {
        if (!is_stylesheet(&patch->edits[i])) continue;
        Sheet after = unpack(address_of(&patch->edits[i], 0));
        restyle(before[i], after);
        free_sheet(before[i]);
        free_sheet(after);
    }
    if (patch->restart) *restart = 1;
    if (edits_function(patch, "_ZNK15ReadingSettings17lineHeightScalarsEv") && !rebuild_line_spacings()) *restart = 1;
    if (!on && edits_function(patch, "_ZN12HomePageView17configureTopRightE15HomePageWidgets") && fixups() < 0) *restart = 1;
    nk_log("%s: %s", patch->id, live_state(patch) == ON ? "on" : "off");
    return 1;
}

/* ---- telling Nickel to look again ---- */

static void *home_controller;
static void *(*real_home_controller)(void *, int, void *);

static void home_controller_gone(void *controller) {
    if (home_controller == controller) home_controller = 0;
}

/* MainWindowController can delete the home page's controller and make a new one, so the current
 * one is kept, and forgotten when Qt says it's destroyed. */
static void *hooked_home_controller(void *controller, int flag, void *parent) {
    void *result = real_home_controller(controller, flag, parent);
    home_controller = controller;
    nk_connect(controller, "_ZN7QObject9destroyedEPS_", "_ZN7QObject16staticMetaObjectE", home_controller_gone, controller);
    return result;
}

void live_refresh(void) {
    int known_at = fixups();
    int *list = all_widgets();
    void *widget;
    EACH_WIDGET(list, widget) {
        const char *name = class_name(widget);
        if (!strcmp(name, "MainNavView")) CALL(void (*)(void *), "_ZN11MainNavView20updateVisibleButtonsEv")(widget);
        else if (!strcmp(name, "MoreView")) CALL(void (*)(void *), "_ZN8MoreView20updateVisibleButtonsEv")(widget);
        else if (!strcmp(name, "HomePageView") && known_at >= 0) {
            /* Shown first so that configureWidgets decides afresh whether it's wanted. */
            void *column = ((void **)widget)[known[known_at].home_top_right / 4];
            if (column && !strcmp(class_name(column), "QStackedWidget")) CALL(void (*)(void *), "_ZN7QWidget4showEv")(column);
        }
    }
    release(list, 0);
    if (home_controller) CALL(void (*)(void *), "_ZN18HomePageController16configureWidgetsEv")(home_controller);
}

int live_init(const char *version) {
    static const char *const symbols[] = {
        "_ZN10QArrayData10deallocateEPS_jj", "_ZN9QListData7disposeEPNS_4DataE", "_ZNK11QMetaObject9classNameEv",
        "_ZN12QApplication10allWidgetsEv", "_Z11qUncompressPKhi", "_ZN7QString15fromUtf8_helperEPKci", "_ZeqRK7QStringS1_",
        "_ZNK7QWidget10styleSheetEv", "_ZN7QWidget13setStyleSheetERK7QString", "_ZNK7QObject10objectNameEv", "_ZN7QWidget4showEv",
        "_ZN11MainNavView20updateVisibleButtonsEv", "_ZN8MoreView20updateVisibleButtonsEv",
        "_ZN18HomePageController16configureWidgetsEv", "_ZN7QObject9destroyedEPS_", "_ZN7QObject16staticMetaObjectE",
    };
    int found = 1;
    for (size_t i = 0; i < sizeof symbols / sizeof *symbols; i++)
        if (!nk_sym(symbols[i])) found = 0;
    if (!found) return 0;
    firmware_version = version;
    dl_iterate_phdr(find_libnickel, 0);
    real_home_controller = nk_hook("_ZN18HomePageControllerC1EbP7QObject", hooked_home_controller);
    if (fixups() < 0) nk_log("no fixups for this firmware: some patches will need a restart");
    return real_home_controller != 0;
}
