/* nk.patch: changing bytes in Nickel's memory while it runs. The files on disk aren't changed.
 *
 * A patch is a list of edits to libnickel or the nickel program, both mapped into Nickel's memory,
 * so each edit's file offset is found in a loaded segment and written there. The rules are here,
 * not in the mod: only those two files, every edit's bytes are checked before any is written, and
 * a patch is written whole or not at all.
 *
 * Most patches change code Nickel runs again each time it's needed. The rest need a nudge, and
 * nk.patch.refresh and the fixups below give it, the way Nickel itself would.
 *
 * As in lua.cpp, nothing raises a Lua error while a Qt value made here is still alive. */
#include <QApplication>
#include <QByteArray>
#include <QList>
#include <QString>
#include <QWidget>
#include <dlfcn.h>
#include <link.h>
#include <string.h>
#include <sys/mman.h>
#include "kit.h"
#include "lua_kit.h"

#define CALL(type, name) ((type)nk_sym(name))
#define GUARD_SECONDS 20
#define MAX_EDITS 4096
#define MAX_BYTES (1 << 15)

static const char *const allowed_files[] = {"/usr/local/Kobo/libnickel.so.1.0.0", "/usr/local/Kobo/nickel"};

struct Edit {
    int file; /* an index into allowed_files */
    unsigned offset;
    QByteArray find, replace;
};

/* ---- finding and changing bytes ---- */

struct Lookup {
    int file;
    unsigned offset;
    unsigned char *address;
    int prot;
};

static bool is_nickel(const char *name) {
    size_t length = strlen(name);
    return !*name || (length >= 7 && !strcmp(name + length - 7, "/nickel"));
}

static int find_segment(struct dl_phdr_info *info, size_t, void *data) {
    Lookup *l = static_cast<Lookup *>(data);
    if (l->file == 0 ? !strstr(info->dlpi_name, "libnickel.so") : !is_nickel(info->dlpi_name)) return 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_LOAD || l->offset < ph->p_offset || l->offset >= ph->p_offset + ph->p_filesz) continue;
        l->address = reinterpret_cast<unsigned char *>(info->dlpi_addr + ph->p_vaddr + (l->offset - ph->p_offset));
        l->prot = (ph->p_flags & PF_R ? PROT_READ : 0) | (ph->p_flags & PF_W ? PROT_WRITE : 0) | (ph->p_flags & PF_X ? PROT_EXEC : 0);
    }
    return 1;
}

/* Where an edit is in memory, or 0. The whole edit has to be inside one segment. */
static unsigned char *address_of(const Edit &edit, int *prot = 0) {
    Lookup l = {edit.file, edit.offset, 0, 0};
    dl_iterate_phdr(find_segment, &l);
    Lookup end = {edit.file, edit.offset + edit.find.size() - 1, 0, 0};
    dl_iterate_phdr(find_segment, &end);
    if (!end.address || end.address != l.address + edit.find.size() - 1) return 0;
    if (prot) *prot = l.prot;
    return l.address;
}

static bool matches(const unsigned char *at, const QByteArray &bytes) { return !memcmp(at, bytes.constData(), bytes.size()); }

enum { OFF, ON, CHANGED, PARTLY };

static int state_of(const QList<Edit> &edits) {
    bool on = true, off = true;
    for (int i = 0; i < edits.size(); i++) {
        const unsigned char *at = address_of(edits[i]);
        if (!at) return CHANGED;
        bool edit_on = matches(at, edits[i].replace), edit_off = matches(at, edits[i].find);
        if (!edit_on && !edit_off) return CHANGED;
        on = on && edit_on;
        off = off && edit_off;
    }
    return on ? ON : off ? OFF : PARTLY;
}

static bool write_bytes(unsigned char *at, const QByteArray &bytes, int prot) {
    uintptr_t start = (uintptr_t)at & ~(uintptr_t)0xfff, end = ((uintptr_t)at + bytes.size() + 0xfff) & ~(uintptr_t)0xfff;
    if (mprotect((void *)start, end - start, prot | PROT_WRITE)) return false;
    memcpy(at, bytes.constData(), bytes.size());
    __builtin___clear_cache((char *)at, (char *)at + bytes.size());
    mprotect((void *)start, end - start, prot);
    return true;
}

/* ---- stylesheets ---- */

/* A stylesheet edit replaces a Qt resource's compressed bytes. They follow a 4-byte length, and
 * unpacking them the way Qt does gives exactly the text that setStyleSheetFile gave each widget. */
static bool is_stylesheet(const Edit &edit) { return edit.file == 1 && edit.find.startsWith("\x78\x9c"); }

static QString unpack(const unsigned char *zlib) {
    int length = zlib[-8] << 24 | zlib[-7] << 16 | zlib[-6] << 8 | zlib[-5];
    QByteArray bytes = qUncompress(zlib - 4, length);
    return QString::fromUtf8(bytes.constData());
}

/* A stylesheet can hide a widget with "#name { qproperty-visible: false; }", but Qt doesn't show it
 * again when that rule goes. */
static bool hides(const QString &sheet, const QString &name) {
    return sheet.contains(QStringLiteral("#") + name + QStringLiteral(" {\n  qproperty-visible: false;"));
}

/* Kobo's Qt adds QWidget::setStyleSheetFile, which reads the file each time with no cache and
 * doesn't keep its name. So the widgets showing the old stylesheet are found by its text. */
static void restyle(const QString &before, const QString &after) {
    QList<QWidget *> widgets = QApplication::allWidgets();
    int count = 0;
    for (int i = 0; i < widgets.size(); i++) {
        QWidget *widget = widgets[i];
        if (widget->styleSheet() == before) {
            widget->setStyleSheet(after);
            count++;
        }
        QString name = widget->objectName();
        if (!name.isEmpty() && hides(before, name) && !hides(after, name)) widget->show();
    }
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
    {"4.46.23852", 0xa3ee45, 0x17489d8, 0x17489dc, 0x38},
};

static char *libnickel_base;

static int find_libnickel(struct dl_phdr_info *info, size_t, void *) {
    if (!strstr(info->dlpi_name, "libnickel.so")) return 0;
    libnickel_base = reinterpret_cast<char *>(info->dlpi_addr);
    return 1;
}

static int fixups() {
    const char *version = kit_firmware();
    for (size_t i = 0; i < sizeof known / sizeof *known; i++) {
        if (strcmp(known[i].version, version)) continue;
        char *scalars = static_cast<char *>(dlsym(RTLD_DEFAULT, "_ZNK15ReadingSettings17lineHeightScalarsEv"));
        if (scalars && libnickel_base && scalars - libnickel_base == (long)known[i].scalars) return (int)i;
    }
    return -1;
}

/* Whether any of the edits fall inside a function. */
static bool edits_function(const QList<Edit> &edits, const char *symbol) {
    void *start = dlsym(RTLD_DEFAULT, symbol);
    Dl_info info;
    const ElfW(Sym) *entry = 0;
    if (!start || !dladdr1(start, &info, (void **)&entry, RTLD_DL_SYMENT) || !entry) return false;
    uintptr_t from = (uintptr_t)start & ~(uintptr_t)1, to = from + entry->st_size;
    for (int i = 0; i < edits.size(); i++) {
        uintptr_t at = (uintptr_t)address_of(edits[i]);
        if (at >= from && at < to) return true;
    }
    return false;
}

/* Clears the cached list so the next call builds it from the changed code. Every rebuild registers
 * one more destructor to run at exit, so the new list gets an extra reference for each rebuild so
 * far, and is freed by the last of them. The old list is released, and kept if it has extra
 * references from an earlier rebuild. Returns false if a restart is needed instead. */
static bool rebuild_line_spacings() {
    int known_at = fixups();
    if (known_at < 0) return false;
    int *guard = reinterpret_cast<int *>(libnickel_base + known[known_at].scalars_guard);
    int **list = reinterpret_cast<int **>(libnickel_base + known[known_at].scalars_list);
    if (!(*guard & 1)) return true; /* not built yet, so it'll be built from the new code */
    void (*scalars)(int **, void *) = CALL(void (*)(int **, void *), "_ZNK15ReadingSettings17lineHeightScalarsEv");
    void (*destroy)(int **) = CALL(void (*)(int **), "_ZN5QListIdED1Ev"); /* QList<double>, which also frees the doubles */
    static int rebuilds;
    if (**list < 1 || !scalars || !destroy) return false;
    destroy(list);
    rebuilds++;
    *guard = 0;
    int *copy;
    scalars(&copy, 0);
    __atomic_add_fetch(*list, rebuilds, __ATOMIC_SEQ_CST);
    destroy(&copy);
    nk_log("rebuilt the line spacing list");
    return true;
}

/* ---- telling Nickel to look again ---- */

static QObject *home_controller;
static void *(*real_home_controller)(void *, int, void *);

/* MainWindowController can delete the home page's controller and make a new one, so the current
 * one is kept, and forgotten when Qt says it's destroyed. */
static void *hooked_home_controller(void *controller, int flag, void *parent) {
    void *result = real_home_controller(controller, flag, parent);
    QObject *object = static_cast<QObject *>(controller);
    home_controller = object;
    QObject::connect(object, &QObject::destroyed, [object]() {
        if (home_controller == object) home_controller = 0;
    });
    return result;
}

static void refresh() {
    int known_at = fixups();
    void (*main_nav)(void *) = CALL(void (*)(void *), "_ZN11MainNavView20updateVisibleButtonsEv");
    void (*more)(void *) = CALL(void (*)(void *), "_ZN8MoreView20updateVisibleButtonsEv");
    void (*configure)(void *) = CALL(void (*)(void *), "_ZN18HomePageController16configureWidgetsEv");
    QList<QWidget *> widgets = QApplication::allWidgets();
    for (int i = 0; i < widgets.size(); i++) {
        QWidget *widget = widgets[i];
        const char *name = widget->metaObject()->className();
        if (!strcmp(name, "MainNavView") && main_nav) main_nav(widget);
        else if (!strcmp(name, "MoreView") && more) more(widget);
        else if (!strcmp(name, "HomePageView") && known_at >= 0) {
            /* Shown first so that configureWidgets decides afresh whether it's wanted. */
            QWidget *column = reinterpret_cast<QWidget **>(widget)[known[known_at].home_top_right / 4];
            if (column && !strcmp(column->metaObject()->className(), "QStackedWidget")) column->show();
        }
    }
    if (home_controller && configure) configure(home_controller);
}

/* Checks every edit, then writes them all. Sets *restart when Nickel won't see the change until it
 * restarts. */
static bool set(const QList<Edit> &edits, bool on, bool restart_flag, bool cached, bool *restart) {
    for (int i = 0; i < edits.size(); i++) {
        const unsigned char *at = address_of(edits[i]);
        if (!at || !(matches(at, edits[i].find) || matches(at, edits[i].replace))) return false;
    }
    nk_failsafe(GUARD_SECONDS);
    QList<QString> before;
    for (int i = 0; i < edits.size(); i++) before << (is_stylesheet(edits[i]) ? unpack(address_of(edits[i])) : QString());
    for (int i = 0; i < edits.size(); i++) {
        int prot;
        unsigned char *at = address_of(edits[i], &prot);
        if (!write_bytes(at, on ? edits[i].replace : edits[i].find, prot)) nk_log("couldn't write an edit at %u", edits[i].offset);
    }
    for (int i = 0; i < edits.size(); i++)
        if (is_stylesheet(edits[i])) restyle(before[i], unpack(address_of(edits[i])));
    /* make patches marks code that runs once as cached. The line spacing list is the one the kit
     * can rebuild; anything else cached takes a restart. */
    bool spacing = edits_function(edits, "_ZNK15ReadingSettings17lineHeightScalarsEv");
    bool rebuilt = spacing && rebuild_line_spacings();
    *restart = restart_flag || ((cached || spacing) && !rebuilt);
    if (!on && edits_function(edits, "_ZN12HomePageView17configureTopRightE15HomePageWidgets") && fixups() < 0) *restart = true;
    return true;
}

/* ---- Lua ---- */

static bool is_hex(const char *text, size_t length) {
    if (!length || length % 2 || length > MAX_BYTES * 2) return false;
    for (size_t i = 0; i < length; i++)
        if (!strchr("0123456789abcdef", text[i])) return false;
    return true;
}

/* A list of edits, {file, at, find, replace} with the bytes in hex. Returns why it can't be used,
 * or 0. */
static const char *read_edits(lua_State *co, int index, QList<Edit> *out) {
    if (!lua_istable(co, index)) return "edits should be a list";
    int count = (int)lua_rawlen(co, index);
    if (!count || count > MAX_EDITS) return "a patch has 1 to 4096 edits";
    for (int i = 1; i <= count; i++) {
        if (lua_rawgeti(co, index, i) != LUA_TTABLE) {
            lua_pop(co, 1);
            return "an edit should be a table";
        }
        Edit edit;
        edit.file = -1;
        lua_getfield(co, -1, "file");
        const char *file = lua_tostring(co, -1);
        for (int k = 0; file && k < 2; k++)
            if (!strcmp(file, allowed_files[k])) edit.file = k;
        lua_getfield(co, -2, "at");
        lua_Integer at = lua_isinteger(co, -1) ? lua_tointeger(co, -1) : -1;
        size_t find_size = 0, replace_size = 0;
        lua_getfield(co, -3, "find");
        const char *find = lua_tolstring(co, -1, &find_size);
        lua_getfield(co, -4, "replace");
        const char *replace = lua_tolstring(co, -1, &replace_size);
        const char *why = 0;
        if (edit.file < 0) why = "an edit may only change libnickel or the nickel program";
        else if (at < 0 || at > 0x7fffffff) why = "an edit's offset isn't right";
        else if (!find || !replace || !is_hex(find, find_size) || find_size != replace_size) why = "an edit's bytes aren't right";
        if (!why) {
            edit.offset = (unsigned)at;
            edit.find = QByteArray::fromHex(QByteArray(find, (int)find_size));
            edit.replace = QByteArray::fromHex(QByteArray(replace, (int)replace_size));
            *out << edit;
        }
        lua_pop(co, 5);
        if (why) return why;
    }
    return 0;
}

static bool ready;

/* Everything this needs from Nickel, looked up once. Without it, nk.patch.ready() is false and
 * nothing else here does anything. */
static bool init() {
    static bool done;
    if (done) return ready;
    done = true;
    static const char *const symbols[] = {"_ZN11MainNavView20updateVisibleButtonsEv", "_ZN8MoreView20updateVisibleButtonsEv",
                                          "_ZN18HomePageController16configureWidgetsEv"};
    for (size_t i = 0; i < sizeof symbols / sizeof *symbols; i++)
        if (!nk_sym(symbols[i])) return false;
    dl_iterate_phdr(find_libnickel, 0);
    real_home_controller = reinterpret_cast<void *(*)(void *, int, void *)>(
        nk_hook("_ZN18HomePageControllerC1EbP7QObject", reinterpret_cast<void *>(hooked_home_controller)));
    if (fixups() < 0) nk_log("no fixups for this firmware: some patches will need a restart");
    ready = real_home_controller != 0;
    return ready;
}

/* nk.patch.ready(): whether this firmware has what patching needs. Call it from main.lua, before
 * Nickel has built anything, so the kit sees every home page made. */
static int l_ready(lua_State *co) {
    lua_pushboolean(co, init());
    return 1;
}

/* nk.patch.state(edits): "on", "off", "partly" (some edits each way, as an older version of the
 * patch with fewer edits leaves things) or "changed" (bytes that are neither side's). */
static int l_state(lua_State *co) {
    static const char *const names[] = {"off", "on", "changed", "partly"};
    int state = -1;
    const char *why;
    {
        QList<Edit> edits;
        why = read_edits(co, 1, &edits);
        if (!why) state = init() ? state_of(edits) : CHANGED;
    }
    if (why) return luaL_error(co, "%s", why);
    lua_pushstring(co, names[state]);
    return 1;
}

/* nk.patch.set(edits, on, {restart, cached}): writes every edit's bytes, after checking each one's
 * current bytes are one side's or the other's. Returns false if they weren't, and then nothing was
 * changed. Else true, and whether Nickel has already used something the patch changes and won't
 * look again until it restarts. The crash guard is on while it writes. */
static int l_set(lua_State *co) {
    bool on = lua_toboolean(co, 2), restart_flag = false, cached = false;
    if (lua_istable(co, 3)) {
        lua_getfield(co, 3, "restart");
        lua_getfield(co, 3, "cached");
        restart_flag = lua_toboolean(co, -2);
        cached = lua_toboolean(co, -1);
        lua_pop(co, 2);
    }
    bool ok = false, restart = false;
    const char *why;
    {
        QList<Edit> edits;
        why = read_edits(co, 1, &edits);
        if (!why && init()) ok = set(edits, on, restart_flag, cached, &restart);
    }
    if (why) return luaL_error(co, "%s", why);
    lua_pushboolean(co, ok);
    lua_pushboolean(co, restart);
    return 2;
}

/* nk.patch.refresh(): asks the screens Nickel has already built to update, so changes show without
 * a restart. */
static int l_refresh(lua_State *) {
    if (init()) refresh();
    return 0;
}

void kit_open_patch(lua_State *L) {
    static const luaL_Reg functions[] = {{"ready", l_ready}, {"state", l_state}, {"set", l_set}, {"refresh", l_refresh}, {0, 0}};
    lua_newtable(L);
    luaL_setfuncs(L, functions, 0);
    lua_setfield(L, -2, "patch");
}
