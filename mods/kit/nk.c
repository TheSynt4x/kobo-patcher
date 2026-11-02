/* See nk.h. Addresses and layouts here come from Qt 5.2's ABI and were checked on Nia firmware
 * 4.38.23852. Anything firmware-specific is verified at runtime before it's used. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "nk.h"

#ifndef NK_NAME
#define NK_NAME "mod"
#endif
#define INSTALLED_AT "/usr/local/Kobo/imageformats/lib" NK_NAME ".so"
#define UNINSTALL_FLAG "/mnt/onboard/.adds/" NK_NAME "/uninstall"
/* Outside imageformats, because Qt would still load libpatches.so.anything from in there. */
#define DISABLED_AT "/usr/local/Kobo/" NK_NAME ".disabled"
#define FAILSAFE_SECONDS 20

/* ---- plumbing ---- */

void nk_log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[" NK_NAME "] ");
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}

static int all_found = 1;

void *nk_sym(const char *name) {
    void *found = dlsym(RTLD_DEFAULT, name);
    if (!found) {
        nk_log("missing %s", name);
        all_found = 0;
    }
    return found;
}

int nk_ok(void) { return all_found; }

#define CALL(type, name) ((type)nk_sym(name))

static void *alloc(size_t size) {
    void *memory = CALL(void *(*)(size_t), "_Znwj")(size);
    memset(memory, 0, size);
    return memory;
}

/* QString and QVariant, the only Qt value types the kit passes around. */
typedef void *QStr;

static QStr qstr(const char *text) {
    QStr out;
    CALL(void (*)(QStr *, const char *, int), "_ZN7QString15fromUtf8_helperEPKci")(&out, text, strlen(text));
    return out;
}

static void set_name(void *object, const char *name) {
    QStr text = qstr(name);
    CALL(void (*)(void *, QStr *), "_ZN7QObject13setObjectNameERK7QString")(object, &text);
}

static void set_property(void *object, const char *name, void *variant) {
    CALL(int (*)(void *, const char *, void *), "_ZN7QObject11setPropertyEPKcRK8QVariant")(object, name, variant);
}

static void set_string_property(void *object, const char *name, const char *value) {
    char variant[16];
    QStr text = qstr(value);
    CALL(void (*)(void *, QStr *), "_ZN8QVariantC1ERK7QString")(variant, &text);
    set_property(object, name, variant);
}

static void set_pixmap_property(void *object, const char *name, const unsigned char *png, unsigned size) {
    char pixmap[32], variant[16];
    CALL(void (*)(void *), "_ZN7QPixmapC1Ev")(pixmap);
    CALL(int (*)(void *, const unsigned char *, unsigned, const char *, int), "_ZN7QPixmap12loadFromDataEPKhjPKc6QFlagsIN2Qt19ImageConversionFlagEE")(pixmap, png, size, "PNG", 0);
    CALL(void (*)(void *, void *), "_ZNK7QPixmapcv8QVariantEv")(variant, pixmap);
    set_property(object, name, variant);
}

static const char *class_name(void *object) {
    void **vtable = *(void ***)object;
    const void *meta = ((const void *(*)(void *))vtable[0])(object);
    return CALL(const char *(*)(const void *), "_ZNK11QMetaObject9classNameEv")(meta);
}

static void object_name(void *object, char *out, size_t size) {
    QStr name;
    CALL(void (*)(QStr *, void *), "_ZNK7QObject10objectNameEv")(&name, object);
    int *header = name;
    const unsigned short *chars = (const unsigned short *)((char *)name + header[3]);
    size_t i = 0;
    for (; i + 1 < size && i < (size_t)header[1]; i++) out[i] = (char)chars[i];
    out[i] = 0;
}

/* The children of a QObject: d_ptr at +4, then QObjectData's QObjectList at +0xc. */
static int children(void *object, void ***out) {
    int *list = ((void **)((void **)object)[1])[3];
    *out = (void **)(list + 4) + list[2];
    return list[3] - list[2];
}

static void *find_named(void *parent, const char *name) {
    void **kids;
    int count = children(parent, &kids);
    char found[64];
    for (int i = 0; i < count; i++) {
        object_name(kids[i], found, sizeof found);
        if (!strcmp(found, name)) return kids[i];
    }
    return 0;
}

static void *find_class(void *parent, const char *name) {
    void **kids;
    int count = children(parent, &kids);
    for (int i = 0; i < count; i++)
        if (!strcmp(class_name(kids[i]), name)) return kids[i];
    return 0;
}

/* Connect a Qt signal to a C function, the way Qt's functor connect does: a QSlotObjectBase is a
 * refcount and an impl function, which gets 0 to destroy, 1 to call, 2 to compare. */
typedef struct {
    int ref;
    void (*impl)(int, void *, void *, void **, char *);
    NkFn fn;
    void *arg;
} Slot;

static void slot_impl(int which, void *base, void *receiver, void **args, char *ret) {
    Slot *slot = base;
    if (which == 1) slot->fn(slot->arg);
    else if (which == 2 && ret) *ret = 0;
}

static void connect_to(void *sender, const char *signal, const char *meta, NkFn fn, void *arg) {
    void *signal_ptr[2] = {nk_sym(signal), 0}, *no_slot[2] = {0, 0}, *connection = 0;
    void *static_meta = nk_sym(meta);
    if (!signal_ptr[0] || !static_meta) return;
    Slot *slot = malloc(sizeof *slot);
    *slot = (Slot){1, slot_impl, fn, arg};
    CALL(void (*)(void **, void *, void **, void *, void **, void *, int, const int *, void *),
         "_ZN7QObject11connectImplEPKS_PPvS1_S3_PN9QtPrivate15QSlotObjectBaseEN2Qt14ConnectionTypeEPKiPK11QMetaObject")(
        &connection, sender, signal_ptr, sender, no_slot, slot, 0, 0, static_meta);
}

void nk_connect(void *sender, const char *signal, const char *meta, NkFn fn, void *arg) { connect_to(sender, signal, meta, fn, arg); }

/* ---- hooking ---- */

/* libnickel calls its own exported functions through its GOT, so changing an entry there
 * redirects the call without touching any code. */
typedef struct {
    const char *symbol;
    void *replacement, *original;
} Hook;

static int hook_library(struct dl_phdr_info *info, size_t size, void *data) {
    Hook *hook = data;
    if (!strstr(info->dlpi_name, "libnickel.so")) return 0;
    const ElfW(Phdr) *ph = info->dlpi_phdr;
    const ElfW(Dyn) *dyn = 0;
    for (int i = 0; i < info->dlpi_phnum; i++)
        if (ph[i].p_type == PT_DYNAMIC) dyn = (const void *)(info->dlpi_addr + ph[i].p_vaddr);
    if (!dyn) return 1;
    const ElfW(Sym) *symbols = 0;
    const char *strings = 0;
    const ElfW(Rel) *rel = 0;
    size_t rel_size = 0;
    for (; dyn->d_tag != DT_NULL; dyn++) {
        if (dyn->d_tag == DT_SYMTAB) symbols = (const void *)dyn->d_un.d_ptr;
        else if (dyn->d_tag == DT_STRTAB) strings = (const void *)dyn->d_un.d_ptr;
        else if (dyn->d_tag == DT_JMPREL) rel = (const void *)dyn->d_un.d_ptr;
        else if (dyn->d_tag == DT_PLTRELSZ) rel_size = dyn->d_un.d_val;
    }
    if (!symbols || !strings || !rel) return 1;
    for (size_t i = 0; i < rel_size / sizeof *rel; i++) {
        const ElfW(Sym) *sym = &symbols[ELF32_R_SYM(rel[i].r_info)];
        if (strcmp(strings + sym->st_name, hook->symbol)) continue;
        void **slot = (void **)(info->dlpi_addr + rel[i].r_offset);
        uintptr_t page = (uintptr_t)slot & ~(uintptr_t)0xfff;
        if (mprotect((void *)page, 0x1000, PROT_READ | PROT_WRITE)) return 1;
        hook->original = dlsym(RTLD_DEFAULT, hook->symbol);
        *slot = hook->replacement;
        return 1;
    }
    return 1;
}

void *nk_hook(const char *symbol, void *replacement) {
    Hook hook = {symbol, replacement, 0};
    dl_iterate_phdr(hook_library, &hook);
    if (!hook.original) {
        nk_log("can't hook %s", symbol);
        all_found = 0;
    }
    return hook.original;
}

static void guard_screen(void);
static void unguard_screen(void);

/* ---- the More row ---- */

static struct {
    const char *title;
    const unsigned char *icon;
    unsigned icon_size;
    NkFn open;
    void *arg;
} more_row;
static void *(*real_more_view)(void *, void *);

static void *italic_label_vptr[2];

static void *hooked_more_view(void *view, void *parent) {
    void *result = real_more_view(view, parent);
    void *container = find_class(view, "GenericContainerWidget");
    void *settings = container ? find_named(container, "settingsButton") : 0;
    if (!container || !settings) {
        nk_log("More page isn't what's expected, leaving it alone");
        return result;
    }
    void *layout = CALL(void *(*)(void *), "_ZNK7QWidget6layoutEv")(container);
    void *row = alloc(0x58);
    CALL(void (*)(void *, void *), "_ZN14IconLeftButtonC1EP7QWidget")(row, container);
    set_name(row, "nkMoreRow");
    set_pixmap_property(row, "pixmap", more_row.icon, more_row.icon_size);
    set_string_property(row, "text", more_row.title);
    int at = CALL(int (*)(void *, void *), "_ZNK7QLayout7indexOfEP7QWidget")(layout, settings);
    CALL(void (*)(void *, int, void *, int, int), "_ZN10QBoxLayout12insertWidgetEiP7QWidgeti6QFlagsIN2Qt13AlignmentFlagEE")(layout, at, row, 0, 0);
    connect_to(row, "_ZN14IconButtonBase6tappedEv", "_ZN14IconButtonBase16staticMetaObjectE", more_row.open, more_row.arg);

    /* Kobo's label classes have no exported constructors, so this one is learned from the row. */
    void *inner = find_class(row, "GenericContainerWidget");
    void *italic = inner ? find_class(inner, "RegularItalicLabel") : 0;
    if (italic) {
        italic_label_vptr[0] = ((void **)italic)[0];
        italic_label_vptr[1] = ((void **)italic)[2];
    }
    nk_log("row added");
    return result;
}

/* Every symbol this file uses, listed by build.sh. They're all checked before anything is touched,
 * so a firmware missing one gets no row at all rather than a crash when it's first called. */
static int all_symbols_present(void) {
    static const char *const symbols[] = {
#include "symbols.inc"
    };
    int found = 1;
    for (size_t i = 0; i < sizeof symbols / sizeof *symbols; i++)
        if (!nk_sym(symbols[i])) found = 0;
    return found;
}

void nk_more_row(const char *title, const unsigned char *icon_png, unsigned icon_size, NkFn open, void *arg) {
    if (!all_symbols_present()) {
        nk_log("this firmware is missing something, not adding the row");
        return;
    }
    more_row.title = title;
    more_row.icon = icon_png;
    more_row.icon_size = icon_size;
    more_row.open = open;
    more_row.arg = arg;
    real_more_view = nk_hook("_ZN8MoreViewC1EP7QWidget", hooked_more_view);
}

/* ---- the Settings row ---- */

static struct {
    const char *title;
    NkFn open;
    void *arg;
} settings_row;
static void *(*real_settings_view)(void *, void *);

static void *find_named_deep(void *parent, const char *name) {
    void **kids;
    int count = children(parent, &kids);
    char found[64];
    for (int i = 0; i < count; i++) {
        object_name(kids[i], found, sizeof found);
        if (!strcmp(found, name)) return kids[i];
        void *deeper = find_named_deep(kids[i], name);
        if (deeper) return deeper;
    }
    return 0;
}

static int inherits(void *object, const char *name) {
    void **vtable = *(void ***)object;
    return ((void *(*)(void *, const char *))vtable[1])(object, name) != 0; /* qt_metacast */
}

/* Where a widget is in a PagingContainer's QList<QPointer<QWidget>>, at +0x2c. Each item is a
 * QPointer: its refcount data, then the widget. -1 if it's not there or the list looks wrong. */
static int paging_index(void *paging, void *widget) {
    int *list = *(int **)((char *)paging + 0x2c);
    if (!list || list[0] < 1 || list[2] < 0 || list[3] < list[2] || list[3] > list[1] || list[3] - list[2] > 64) return -1;
    for (int i = list[2]; i < list[3]; i++) {
        void **pointer = ((void ***)(list + 4))[i];
        if (pointer && pointer[1] == widget) return i - list[2];
    }
    return -1;
}

/* Kobo's Settings rows are a TouchLabel subclass with no constructor of its own: Ui_N3SettingsView
 * makes a TouchLabel and sets these vtable pointers. They're copied from Device information. The
 * new row goes first, where it's easy to find, and Device information's place checks the list. */
static const int settings_row_vptrs[] = {0, 2, 8, 0x13, 0x16};

static void *hooked_settings_view(void *view, void *parent) {
    void *result = real_settings_view(view, parent);
    void *paging = CALL(void *(*)(void *), "_ZNK14N3SettingsView15pagingContainerEv")(view);
    void **model = paging ? find_named_deep(paging, "deviceInformation") : 0;
    if (!model || !inherits(model, "TouchLabel") || paging_index(paging, model) < 0) {
        nk_log("Settings isn't what's expected, leaving it alone");
        return result;
    }
    void **row = alloc(0x84);
    CALL(void (*)(void *, void *, int), "_ZN10TouchLabelC1EP7QWidget6QFlagsIN2Qt10WindowTypeEE")(row, paging, 0);
    for (size_t i = 0; i < sizeof settings_row_vptrs / sizeof *settings_row_vptrs; i++) row[settings_row_vptrs[i]] = model[settings_row_vptrs[i]];
    set_name(row, "nkSettingsRow");
    QStr text = qstr(settings_row.title);
    CALL(void (*)(void *, QStr *), "_ZN6QLabel7setTextERK7QString")(row, &text);
    CALL(void (*)(void *, int, void *), "_ZN15PagingContainer12insertWidgetEiP7QWidget")(paging, 0, row);
    connect_to(row, "_ZN10TouchLabel6tappedEb", "_ZN10TouchLabel16staticMetaObjectE", settings_row.open, settings_row.arg);
    nk_log("settings row added");
    return result;
}

void nk_settings_row(const char *title, NkFn open, void *arg) {
    if (!all_symbols_present()) {
        nk_log("this firmware is missing something, not adding the row");
        return;
    }
    settings_row.title = title;
    settings_row.open = open;
    settings_row.arg = arg;
    real_settings_view = nk_hook("_ZN14N3SettingsViewC1EP7QWidget", hooked_settings_view);
}

/* ---- screens ---- */

/* Screens reuse N3SettingsLoggingCategoryController and its view, a bare paged settings page
 * with nothing else on it. A copy of the controller's vtables replaces what fills the page. */
enum { PRIMARY_SLOTS = 16, GET_TITLE = 13, GET_SMALL_TITLE = 14, LOAD_VIEW = 15, ABSTRACT_SLOTS = 9, ABSTRACT_LOAD_VIEW = 8 };

struct NkScreen {
    void *paging;
};

static struct {
    const char *title, *small_title;
    void (*build)(NkScreen *, void *);
    void *arg;
    void *vptrs[4][2];
    int learned;
} screen;

static void learn_label(void *parent, const char *name, int which) {
    void **kids;
    int count = children(parent, &kids);
    for (int i = 0; i < count && !screen.vptrs[which][0]; i++) {
        void **object = kids[i];
        if (!strcmp(class_name(object), name)) {
            screen.vptrs[which][0] = object[0];
            screen.vptrs[which][1] = object[2];
        }
        learn_label(object, name, which);
    }
}

enum { LABEL_SECTION, LABEL_TEXT, LABEL_ROW };

/* Beta Features holds the heading and text labels, so one is built and thrown away to learn them. */
static void learn_labels(void) {
    if (screen.learned) return;
    screen.learned = 1;
    void *view = alloc(0x400);
    CALL(void (*)(void *, void *), "_ZN20N3SettingsExtrasViewC1EP7QWidget")(view, 0);
    learn_label(view, "MetaDataHeaderLabel", LABEL_SECTION);
    learn_label(view, "RegularLabel", LABEL_TEXT);
    learn_label(view, "RegularLabelVPadded", LABEL_ROW);
    CALL(void (*)(void *), "_ZN7QObject11deleteLaterEv")(view);
    /* The italic label, for values, is in Kobo's icon rows. */
    if (!italic_label_vptr[0]) {
        void *button = alloc(0x58);
        CALL(void (*)(void *, void *), "_ZN14IconLeftButtonC1EP7QWidget")(button, 0);
        void *inner = find_class(button, "GenericContainerWidget");
        void **italic = inner ? find_class(inner, "RegularItalicLabel") : 0;
        if (italic) {
            italic_label_vptr[0] = italic[0];
            italic_label_vptr[1] = italic[2];
        }
        CALL(void (*)(void *), "_ZN7QObject11deleteLaterEv")(button);
    }
}

static void *label(void *parent, void *vptrs[2], const char *text) {
    void **object = alloc(0x18);
    CALL(void (*)(void *, void *, int), "_ZN6QLabelC1EP7QWidget6QFlagsIN2Qt10WindowTypeEE")(object, parent, 0);
    if (vptrs[0]) {
        object[0] = vptrs[0];
        object[2] = vptrs[1];
    }
    QStr value = qstr(text);
    CALL(void (*)(void *, QStr *), "_ZN6QLabel7setTextERK7QString")(object, &value);
    return object;
}

static void add_to_page(NkScreen *s, void *widget) {
    CALL(void (*)(void *, void *), "_ZN15PagingContainer9addWidgetEP7QWidget")(s->paging, widget);
}

void nk_section(NkScreen *s, const char *title) {
    add_to_page(s, label(s->paging, screen.vptrs[LABEL_SECTION], title));
}

void nk_text(NkScreen *s, const char *text) {
    void *line = label(s->paging, screen.vptrs[LABEL_TEXT], text);
    CALL(void (*)(void *, int), "_ZN6QLabel11setWordWrapEb")(line, 1);
    add_to_page(s, line);
}

void nk_value_row(NkScreen *s, const char *title, const char *value) {
    void *row = alloc(0x6c);
    CALL(void (*)(void *, void *), "_ZN16SettingContainerC1EP7QWidget")(row, s->paging);
    void *box = alloc(0x10);
    CALL(void (*)(void *, void *), "_ZN11QHBoxLayoutC1EP7QWidget")(box, row);
    void (*add)(void *, void *, int, int) = CALL(void (*)(void *, void *, int, int), "_ZN10QBoxLayout9addWidgetEP7QWidgeti6QFlagsIN2Qt13AlignmentFlagEE");
    void *name = label(0, screen.vptrs[LABEL_ROW], title);
    CALL(void (*)(void *, int), "_ZN6QLabel11setWordWrapEb")(name, 1);
    add(box, name, 1, 0);
    void *right = label(0, italic_label_vptr, value);
    /* Lines the value up with the "On" and "Start" of Kobo's own settings rows. */
    QStr inset = qstr("padding-right: 17px;");
    CALL(void (*)(void *, QStr *), "_ZN7QWidget13setStyleSheetERK7QString")(right, &inset);
    add(box, right, 0, 0);
    add_to_page(s, row);
}

NkToggle *nk_toggle_row(NkScreen *s, const char *title, int on, NkFn changed, void *arg) {
    void *row = alloc(0x6c);
    CALL(void (*)(void *, void *), "_ZN16SettingContainerC1EP7QWidget")(row, s->paging);
    void *box = alloc(0x10);
    CALL(void (*)(void *, void *), "_ZN11QHBoxLayoutC1EP7QWidget")(box, row);
    void (*add)(void *, void *, int, int) = CALL(void (*)(void *, void *, int, int), "_ZN10QBoxLayout9addWidgetEP7QWidgeti6QFlagsIN2Qt13AlignmentFlagEE");
    void *name = label(0, screen.vptrs[LABEL_ROW], title);
    CALL(void (*)(void *, int), "_ZN6QLabel11setWordWrapEb")(name, 1);
    add(box, name, 1, 0);
    void *check = alloc(0x58);
    CALL(void (*)(void *, void *), "_ZN13TouchCheckBoxC1EP7QWidget")(check, 0);
    QStr text = qstr("On");
    CALL(void (*)(void *, QStr *), "_ZN15QAbstractButton7setTextERK7QString")(check, &text);
    nk_toggle_set((NkToggle *)check, on);
    connect_to(check, "_ZN9QCheckBox12stateChangedEi", "_ZN9QCheckBox16staticMetaObjectE", changed, arg);
    add(box, check, 0, 0);
    add_to_page(s, row);
    return (NkToggle *)check;
}

int nk_toggle_on(NkToggle *toggle) { return CALL(int (*)(void *), "_ZNK15QAbstractButton9isCheckedEv")(toggle); }

void nk_toggle_set(NkToggle *toggle, int on) { CALL(void (*)(void *, int), "_ZN15QAbstractButton10setCheckedEb")(toggle, on); }

NkLabel *nk_button_row(NkScreen *s, const char *title, const char *button, NkFn tapped, void *arg) {
    void *row = alloc(0x6c);
    CALL(void (*)(void *, void *), "_ZN16SettingContainerC1EP7QWidget")(row, s->paging);
    void *box = alloc(0x10);
    CALL(void (*)(void *, void *), "_ZN11QHBoxLayoutC1EP7QWidget")(box, row);
    void (*add)(void *, void *, int, int) = CALL(void (*)(void *, void *, int, int), "_ZN10QBoxLayout9addWidgetEP7QWidgeti6QFlagsIN2Qt13AlignmentFlagEE");
    void *name = label(0, screen.vptrs[LABEL_ROW], title);
    CALL(void (*)(void *, int), "_ZN6QLabel11setWordWrapEb")(name, 1);
    add(box, name, 1, 0);
    void *action = alloc(0x98);
    CALL(void (*)(void *, void *), "_ZN13N3ButtonLabelC1EP7QWidget")(action, 0);
    QStr text = qstr(button);
    CALL(void (*)(void *, QStr *), "_ZN6QLabel7setTextERK7QString")(action, &text);
    connect_to(action, "_ZN10TouchLabel6tappedEb", "_ZN10TouchLabel16staticMetaObjectE", tapped, arg);
    add(box, action, 0, 0);
    add_to_page(s, row);
    return (NkLabel *)name;
}

void nk_label_set(NkLabel *line, const char *text) {
    QStr value = qstr(text);
    CALL(void (*)(void *, QStr *), "_ZN6QLabel7setTextERK7QString")(line, &value);
}

void nk_confirm(const char *title, const char *text, const char *accept, const char *reject, NkFn accepted, NkFn rejected, void *arg) {
    void *dialog = CALL(void *(*)(void *), "_ZN25ConfirmationDialogFactory21getConfirmationDialogEP7QWidget")(0);
    CALL(void (*)(void *, int, int), "_ZN7QWidget12setAttributeEN2Qt15WidgetAttributeEb")(dialog, 0x37, 1); /* delete on close */
    QStr value = qstr(title);
    CALL(void (*)(void *, QStr *), "_ZN18ConfirmationDialog8setTitleERK7QString")(dialog, &value);
    value = qstr(text);
    CALL(void (*)(void *, QStr *), "_ZN18ConfirmationDialog7setTextERK7QString")(dialog, &value);
    value = qstr(accept);
    CALL(void (*)(void *, QStr *), "_ZN18ConfirmationDialog19setAcceptButtonTextERK7QString")(dialog, &value);
    value = qstr(reject);
    CALL(void (*)(void *, QStr *), "_ZN18ConfirmationDialog19setRejectButtonTextERK7QString")(dialog, &value);
    if (accepted) connect_to(dialog, "_ZN7QDialog8acceptedEv", "_ZN7QDialog16staticMetaObjectE", accepted, arg);
    if (rejected) connect_to(dialog, "_ZN7QDialog8rejectedEv", "_ZN7QDialog16staticMetaObjectE", rejected, arg);
    CALL(void (*)(void *), "_ZN7QDialog4openEv")(dialog);
}

void nk_error(const char *title, const char *text) {
    QStr heading = qstr(title), body = qstr(text);
    CALL(void *(*)(QStr *, QStr *), "_ZN25ConfirmationDialogFactory15showErrorDialogERK7QStringS2_")(&heading, &body);
}

static void failsafe_stop(void);

const char *nk_self_path(void) { return access(INSTALLED_AT, F_OK) == 0 ? INSTALLED_AT : DISABLED_AT; }

/* Written beside the old file and renamed over it, so a lost write leaves the old one. Nickel keeps
 * running the old copy it already loaded. */
int nk_replace_self(const void *data, unsigned size) {
    failsafe_stop();
    int fd = open(INSTALLED_AT ".new", O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) return 0;
    int ok = write(fd, data, size) == (ssize_t)size;
    ok = !fsync(fd) && ok;
    close(fd);
    if (!ok || rename(INSTALLED_AT ".new", INSTALLED_AT)) {
        unlink(INSTALLED_AT ".new");
        return 0;
    }
    unlink(DISABLED_AT);
    sync();
    nk_log("replaced with a new build");
    return 1;
}

void nk_reboot(void) {
    failsafe_stop();
    void *power = CALL(void *(*)(void), "_ZN22N3PowerWorkflowManager14sharedInstanceEv")();
    CALL(void (*)(void *), "_ZN22N3PowerWorkflowManager6rebootEv")(power);
}

/* Forked twice, so init takes the command and Nickel is left no child to wait for. Between fork
 * and exec only async-signal-safe calls are made, since Nickel has other threads. */
void nk_spawn(const char *command) {
    failsafe_stop();
    pid_t child = fork();
    if (child == 0) {
        if (fork() != 0) _exit(0);
        setsid();
        if (chdir("/")) _exit(127);
        for (int fd = 3; fd < 1024; fd++) close(fd);
        execl("/bin/sh", "sh", "-c", command, (char *)0);
        _exit(127);
    }
    if (child > 0) waitpid(child, 0, 0);
    else nk_log("couldn't start %s", command);
}

static void get_title(QStr *out, void *self) { *out = qstr(screen.title); }
static void get_small_title(QStr *out, void *self) { *out = qstr(screen.small_title); }

/* Mirrors what every settings controller's loadView does: make the view and keep a QPointer to it. */
static void load_view(void *self) {
    void **controller = self;
    void *view = alloc(0x20);
    CALL(void (*)(void *, void *), "_ZN29N3SettingsLoggingCategoryViewC1EP7QWidget")(view, 0);
    controller[3] = CALL(void *(*)(void *), "_ZN15QtSharedPointer20ExternalRefCountData9getAndRefEPK7QObject")(view);
    controller[4] = view;
    NkScreen built = {CALL(void *(*)(void *), "_ZNK29N3SettingsLoggingCategoryView15pagingContainerEv")(view)};
    learn_labels();
    screen.build(&built, screen.arg);
}

/* The abstract-controller vtable's entry is a thunk: `this` points 8 bytes into the object. */
static void load_view_thunk(void *self) { load_view((char *)self - 8); }

static void **copy_vtable(void **vtable, int slots) {
    void **copy = malloc((slots + 2) * sizeof(void *));
    memcpy(copy, vtable - 2, (slots + 2) * sizeof(void *));
    return copy + 2;
}

void nk_open_screen(const char *title, const char *small_title, void (*build)(NkScreen *, void *), void *arg) {
    void *load_real = nk_sym("_ZN35N3SettingsLoggingCategoryController8loadViewEv");
    void *title_real = nk_sym("_ZN35N3SettingsLoggingCategoryController8getTitleEv");
    void *controller_ctor = nk_sym("_ZN35N3SettingsLoggingCategoryControllerC1EP7QObject");
    if (!load_real || !title_real || !controller_ctor) return;
    screen.title = title;
    screen.small_title = small_title;
    screen.build = build;
    screen.arg = arg;
    void **controller = alloc(0x1c);
    ((void (*)(void *, void *))controller_ctor)(controller, 0);
    void **primary = (void **)controller[0], **abstract = (void **)controller[2];
    if (primary[LOAD_VIEW] != load_real || primary[GET_TITLE] != title_real) {
        nk_log("controller layout has changed on this firmware, not opening");
        return;
    }
    static void **new_primary, **new_abstract;
    if (!new_primary) {
        new_primary = copy_vtable(primary, PRIMARY_SLOTS);
        new_abstract = copy_vtable(abstract, ABSTRACT_SLOTS);
        new_primary[GET_TITLE] = get_title;
        new_primary[GET_SMALL_TITLE] = get_small_title;
        new_primary[LOAD_VIEW] = load_view;
        new_abstract[ABSTRACT_LOAD_VIEW] = load_view_thunk;
    }
    controller[0] = new_primary;
    controller[2] = new_abstract;
    void *window = CALL(void *(*)(void), "_ZN20MainWindowController14sharedInstanceEv")();
    /* Opening a screen is the riskiest thing a mod does, so a crash here disables it too. */
    guard_screen();
    CALL(void (*)(void *, void *, int), "_ZN20MainWindowController4pushEP18AbstractControllerb")(window, controller + 2, 0);
    unguard_screen();
}

/* ---- loading, as an image-format plugin ---- */

/* Qt reads this block out of the file without loading it, then loads every image-format plugin
 * it finds in /usr/local/Kobo/imageformats. It's the header from Kobo's own libkimg.so with the
 * key and names changed, so the format never matches a real image. */
__attribute__((used, section(".qtmetadata"))) static const unsigned char plugin_metadata[] = {
#include "metadata.inc"
};

__attribute__((visibility("default"))) const char *qt_plugin_query_metadata(void) {
    return (const char *)plugin_metadata;
}

__attribute__((visibility("default"))) void *qt_plugin_instance(void) { return 0; }

/* A mod that crashes Nickel mustn't stop the Kobo starting. The file is moved away while it's being
 * tried, so a crash means it isn't loaded next time, and it's put back after a quiet while. Each move
 * is synced, because a frozen Kobo gets held off, and an unsynced move can be lost with the power. */
static void failsafe_arm(void) { rename(INSTALLED_AT, DISABLED_AT); sync(); }
static void failsafe_clear(void) { rename(DISABLED_AT, INSTALLED_AT); sync(); }

static pthread_mutex_t failsafe_lock = PTHREAD_MUTEX_INITIALIZER;
static long failsafe_until; /* while this is set, a thread clears the guard once it's passed */

static long now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec;
}

static void *failsafe_wait(void *unused) {
    for (;;) {
        pthread_mutex_lock(&failsafe_lock);
        long left = failsafe_until - now();
        if (left <= 0) {
            failsafe_until = 0;
            failsafe_clear();
            pthread_mutex_unlock(&failsafe_lock);
            nk_log("failsafe cleared");
            return 0;
        }
        pthread_mutex_unlock(&failsafe_lock);
        sleep(left);
    }
}

void nk_failsafe(int seconds) {
    pthread_mutex_lock(&failsafe_lock);
    if (!failsafe_until) {
        failsafe_arm();
        pthread_t thread;
        pthread_create(&thread, 0, failsafe_wait, 0);
        pthread_detach(thread);
    }
    if (now() + seconds > failsafe_until) failsafe_until = now() + seconds;
    pthread_mutex_unlock(&failsafe_lock);
}

/* A restart Nickel chooses isn't a crash, so it mustn't disable the mod. A crash or abort never
 * runs atexit handlers, so those are still caught. */
static void failsafe_stop(void) {
    pthread_mutex_lock(&failsafe_lock);
    if (failsafe_until) {
        failsafe_until = 0;
        failsafe_clear();
    }
    pthread_mutex_unlock(&failsafe_lock);
}

/* A screen opening while the guard is already timed leaves the guard to its thread. */
static void guard_screen(void) {
    pthread_mutex_lock(&failsafe_lock);
    if (!failsafe_until) failsafe_arm();
    pthread_mutex_unlock(&failsafe_lock);
}

static void unguard_screen(void) {
    pthread_mutex_lock(&failsafe_lock);
    if (!failsafe_until) failsafe_clear();
    pthread_mutex_unlock(&failsafe_lock);
}

__attribute__((constructor)) static void load(void) {
    /* Other Qt programs on the Kobo load image plugins too. */
    if (!strstr(program_invocation_short_name, "nickel")) return;
    if (access(UNINSTALL_FLAG, F_OK) == 0) {
        unlink(INSTALLED_AT);
        unlink(DISABLED_AT);
        unlink(UNINSTALL_FLAG);
        nk_log("uninstalled");
        return;
    }
    nk_failsafe(FAILSAFE_SECONDS);
    atexit(failsafe_stop);
    nk_log("loaded");
    nk_mod_init();
}
