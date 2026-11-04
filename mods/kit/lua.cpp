/* The kit's Lua side, built into a mod that has a main.lua. The mod is then only scripts: main.lua
 * runs once when Nickel starts and says where the mod goes, and everything else runs when it's
 * tapped or timed.
 *
 * - Every callback runs as its own coroutine, so a script can wait (nk.sleep, nk.confirm) without
 *   holding Nickel up. The kit starts the work, the coroutine yields, and the kit resumes it with
 *   the answer.
 * - Every call into Lua is protected. An error is logged with its file and line and shown in
 *   Kobo's error dialog. It never takes Nickel down.
 * - Scripts and icons are built into the .so. If /mnt/onboard/.adds/<mod>/dev is a folder, scripts
 *   are read from there instead, and when one has changed, the next screen to open runs main.lua
 *   again first. Tables from nk.keep, and places already in Nickel, stay.
 *
 * Lua reports errors with longjmp, which skips C++ destructors. So no function here raises a Lua
 * error while a Qt value it made is still alive. */
#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLabel>
#include <QVariant>
#include <QList>
#include <QPointer>
#include <QTimer>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
extern "C" {
#include "vendor/lua/lualib.h"
}
#include "kit.h"
#include "lua_kit.h"

#define DEV NK_FOLDER "/dev"
#define PREFS NK_FOLDER "/settings.ini"

struct Embedded {
    const char *name;
    const unsigned char *data;
    unsigned size;
};
#include <embedded.inc> /* build.sh: the mod's scripts and icons, as embedded[] */

/* The kit starts the mod from a constructor that can run before this file's own, so nothing here
 * has one: what needs one is made in nk_mod_init. */
static lua_State *L;
static lua_State *running; /* the coroutine being resumed, if any */
static QHash<lua_State *, int> *tasks; /* each coroutine, to its reference that keeps it alive */
static bool started; /* main.lua has run, so Nickel's UI exists and dialogs can show */
static QByteArray *startup_error;

/* ---- scripts ---- */

static const Embedded *find_embedded(const char *name) {
    for (unsigned i = 0; i < sizeof embedded / sizeof *embedded; i++)
        if (!strcmp(embedded[i].name, name)) return &embedded[i];
    return 0;
}

bool kit_embedded(const char *name, const unsigned char **data, unsigned *size) {
    const Embedded *file = find_embedded(name);
    if (!file) return false;
    *data = file->data;
    *size = file->size;
    return true;
}

static bool dev_mode() { return QFileInfo(QStringLiteral(DEV)).isDir(); }

/* A script's text: from the dev folder when there is one, else built in. */
static bool read_script(const char *name, QByteArray *out) {
    if (dev_mode()) {
        QFile file(QStringLiteral(DEV "/") + QString::fromUtf8(name));
        if (file.open(QIODevice::ReadOnly)) {
            *out = file.readAll();
            return true;
        }
    }
    const Embedded *built = find_embedded(name);
    if (!built) return false;
    *out = QByteArray::fromRawData(reinterpret_cast<const char *>(built->data), built->size);
    return true;
}

/* What the dev folder holds, to tell when a script has changed: each file's name, size and time. */
static QByteArray dev_stamp() {
    QByteArray stamp;
    QFileInfoList files = QDir(QStringLiteral(DEV)).entryInfoList(QDir::Files, QDir::Name);
    for (int i = 0; i < files.size(); i++)
        stamp += files[i].fileName().toUtf8() + ' ' + QByteArray::number(files[i].size()) + ' ' +
                 QByteArray::number(files[i].lastModified().toMSecsSinceEpoch()) + '\n';
    return stamp;
}
static QByteArray *loaded_stamp;

/* ---- running Lua ---- */

static void report(lua_State *co) {
    const char *message = lua_tostring(co, -1);
    luaL_traceback(L, co, message ? message : "an error without a message", 0);
    nk_log("%s", lua_tostring(L, -1));
    lua_pop(L, 1);
    QByteArray text = message ? message : "an error without a message";
    if (started) nk_error(NK_NAME, text.constData());
    else if (startup_error->isEmpty()) *startup_error = text;
}

/* A coroutine that will run the function `fn` refers to. Push its arguments, then resume it. */
static lua_State *task(int fn) {
    lua_State *co = lua_newthread(L);
    tasks->insert(co, luaL_ref(L, LUA_REGISTRYINDEX));
    lua_rawgeti(co, LUA_REGISTRYINDEX, fn);
    return co;
}

/* Run a coroutine until it ends or waits. One that waits is resumed by whatever it waits for. */
static void resume(lua_State *co, int args) {
    lua_State *outer = running;
    running = co;
    int results = 0;
    int status = lua_resume(co, outer, args, &results);
    running = outer;
    if (status == LUA_YIELD) {
        lua_pop(co, results);
        return;
    }
    if (status != LUA_OK) report(co);
    else lua_pop(co, results);
    luaL_unref(L, LUA_REGISTRYINDEX, tasks->take(co));
}

QByteArray kit_ask(int fn, void (*push)(lua_State *, void *), void *arg) {
    lua_State *co = task(fn);
    int args = lua_gettop(co) - 1;
    if (push) push(co, arg);
    args = lua_gettop(co) - 1;
    lua_State *outer = running;
    running = co;
    int results = 0;
    int status = lua_resume(co, outer, args, &results);
    running = outer;
    QByteArray answer;
    if (status == LUA_YIELD) {
        /* It goes on by itself; there's no answer now. */
        lua_pop(co, results);
        nk_log("a function that had to answer straight away waited instead");
        return answer;
    }
    if (status != LUA_OK) report(co);
    else {
        if (results > 0 && lua_type(co, -results) == LUA_TSTRING) answer = lua_tostring(co, -results);
        lua_pop(co, results);
    }
    luaL_unref(L, LUA_REGISTRYINDEX, tasks->take(co));
    return answer;
}

/* For functions that wait: they need a coroutine the kit can resume. */
static void must_wait_here(lua_State *co) {
    if (!tasks->contains(co) || !lua_isyieldable(co)) luaL_error(co, "this can only wait in a callback");
}

static int ref_function(lua_State *co, int index) {
    luaL_checktype(co, index, LUA_TFUNCTION);
    lua_pushvalue(co, index);
    return luaL_ref(co, LUA_REGISTRYINDEX);
}

lua_State *kit_lua() { return L; }
lua_State *kit_task(int fn) { return task(fn); }
void kit_resume(lua_State *co, int args) { resume(co, args); }
void kit_must_wait(lua_State *co) { must_wait_here(co); }
int kit_ref(lua_State *co, int index) { return ref_function(co, index); }

static int l_require(lua_State *co) {
    const char *name = luaL_checkstring(co, 1);
    lua_getfield(co, LUA_REGISTRYINDEX, "nk.loaded");
    lua_getfield(co, -1, name);
    if (!lua_isnil(co, -1)) return 1;
    lua_pop(co, 1);
    int status;
    {
        QByteArray file = QByteArray(name) + ".lua", text;
        if (!read_script(file.constData(), &text)) status = -1;
        else status = luaL_loadbuffer(co, text.constData(), text.size(), ("@" + file).constData());
    }
    if (status == -1) return luaL_error(co, "no script %s.lua", name);
    if (status != LUA_OK) return lua_error(co);
    lua_pushstring(co, name);
    lua_call(co, 1, 1);
    if (lua_isnil(co, -1)) {
        lua_pop(co, 1);
        lua_pushboolean(co, 1);
    }
    lua_pushvalue(co, -1);
    lua_setfield(co, -3, name);
    return 1;
}

static void forget_places();

/* (Re)runs main.lua with fresh modules, and the rows and screen watchers it adds. */
static void run_main() {
    forget_places();
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "nk.loaded");
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "nk.keepcount");
    if (dev_mode()) *loaded_stamp = dev_stamp();
    lua_pushcfunction(L, l_require);
    int fn = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_State *co = task(fn);
    luaL_unref(L, LUA_REGISTRYINDEX, fn);
    lua_pushstring(co, "main");
    resume(co, 1);
}

/* Before a place opens: in development, a changed script means main.lua runs again. */
static bool ready_to_open() {
    if (!startup_error->isEmpty()) {
        nk_error(NK_NAME " couldn't start", startup_error->constData());
        return false;
    }
    if (dev_mode() && dev_stamp() != *loaded_stamp) {
        nk_log("scripts changed, running main.lua again");
        run_main();
    }
    return true;
}

bool kit_ready_to_open() { return ready_to_open(); }

/* ---- development, for mods/tools/kobo through the devtools mod ---- */

/* Each writes its answer into out, and returns 0 if it failed. They do nothing unless DEV exists,
 * so a release behaves as built. */
static int dev_answer(char *out, int size, const QByteArray &text, int ok) {
    qstrncpy(out, text.constData(), size);
    return ok;
}

/* Runs main.lua again from the dev folder, and rebuilds the tab if it's showing. */
extern "C" __attribute__((visibility("default"))) int nk_dev_reload(const char *, char *out, int size) {
    if (!dev_mode()) return dev_answer(out, size, "no " DEV ", so the scripts are the built-in ones\n", 0);
    run_main();
    if (kit_reload_tab()) return dev_answer(out, size, "ran main.lua again and rebuilt the tab\n", 1);
    return dev_answer(out, size, "ran main.lua again; reopen the screen to see it\n", 1);
}

/* A value as text, without its __tostring, which could raise an error here. */
static QByteArray describe(lua_State *co, int index) {
    switch (lua_type(co, index)) {
    case LUA_TNIL: return "nil";
    case LUA_TBOOLEAN: return lua_toboolean(co, index) ? "true" : "false";
    case LUA_TNUMBER:
    case LUA_TSTRING: return lua_tostring(co, index);
    default: return QByteArray(luaL_typename(co, index)) + ": 0x" + QByteArray::number(quintptr(lua_topointer(co, index)), 16);
    }
}

/* Runs code in the mod's state, as an expression if it is one, in a callback's coroutine so it
 * can wait. The answer is what it returns, or its error. One that waits finishes on its own, and
 * print() in it goes to the log. */
extern "C" __attribute__((visibility("default"))) int nk_dev_eval(const char *code, char *out, int size) {
    if (!dev_mode()) return dev_answer(out, size, "no " DEV ", so eval is off\n", 0);
    lua_State *co = lua_newthread(L);
    tasks->insert(co, luaL_ref(L, LUA_REGISTRYINDEX));
    QByteArray expression = QByteArray("return ") + code;
    if (luaL_loadbuffer(co, expression.constData(), expression.size(), "=eval") != LUA_OK) {
        lua_pop(co, 1);
        if (luaL_loadbuffer(co, code, qstrlen(code), "=eval") != LUA_OK) {
            QByteArray error = describe(co, -1) + "\n";
            luaL_unref(L, LUA_REGISTRYINDEX, tasks->take(co));
            return dev_answer(out, size, error, 0);
        }
    }
    lua_State *outer = running;
    running = co;
    int results = 0;
    int status = lua_resume(co, outer, 0, &results);
    running = outer;
    if (status == LUA_YIELD) {
        lua_pop(co, results);
        return dev_answer(out, size, "waiting; it finishes on its own, and print() goes to the log\n", 1);
    }
    QByteArray answer;
    if (status == LUA_OK) {
        for (int i = -results; i < 0; i++) answer += describe(co, i) + (i < -1 ? "\t" : "");
        lua_pop(co, results);
    } else {
        luaL_traceback(L, co, lua_tostring(co, -1), 0);
        answer = lua_tostring(L, -1);
        lua_pop(L, 1);
    }
    luaL_unref(L, LUA_REGISTRYINDEX, tasks->take(co));
    return dev_answer(out, size, answer + "\n", status == LUA_OK);
}

/* ---- screens and their blocks ---- */

/* Every page is a PagingContainer, Kobo's own list that turns pages, whether it's a screen's or a
 * tab's. Blocks are added to it in order. */
struct Page {
    QPointer<QWidget> paging;
    int build;
    bool refreshing;
    bool tab;
};

static Page *check_page(lua_State *co) {
    Page *page = static_cast<Page *>(luaL_checkudata(co, 1, "nk.page"));
    if (!page->paging) luaL_error(co, "this screen has closed");
    return page;
}

PagingContainer *kit_check_page(lua_State *co) { return static_cast<PagingContainer *>(check_page(co)->paging.data()); }

static NkScreen screen_of(Page *page) {
    NkScreen screen = {static_cast<PagingContainer *>(page->paging.data())};
    return screen;
}

/* A callback for a block, with what it needs to run. */
struct KitHook {
    int fn;
    QPointer<QObject> widget;
};
typedef KitHook Hook;

static Hook *new_hook(lua_State *co, int index) {
    Hook *hook = new Hook;
    hook->fn = ref_function(co, index);
    return hook;
}

/* The hook goes with its widget, when the screen closes or is built again. */
static void hook_to(Hook *hook, QObject *widget) {
    hook->widget = widget;
    QObject::connect(widget, &QObject::destroyed, [hook]() {
        luaL_unref(L, LUA_REGISTRYINDEX, hook->fn);
        delete hook;
    });
}

KitHook *kit_hook(lua_State *co, int index, QObject *widget) {
    Hook *hook = new_hook(co, index);
    hook_to(hook, widget);
    return hook;
}

lua_State *kit_hook_task(KitHook *hook) { return hook->widget ? task(hook->fn) : 0; }

static void tapped(void *arg) {
    resume(task(static_cast<Hook *>(arg)->fn), 0);
}

static void toggled(void *arg) {
    Hook *hook = static_cast<Hook *>(arg);
    if (!hook->widget) return;
    lua_State *co = task(hook->fn);
    lua_pushboolean(co, nk_toggle_on(reinterpret_cast<NkToggle *>(hook->widget.data())));
    resume(co, 1);
}

/* A handle on a widget for Lua, which goes stale when the widget does. */
static void push_handle(lua_State *co, QObject *widget, const char *type) {
    new (lua_newuserdatauv(co, sizeof(QPointer<QObject>), 0)) QPointer<QObject>(widget);
    luaL_setmetatable(co, type);
}

static QObject *check_handle(lua_State *co, const char *type) {
    QPointer<QObject> *handle = static_cast<QPointer<QObject> *>(luaL_checkudata(co, 1, type));
    if (!*handle) luaL_error(co, "this block has gone");
    return handle->data();
}

void kit_push_handle(lua_State *co, QObject *widget, const char *type) { push_handle(co, widget, type); }

QObject *kit_check_handle(lua_State *co, int index, const char *type) {
    QPointer<QObject> *handle = static_cast<QPointer<QObject> *>(luaL_checkudata(co, index, type));
    if (!*handle) luaL_error(co, "this block has gone");
    return handle->data();
}

static int handle_gc(lua_State *co) {
    static_cast<QPointer<QObject> *>(lua_touserdata(co, 1))->~QPointer<QObject>();
    return 0;
}

/* Every block returns a handle, whose text can change with :set(text). */
static int page_section(lua_State *co) {
    Page *page = check_page(co);
    NkScreen s = screen_of(page);
    push_handle(co, reinterpret_cast<QObject *>(nk_section(&s, luaL_checkstring(co, 2))), "nk.label");
    return 1;
}

static int page_text(lua_State *co) {
    Page *page = check_page(co);
    NkScreen s = screen_of(page);
    push_handle(co, reinterpret_cast<QObject *>(nk_text(&s, luaL_checkstring(co, 2))), "nk.label");
    return 1;
}

/* Its handle is the value's. */
static int page_value(lua_State *co) {
    Page *page = check_page(co);
    NkScreen s = screen_of(page);
    push_handle(co, reinterpret_cast<QObject *>(nk_value_row(&s, luaL_checkstring(co, 2), luaL_checkstring(co, 3))), "nk.label");
    return 1;
}

/* page:toggle(title, on, changed): changed(on) runs whenever it's ticked or unticked. */
static int page_toggle(lua_State *co) {
    Page *page = check_page(co);
    const char *title = luaL_checkstring(co, 2);
    int on = lua_toboolean(co, 3);
    Hook *hook = new_hook(co, 4);
    NkScreen s = screen_of(page);
    NkToggle *toggle = nk_toggle_row(&s, title, on, toggled, hook);
    hook_to(hook, reinterpret_cast<QObject *>(toggle));
    push_handle(co, hook->widget, "nk.toggle");
    return 1;
}

static int toggle_on(lua_State *co) {
    lua_pushboolean(co, nk_toggle_on(reinterpret_cast<NkToggle *>(check_handle(co, "nk.toggle"))));
    return 1;
}

static int toggle_set(lua_State *co) {
    NkToggle *toggle = reinterpret_cast<NkToggle *>(check_handle(co, "nk.toggle"));
    nk_toggle_set(toggle, lua_toboolean(co, 2));
    return 0;
}

/* page:button(title, label, tapped). Returns the title and the button, as handles. */
static int page_button(lua_State *co) {
    Page *page = check_page(co);
    const char *title = luaL_checkstring(co, 2), *button = luaL_checkstring(co, 3);
    Hook *hook = new_hook(co, 4);
    NkScreen s = screen_of(page);
    QLabel *label = reinterpret_cast<QLabel *>(nk_button_row(&s, title, button, tapped, hook));
    hook_to(hook, label);
    push_handle(co, label, "nk.label");
    QList<QLabel *> labels = label->parentWidget()->findChildren<QLabel *>();
    QLabel *action = 0;
    for (int i = 0; i < labels.size(); i++)
        if (labels[i]->inherits("N3ButtonLabel")) action = labels[i];
    push_handle(co, action, "nk.label");
    return 2;
}

static int label_set(lua_State *co) {
    QLabel *label = static_cast<QLabel *>(check_handle(co, "nk.label"));
    const char *text = luaL_checkstring(co, 2);
    kit_set_text(label, QString::fromUtf8(text));
    return 0;
}

/* handle:show(on): a block that isn't shown takes no room. */
static int label_show(lua_State *co) {
    static_cast<QWidget *>(check_handle(co, "nk.label"))->setVisible(lua_toboolean(co, 2));
    return 0;
}

static void build_page(Page *page, int page_ref) {
    lua_State *co = task(page->build);
    lua_rawgeti(co, LUA_REGISTRYINDEX, page_ref);
    resume(co, 1);
}

/* page:refresh(): the page is built again by its build function, from the script's own state.
 * Its screen or tab is closed and opened again, once the current tap or timer is done, since the
 * widget it came from goes with it. Only the screen opened last can be refreshed. */
static void rebuild(void *arg) {
    int page_ref = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    lua_rawgeti(L, LUA_REGISTRYINDEX, page_ref);
    Page *page = static_cast<Page *>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    page->refreshing = false;
    if (page->paging && page->tab) kit_reopen_tab();
    else if (page->paging) kit_reopen_screen();
    luaL_unref(L, LUA_REGISTRYINDEX, page_ref);
}

static int page_refresh(lua_State *co) {
    Page *page = check_page(co);
    if (page->refreshing) return 0;
    page->refreshing = true;
    lua_pushvalue(co, 1);
    int page_ref = luaL_ref(co, LUA_REGISTRYINDEX);
    QTimer *soon = new QTimer();
    soon->setSingleShot(true);
    kit_connect(soon, &QTimer::timeout, &QTimer::staticMetaObject, rebuild, reinterpret_cast<void *>(static_cast<intptr_t>(page_ref)));
    QObject::connect(soon, &QTimer::timeout, soon, &QObject::deleteLater);
    soon->start(0);
    return 0;
}

void kit_build_page(PagingContainer *paging, int build, bool tab) {
    Page *page = static_cast<Page *>(lua_newuserdatauv(L, sizeof(Page), 0));
    new (page) Page();
    page->paging = paging;
    page->build = build;
    page->tab = tab;
    luaL_setmetatable(L, "nk.page");
    int page_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    build_page(page, page_ref);
    /* The page stays referenced while it's on screen, for page:refresh from a callback. */
    QObject::connect(paging, &QObject::destroyed, [page_ref]() { luaL_unref(L, LUA_REGISTRYINDEX, page_ref); });
}

static void build_screen(NkScreen *screen, void *arg) { kit_build_page(screen->paging, *static_cast<int *>(arg), false); }

static int page_gc(lua_State *co) {
    static_cast<Page *>(lua_touserdata(co, 1))->~Page();
    return 0;
}

/* A screen's build function, kept until the screen has gone for good. */
static void free_screen_build(void *arg) {
    int *build = static_cast<int *>(arg);
    luaL_unref(L, LUA_REGISTRYINDEX, *build);
    delete build;
}

void kit_lua_open_screen(lua_State *co, const char *title, const char *small_title, int build_index) {
    luaL_checktype(co, build_index, LUA_TFUNCTION);
    lua_pushvalue(co, build_index);
    int *build = new int(luaL_ref(co, LUA_REGISTRYINDEX));
    kit_open_screen(title, small_title, build_screen, build, free_screen_build);
}

/* ---- places ---- */

/* A row's or screen watcher's function, with what it needs, kept for as long as Kobo may call it:
 * a screen already showing a row can have it tapped after main.lua has run again. Running main.lua
 * again marks every place gone, and it adds them again. */
struct Place {
    char kind; /* 's' Settings row, 'm' More row, 'w' screen watcher */
    QByteArray name;
    int fn;
    bool gone;
};
static QList<Place *> *places;

static Place *new_place(lua_State *co, char kind, const QByteArray &name, int fn) {
    Place *place = new Place;
    place->kind = kind;
    place->name = name;
    place->fn = fn;
    place->gone = false;
    if (!places) places = new QList<Place *>;
    *places << place;
    (void)co;
    return place;
}

/* Before main.lua runs again. The places themselves are kept, as rows may still point at them. */
static void forget_places() {
    if (!places) return;
    foreach (Place *place, *places) {
        luaL_unref(L, LUA_REGISTRYINDEX, place->fn);
        place->gone = true;
    }
    places->clear();
    kit_forget_rows();
}

/* The place a row stands for now: itself, or after main.lua has run again, the one it added in its
 * place with the same name, or 0 if it didn't. */
static Place *current(Place *place) {
    if (!place->gone) return place;
    if (places)
        foreach (Place *now, *places)
            if (now->kind == place->kind && now->name == place->name) return now;
    return 0;
}

/* The table argument's string field, or 0. */
static const char *opt_field(lua_State *co, const char *name) {
    lua_getfield(co, 1, name);
    const char *value = lua_isnil(co, -1) ? 0 : luaL_checkstring(co, -1);
    lua_pop(co, 1); /* the string stays alive in the table */
    return value;
}

/* A function field of the table argument, referenced. */
static int fn_field(lua_State *co, const char *name, const char *call) {
    lua_getfield(co, 1, name);
    if (!lua_isfunction(co, -1)) luaL_error(co, "%s needs a function for %s", call, name);
    return luaL_ref(co, LUA_REGISTRYINDEX);
}

/* after or before, as the object name of one of Kobo's own rows. */
static const char *beside(lua_State *co, const char *call, bool *after) {
    const char *a = opt_field(co, "after"), *b = opt_field(co, "before");
    if (a && b) luaL_error(co, "%s takes after or before, not both", call);
    *after = a != 0;
    return a ? a : b;
}

static void open_settings(void *arg) {
    if (!ready_to_open()) return;
    Place *place = current(static_cast<Place *>(arg));
    if (place) nk_open_screen(place->name.constData(), "Settings", build_screen, &place->fn);
}

/* nk.settings{name = , build = , after = or before = }: a row in Kobo's Settings, opening a screen
 * built by build(page). after or before name one of Kobo's rows, such as "deviceInformation";
 * without either it goes at the top. A mod can add several. */
static int l_settings(lua_State *co) {
    luaL_checktype(co, 1, LUA_TTABLE);
    const char *name = opt_field(co, "name");
    if (!name) return luaL_error(co, "nk.settings needs a name");
    bool after;
    const char *next_to = beside(co, "nk.settings", &after);
    Place *place = new_place(co, 's', name, fn_field(co, "build", "nk.settings"));
    kit_settings_row(name, next_to, after, open_settings, place);
    return 0;
}

static void open_more(void *arg) {
    if (!ready_to_open()) return;
    Place *place = current(static_cast<Place *>(arg));
    if (place) resume(task(place->fn), 0);
}

/* nk.more{name = , icon = , open = , after = or before = }: a row on Kobo's More page, with
 * icons/<icon>.png, running open(). after or before name one of Kobo's rows, such as
 * "betaFeaturesButton"; without either it goes above Settings. A mod can add several. */
static int l_more(lua_State *co) {
    luaL_checktype(co, 1, LUA_TTABLE);
    const char *name = opt_field(co, "name"), *icon = opt_field(co, "icon");
    if (!name) return luaL_error(co, "nk.more needs a name");
    if (!icon) return luaL_error(co, "nk.more needs an icon");
    const Embedded *png = find_embedded((QByteArray("icons/") + icon + ".png").constData());
    if (!png) return luaL_error(co, "no icons/%s.png", icon);
    bool after;
    const char *next_to = beside(co, "nk.more", &after);
    Place *place = new_place(co, 'm', name, fn_field(co, "open", "nk.more"));
    kit_more_row(name, png->data, png->size, next_to, after, open_more, place);
    return 0;
}

static void screen_built(void *view, void *arg) {
    Place *place = static_cast<Place *>(arg);
    if (place->gone) return;
    lua_State *co = task(place->fn);
    push_handle(co, static_cast<QObject *>(view), "nk.widget");
    resume(co, 1);
}

/* nk.on_screen(class, fn): runs fn(view) each time Nickel makes a widget of one of Kobo's classes,
 * such as "MoreView", once its constructor is done. Only for classes made with a parent widget.
 * view is an nk.widget. Returns false if this firmware has no such class. Call it from main.lua. */
static int l_on_screen(lua_State *co) {
    const char *cls = luaL_checkstring(co, 1);
    luaL_checktype(co, 2, LUA_TFUNCTION);
    lua_pushvalue(co, 2);
    Place *place = new_place(co, 'w', cls, luaL_ref(co, LUA_REGISTRYINDEX));
    lua_pushboolean(co, nk_on_screen(cls, screen_built, place));
    return 1;
}

/* nk.screen{title = , small_title = , build = }: opens a settings-style screen built by
 * build(page), over whatever is showing. small_title is the small line above the title, by default
 * the title of the screen it opens over, or "Settings". */
static int l_screen(lua_State *co) {
    luaL_checktype(co, 1, LUA_TTABLE);
    const char *title = opt_field(co, "title"), *small_title = opt_field(co, "small_title");
    if (!title) return luaL_error(co, "nk.screen needs a title");
    if (!small_title) small_title = kit_top_screen_title();
    lua_getfield(co, 1, "build");
    kit_lua_open_screen(co, title, small_title ? small_title : "Settings", lua_gettop(co));
    lua_pop(co, 1);
    return 0;
}

/* ---- Kobo's own widgets, from nk.on_screen ---- */

static QObject *find_deep(QObject *parent, const QString &name) {
    foreach (QObject *kid, parent->children()) {
        if (kid->objectName() == name) return kid;
        QObject *deeper = find_deep(kid, name);
        if (deeper) return deeper;
    }
    return 0;
}

/* widget:find(name): the widget inside it with that object name, or nil. */
static int widget_find(lua_State *co) {
    QObject *inside = find_deep(check_handle(co, "nk.widget"), QString::fromUtf8(luaL_checkstring(co, 2)));
    if (!inside) return 0;
    push_handle(co, inside, "nk.widget");
    return 1;
}

/* widget:text(): its text, for labels and anything else with a text property. */
static int widget_text(lua_State *co) {
    QObject *widget = check_handle(co, "nk.widget");
    QLabel *label = qobject_cast<QLabel *>(widget);
    QString text = label ? label->text() : widget->property("text").toString();
    lua_pushstring(co, text.toUtf8().constData());
    return 1;
}

/* widget:set(text): changes its text. */
static int widget_set(lua_State *co) {
    QObject *widget = check_handle(co, "nk.widget");
    QString text = QString::fromUtf8(luaL_checkstring(co, 2));
    QLabel *label = qobject_cast<QLabel *>(widget);
    if (label) kit_set_text(label, text);
    else widget->setProperty("text", text);
    return 0;
}

/* widget:show(on): shows or hides it. */
static int widget_show(lua_State *co) {
    QWidget *widget = qobject_cast<QWidget *>(check_handle(co, "nk.widget"));
    if (widget) widget->setVisible(lua_toboolean(co, 2));
    return 0;
}

/* widget:class(): Kobo's name for its class, such as "MoreView". */
static int widget_class(lua_State *co) {
    lua_pushstring(co, check_handle(co, "nk.widget")->metaObject()->className());
    return 1;
}

/* ---- dialogs ---- */

static void answered(lua_State *co, bool yes) {
    lua_pushboolean(co, yes);
    resume(co, 1);
}
static void accepted(void *co) { answered(static_cast<lua_State *>(co), true); }
static void rejected(void *co) { answered(static_cast<lua_State *>(co), false); }

/* nk.confirm(title, text, accept, reject): waits for an answer, true or false. */
static int l_confirm(lua_State *co) {
    must_wait_here(co);
    const char *title = luaL_checkstring(co, 1), *text = luaL_checkstring(co, 2);
    const char *accept = luaL_optstring(co, 3, "OK"), *reject = luaL_optstring(co, 4, "Cancel");
    nk_confirm(title, text, accept, reject, accepted, rejected, co);
    return lua_yield(co, 0);
}

static int l_error(lua_State *co) {
    nk_error(luaL_checkstring(co, 1), luaL_checkstring(co, 2));
    return 0;
}

/* ---- time ---- */

struct Timer {
    QTimer *timer;
    int fn;         /* runs each time, or */
    lua_State *co;  /* resumes once, for nk.sleep */
    int id;
};
static QHash<int, Timer *> *timers;
static int next_timer;

static void stop_timer(Timer *t) {
    timers->remove(t->id);
    t->timer->stop();
    t->timer->deleteLater();
    if (t->fn) luaL_unref(L, LUA_REGISTRYINDEX, t->fn);
    delete t;
}

static void fire(void *arg) {
    Timer *t = static_cast<Timer *>(arg);
    if (t->co) {
        lua_State *co = t->co;
        stop_timer(t);
        resume(co, 0);
        return;
    }
    int repeat = !t->timer->isSingleShot();
    lua_State *co = task(t->fn);
    /* The coroutine holds the function now, so a timer that's done can go before it runs. */
    if (!repeat) stop_timer(t);
    resume(co, 0);
}

static Timer *start_timer(int ms, bool repeat) {
    Timer *t = new Timer();
    t->timer = new QTimer();
    t->timer->setSingleShot(!repeat);
    t->timer->setInterval(ms);
    t->id = ++next_timer;
    timers->insert(t->id, t);
    kit_connect(t->timer, &QTimer::timeout, &QTimer::staticMetaObject, fire, t);
    t->timer->start();
    return t;
}

static int push_timer(lua_State *co, Timer *t) {
    *static_cast<int *>(lua_newuserdatauv(co, sizeof(int), 0)) = t->id;
    luaL_setmetatable(co, "nk.timer");
    return 1;
}

static int timer(lua_State *co, bool repeat) {
    int ms = (int)luaL_checkinteger(co, 1);
    int fn = ref_function(co, 2);
    Timer *t = start_timer(ms, repeat);
    t->fn = fn;
    return push_timer(co, t);
}

static int l_after(lua_State *co) { return timer(co, false); }
static int l_every(lua_State *co) { return timer(co, true); }

static int timer_stop(lua_State *co) {
    int id = *static_cast<int *>(luaL_checkudata(co, 1, "nk.timer"));
    if (timers->contains(id)) stop_timer(timers->value(id));
    return 0;
}

/* nk.go(fn): runs fn on its own, straight after this callback, so several things can wait at once. */
static int l_go(lua_State *co) {
    int fn = ref_function(co, 1);
    start_timer(0, false)->fn = fn;
    return 0;
}

static int l_sleep(lua_State *co) {
    must_wait_here(co);
    int ms = (int)luaL_checkinteger(co, 1);
    start_timer(ms, false)->co = co;
    return lua_yield(co, 0);
}

/* nk.time(): seconds since 1970, in UTC. */
static int l_time(lua_State *co) {
    lua_pushinteger(co, QDateTime::currentMSecsSinceEpoch() / 1000);
    return 1;
}

/* ---- storage ---- */

/* nk.pref(key, default) and nk.set_pref(key, value), in .adds/<mod>/settings.ini: key=value lines. */
static QList<QByteArray> pref_lines() {
    QFile file(QStringLiteral(PREFS));
    if (!file.open(QIODevice::ReadOnly)) return QList<QByteArray>();
    return file.readAll().split('\n');
}

static int l_pref(lua_State *co) {
    const char *key = luaL_checkstring(co, 1);
    bool found = false;
    {
        QByteArray prefix = QByteArray(key) + '=';
        QList<QByteArray> lines = pref_lines();
        for (int i = 0; i < lines.size() && !found; i++)
            if (lines[i].startsWith(prefix)) {
                QByteArray value = lines[i].mid(prefix.size());
                lua_pushlstring(co, value.constData(), value.size());
                found = true;
            }
    }
    if (!found) lua_pushvalue(co, 2);
    return 1;
}

/* Written beside the old file and renamed over it, so a lost write leaves the old one. */
static bool write_whole(const QString &path, const QByteArray &data) {
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path + QStringLiteral(".new"));
    if (!file.open(QIODevice::WriteOnly)) return false;
    bool ok = file.write(data) == data.size() && file.flush() && fsync(file.handle()) == 0;
    file.close();
    if (!ok || rename(QFile::encodeName(file.fileName()).constData(), QFile::encodeName(path).constData())) {
        file.remove();
        return false;
    }
    return true;
}

static int l_set_pref(lua_State *co) {
    const char *key = luaL_checkstring(co, 1), *value = luaL_checkstring(co, 2);
    if (strchr(key, '=') || strchr(key, '\n') || strchr(value, '\n')) return luaL_error(co, "a pref can't hold = in its key or a new line");
    bool ok;
    {
        QByteArray prefix = QByteArray(key) + '=', line = prefix + value;
        QList<QByteArray> lines = pref_lines();
        bool replaced = false;
        for (int i = 0; i < lines.size(); i++)
            if (lines[i].startsWith(prefix)) {
                lines[i] = line;
                replaced = true;
            }
        while (!lines.isEmpty() && lines.last().isEmpty()) lines.removeLast();
        if (!replaced) lines << line;
        QByteArray text;
        for (int i = 0; i < lines.size(); i++) text += lines[i] + '\n';
        ok = write_whole(QStringLiteral(PREFS), text);
    }
    lua_pushboolean(co, ok);
    return 1;
}

/* nk.keep(table): the same table again when main.lua runs again, for state that should survive a
 * reload. Each call is told apart by its script and how many came before it there. */
static int l_keep(lua_State *co) {
    luaL_checktype(co, 1, LUA_TTABLE);
    lua_Debug where;
    if (!lua_getstack(co, 1, &where) || !lua_getinfo(co, "S", &where)) return 1;
    lua_getfield(co, LUA_REGISTRYINDEX, "nk.keepcount");
    lua_Integer n = (lua_getfield(co, -1, where.source), lua_tointeger(co, -1)) + 1;
    lua_pop(co, 1);
    lua_pushinteger(co, n);
    lua_setfield(co, -2, where.source);
    lua_pop(co, 1);
    lua_getfield(co, LUA_REGISTRYINDEX, "nk.kept");
    lua_pushfstring(co, "%s#%d", where.source, (int)n);
    lua_pushvalue(co, -1);
    if (lua_rawget(co, -3) == LUA_TTABLE) return 1;
    lua_pop(co, 1);
    lua_pushvalue(co, 1);
    lua_rawset(co, -3);
    lua_pushvalue(co, 1);
    return 1;
}

/* ---- the rest ---- */

static int l_log(lua_State *co) {
    int n = lua_gettop(co);
    luaL_Buffer out;
    luaL_buffinit(co, &out);
    for (int i = 1; i <= n; i++) {
        if (i > 1) luaL_addchar(&out, ' ');
        luaL_tolstring(co, i, 0);
        luaL_addvalue(&out);
    }
    luaL_pushresult(&out);
    nk_log("%s", lua_tostring(co, -1));
    return 0;
}

static void add_type(const char *name, const luaL_Reg *methods, lua_CFunction gc);

void kit_add_type(const char *type, const luaL_Reg *methods) { add_type(type, methods, handle_gc); }

void kit_add_page_methods(const luaL_Reg *methods) {
    luaL_getmetatable(L, "nk.page");
    lua_getfield(L, -1, "__index");
    luaL_setfuncs(L, methods, 0);
    lua_pop(L, 2);
}

static void add_type(const char *name, const luaL_Reg *methods, lua_CFunction gc) {
    luaL_newmetatable(L, name);
    lua_newtable(L);
    luaL_setfuncs(L, methods, 0);
    lua_setfield(L, -2, "__index");
    if (gc) {
        lua_pushcfunction(L, gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);
}

static int panic(lua_State *co) {
    nk_log("Lua panicked: %s", lua_tostring(co, -1));
    return 0;
}

extern "C" void nk_mod_init(void) {
    tasks = new QHash<lua_State *, int>;
    timers = new QHash<int, Timer *>;
    startup_error = new QByteArray;
    loaded_stamp = new QByteArray;
    L = luaL_newstate();
    if (!L) return;
    lua_atpanic(L, panic);
    static const luaL_Reg libraries[] = {{LUA_GNAME, luaopen_base}, {LUA_COLIBNAME, luaopen_coroutine},
                                         {LUA_TABLIBNAME, luaopen_table}, {LUA_STRLIBNAME, luaopen_string},
                                         {LUA_MATHLIBNAME, luaopen_math}, {LUA_UTF8LIBNAME, luaopen_utf8}};
    for (unsigned i = 0; i < sizeof libraries / sizeof *libraries; i++) {
        luaL_requiref(L, libraries[i].name, libraries[i].func, 1);
        lua_pop(L, 1);
    }
    /* Files go through the kit, so these two aren't here. */
    lua_pushnil(L);
    lua_setglobal(L, "dofile");
    lua_pushnil(L);
    lua_setglobal(L, "loadfile");

    static const luaL_Reg page_methods[] = {{"section", page_section}, {"text", page_text}, {"value", page_value},
                                           {"toggle", page_toggle}, {"button", page_button}, {"refresh", page_refresh}, {0, 0}};
    static const luaL_Reg toggle_methods[] = {{"on", toggle_on}, {"set", toggle_set}, {0, 0}};
    static const luaL_Reg label_methods[] = {{"set", label_set}, {"show", label_show}, {0, 0}};
    static const luaL_Reg timer_methods[] = {{"stop", timer_stop}, {0, 0}};
    add_type("nk.page", page_methods, page_gc);
    add_type("nk.toggle", toggle_methods, handle_gc);
    add_type("nk.label", label_methods, handle_gc);
    static const luaL_Reg widget_methods[] = {{"find", widget_find}, {"text", widget_text}, {"set", widget_set}, {"show", widget_show}, {"class", widget_class}, {0, 0}};
    add_type("nk.widget", widget_methods, handle_gc);
    add_type("nk.timer", timer_methods, 0);

    static const luaL_Reg nk[] = {{"log", l_log}, {"settings", l_settings}, {"more", l_more}, {"on_screen", l_on_screen}, {"screen", l_screen},
                                  {"confirm", l_confirm}, {"error", l_error}, {"after", l_after}, {"every", l_every},
                                  {"sleep", l_sleep}, {"go", l_go}, {"time", l_time}, {"pref", l_pref}, {"set_pref", l_set_pref},
                                  {"keep", l_keep}, {"require", l_require}, {0, 0}};
    luaL_newlib(L, nk);
    kit_open_tools(L);
    kit_open_ui(L);
    lua_pushstring(L, NK_NAME);
    lua_setfield(L, -2, "name");
    lua_setglobal(L, "nk");
    lua_pushcfunction(L, l_require);
    lua_setglobal(L, "require");
    lua_pushcfunction(L, l_log);
    lua_setglobal(L, "print");
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "nk.kept");

    if (dev_mode()) nk_log("scripts from " DEV);
    run_main();
    started = true;
}
