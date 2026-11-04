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
#include <QList>
#include <QPointer>
#include <QTimer>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
extern "C" {
#include "vendor/lua/lua.h"
#include "vendor/lua/lauxlib.h"
#include "vendor/lua/lualib.h"
}
#include "kit.h"

#define FOLDER "/mnt/onboard/.adds/" NK_NAME
#define DEV FOLDER "/dev"
#define PREFS FOLDER "/settings.ini"

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

/* For functions that wait: they need a coroutine the kit can resume. */
static void must_wait_here(lua_State *co) {
    if (!tasks->contains(co) || !lua_isyieldable(co)) luaL_error(co, "this can only wait in a callback");
}

static int ref_function(lua_State *co, int index) {
    luaL_checktype(co, index, LUA_TFUNCTION);
    lua_pushvalue(co, index);
    return luaL_ref(co, LUA_REGISTRYINDEX);
}

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

/* (Re)runs main.lua with fresh modules. */
static void run_main() {
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

/* ---- screens and their blocks ---- */

struct Page {
    QPointer<QWidget> paging;
    int build;
    bool refreshing;
    const char *title, *small_title;
};

static Page *check_page(lua_State *co) {
    Page *page = static_cast<Page *>(luaL_checkudata(co, 1, "nk.page"));
    if (!page->paging) luaL_error(co, "this screen has closed");
    return page;
}

static NkScreen screen_of(Page *page) {
    NkScreen screen = {static_cast<PagingContainer *>(page->paging.data())};
    return screen;
}

/* A callback for a block, with what it needs to run. */
struct Hook {
    int fn;
    QPointer<QObject> widget;
};

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

static int handle_gc(lua_State *co) {
    static_cast<QPointer<QObject> *>(lua_touserdata(co, 1))->~QPointer<QObject>();
    return 0;
}

static int page_section(lua_State *co) {
    Page *page = check_page(co);
    NkScreen s = screen_of(page);
    nk_section(&s, luaL_checkstring(co, 2));
    return 0;
}

static int page_text(lua_State *co) {
    Page *page = check_page(co);
    NkScreen s = screen_of(page);
    nk_text(&s, luaL_checkstring(co, 2));
    return 0;
}

static int page_value(lua_State *co) {
    Page *page = check_page(co);
    NkScreen s = screen_of(page);
    nk_value_row(&s, luaL_checkstring(co, 2), luaL_checkstring(co, 3));
    return 0;
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

/* page:button(title, label, tapped). Returns the row, whose title can change with row:set(text). */
static int page_button(lua_State *co) {
    Page *page = check_page(co);
    const char *title = luaL_checkstring(co, 2), *button = luaL_checkstring(co, 3);
    Hook *hook = new_hook(co, 4);
    NkScreen s = screen_of(page);
    NkLabel *label = nk_button_row(&s, title, button, tapped, hook);
    hook_to(hook, reinterpret_cast<QObject *>(label));
    push_handle(co, hook->widget, "nk.label");
    return 1;
}

static int label_set(lua_State *co) {
    NkLabel *label = reinterpret_cast<NkLabel *>(check_handle(co, "nk.label"));
    nk_label_set(label, luaL_checkstring(co, 2));
    return 0;
}

static void build_page(Page *page, int page_ref) {
    lua_State *co = task(page->build);
    lua_rawgeti(co, LUA_REGISTRYINDEX, page_ref);
    resume(co, 1);
}

/* page:refresh(): the screen is built again by its build function, from the script's own state.
 * The screen is closed and opened again, once the current tap or timer is done, since the widget
 * it came from goes with the screen. Only the screen opened last can be refreshed. */
static void rebuild(void *arg) {
    int page_ref = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    lua_rawgeti(L, LUA_REGISTRYINDEX, page_ref);
    Page *page = static_cast<Page *>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    page->refreshing = false;
    if (page->paging) kit_reopen_screen();
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

static void build_screen(NkScreen *screen, void *arg) {
    int build = *static_cast<int *>(arg);
    Page *page = static_cast<Page *>(lua_newuserdatauv(L, sizeof(Page), 0));
    new (page) Page();
    page->paging = screen->paging;
    page->build = build;
    luaL_setmetatable(L, "nk.page");
    int page_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    build_page(page, page_ref);
    /* The page stays referenced while it's on screen, for page:refresh from a callback. */
    QObject::connect(screen->paging, &QObject::destroyed, [page_ref]() { luaL_unref(L, LUA_REGISTRYINDEX, page_ref); });
}

static int page_gc(lua_State *co) {
    static_cast<Page *>(lua_touserdata(co, 1))->~Page();
    return 0;
}

/* nk.screen(title, small_title, build): opens a settings-style screen built by build(page). A screen
 * loads after it's pushed, so the last few build functions are kept. */
static int screen_builds[8], next_screen;
static int l_screen(lua_State *co) {
    const char *title = luaL_checkstring(co, 1), *small_title = luaL_checkstring(co, 2);
    int *build = &screen_builds[next_screen++ % 8];
    if (*build) luaL_unref(co, LUA_REGISTRYINDEX, *build);
    *build = ref_function(co, 3);
    nk_open_screen(title, small_title, build_screen, build);
    return 0;
}

/* ---- places ---- */

static struct {
    char *title;
    int build;
} settings_place;

static void open_settings(void *) {
    if (!ready_to_open()) return;
    nk_open_screen(settings_place.title, "Settings", build_screen, &settings_place.build);
}

/* nk.settings(title, build): a row at the top of Kobo's Settings, opening a screen built by build. */
static int l_settings(lua_State *co) {
    const char *title = luaL_checkstring(co, 1);
    int build = ref_function(co, 2);
    if (settings_place.build) luaL_unref(co, LUA_REGISTRYINDEX, settings_place.build);
    settings_place.build = build;
    if (!settings_place.title) {
        settings_place.title = strdup(title);
        nk_settings_row(settings_place.title, open_settings, 0);
    }
    return 0;
}

static struct {
    char *title;
    int open;
} more_place;

static void open_more(void *) {
    if (!ready_to_open()) return;
    resume(task(more_place.open), 0);
}

/* nk.more(title, icon, open): a row on the More page, with icons/<icon>.png, running open(). */
static int l_more(lua_State *co) {
    const char *title = luaL_checkstring(co, 1), *icon = luaL_checkstring(co, 2);
    int open = ref_function(co, 3);
    if (more_place.open) luaL_unref(co, LUA_REGISTRYINDEX, more_place.open);
    more_place.open = open;
    if (more_place.title) return 0;
    const Embedded *png = find_embedded((QByteArray("icons/") + icon + ".png").constData());
    if (!png) return luaL_error(co, "no icons/%s.png", icon);
    more_place.title = strdup(title);
    nk_more_row(more_place.title, png->data, png->size, open_more, 0);
    return 0;
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

static int l_sleep(lua_State *co) {
    must_wait_here(co);
    int ms = (int)luaL_checkinteger(co, 1);
    start_timer(ms, false)->co = co;
    return lua_yield(co, 0);
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
    static const luaL_Reg label_methods[] = {{"set", label_set}, {0, 0}};
    static const luaL_Reg timer_methods[] = {{"stop", timer_stop}, {0, 0}};
    add_type("nk.page", page_methods, page_gc);
    add_type("nk.toggle", toggle_methods, handle_gc);
    add_type("nk.label", label_methods, handle_gc);
    add_type("nk.timer", timer_methods, 0);

    static const luaL_Reg nk[] = {{"log", l_log}, {"settings", l_settings}, {"more", l_more}, {"screen", l_screen},
                                  {"confirm", l_confirm}, {"error", l_error}, {"after", l_after}, {"every", l_every},
                                  {"sleep", l_sleep}, {"pref", l_pref}, {"set_pref", l_set_pref}, {"keep", l_keep},
                                  {"require", l_require}, {0, 0}};
    luaL_newlib(L, nk);
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
