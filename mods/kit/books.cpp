/* nk.books: what's in My Books. The languages of the books on the Kobo, and entries a mod adds to
 * My Books' filter menu.
 *
 * - Kobo's filter menu lists LibrarySubFilters::bookFilters(), makes each entry's filter with
 *   createFilter(), names it with displayString() and shortDisplayString(), and saves the one
 *   chosen with serialized(), read back with deserialized(). The kit adds its entries after
 *   Kobo's, with numbers Kobo doesn't use, and answers for those. It leaves the rest to Kobo, or to
 *   another mod's kit that hooked them first.
 * - An entry matches the books in any of its languages. Its filter holds one of Kobo's own
 *   LanguageFilter<Volume> for each, on a copy of LanguageFilter<Volume>'s vtable whose filter asks
 *   them in turn, so the kit never reads a book's fields itself.
 * - The entries come from the mod's function, asked only when Kobo builds the menu or reads back
 *   the saved choice, never while the mod loads: reading the books opens Qt's SQL driver, a plugin.
 * - A choice is saved by its entry's name, so one whose entry or mod has gone is read back as
 *   Kobo's default.
 *
 * As in lua.cpp, nothing raises a Lua error while a Qt value made here is still alive. */
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QLocale>
#include <QObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>
#include <QVariant>
#include <dlfcn.h>
#include <new>
#include <string.h>
#include "kit.h"
#include "lua_kit.h"

struct Entry {
    QString name, short_name;
    QList<QByteArray> languages;
};
static QList<Entry> *entries;
static int provider = LUA_NOREF;

/* Kobo's own filters are numbered from 1 to about 20. Each mod's kit takes its own block. */
static int first_id() {
    static int base;
    if (!base) base = 0x10000 + int(qHash(QByteArray(NK_NAME)) & 0xff) * 0x100;
    return base;
}

static const Entry *entry_of(int id) {
    int at = id - first_id();
    return entries && at >= 0 && at < entries->size() ? &entries->at(at) : 0;
}

static QString saved_name(const QString &name) { return QStringLiteral("nkFilter(" NK_NAME ":") + name + QStringLiteral(")"); }

/* ---- the mod's entries ---- */

static void read_entries(lua_State *co, int index, void *) {
    entries->clear();
    lua_Integer count = luaL_len(co, index);
    for (lua_Integer i = 1; i <= count && i <= 64; i++) {
        if (lua_geti(co, index, i) != LUA_TTABLE) {
            lua_pop(co, 1);
            continue;
        }
        int item = lua_gettop(co);
        Entry entry;
        lua_getfield(co, item, "name");
        if (lua_type(co, -1) == LUA_TSTRING) entry.name = QString::fromUtf8(lua_tostring(co, -1));
        lua_getfield(co, item, "short_name");
        entry.short_name = lua_type(co, -1) == LUA_TSTRING ? QString::fromUtf8(lua_tostring(co, -1)) : entry.name;
        lua_pop(co, 2);
        lua_getfield(co, item, "languages");
        if (lua_istable(co, -1)) {
            lua_Integer languages = luaL_len(co, -1);
            for (lua_Integer j = 1; j <= languages && j <= 64; j++) {
                if (lua_geti(co, -1, j) == LUA_TSTRING) entry.languages << QByteArray(lua_tostring(co, -1));
                lua_pop(co, 1);
            }
        }
        lua_pop(co, 2);
        if (!entry.name.isEmpty() && !entry.languages.isEmpty()) *entries << entry;
    }
}

static void ask_for_entries() {
    if (!entries) entries = new QList<Entry>;
    if (provider == LUA_NOREF) return;
    kit_ask_table(provider, read_entries, 0);
}

/* ---- the filter: any of Kobo's language filters ---- */

typedef bool (*FilterFn)(void *self, const void *volume);
typedef void (*LanguageFilterCtor)(void *self, const QByteArray &language);
enum { VTABLE_SLOTS = 32 };
static void **any_vtable;
static int filter_slot = -1;
static QHash<void *, QList<QObject *> > *parts_of;

static bool any_filter(void *self, const void *volume) {
    QList<QObject *> parts = parts_of->value(self);
    foreach (QObject *part, parts) {
        void **vtable = *reinterpret_cast<void ***>(part);
        if (reinterpret_cast<FilterFn>(vtable[filter_slot])(part, volume)) return true;
    }
    return false;
}

static void drop_parts(void *self) {
    QList<QObject *> parts = parts_of->take(self);
    foreach (QObject *part, parts) delete part;
}

static void *any_destroy(void *self) {
    drop_parts(self);
    static_cast<QObject *>(self)->QObject::~QObject();
    return self;
}

static void any_delete(void *self) {
    any_destroy(self);
    operator delete(self);
}

static LanguageFilterCtor language_filter_ctor() {
    static LanguageFilterCtor ctor;
    if (!ctor) ctor = reinterpret_cast<LanguageFilterCtor>(dlsym(RTLD_DEFAULT, "_ZN14LanguageFilterI6VolumeEC1ERK10QByteArray"));
    if (!ctor) ctor = reinterpret_cast<LanguageFilterCtor>(dlsym(RTLD_DEFAULT, "_ZN14LanguageFilterI6VolumeEC2ERK10QByteArray"));
    return ctor;
}

static QObject *new_language_filter(const QByteArray &language) {
    LanguageFilterCtor ctor = language_filter_ctor();
    if (!ctor) return 0;
    void *part = operator new(0x40); /* 0xc on 4.38 */
    memset(part, 0, 0x40);
    ctor(part, language);
    return static_cast<QObject *>(part);
}

/* The vtable is LanguageFilter<Volume>'s, learned from one, with its filter and destructors ours,
 * and FilterBase<Volume>'s type, as the filter isn't one of a single language. */
static bool learn_vtable() {
    if (any_vtable) return true;
    void *filter = dlsym(RTLD_DEFAULT, "_ZN14LanguageFilterI6VolumeE6filterERKS0_");
    void *destroy = dlsym(RTLD_DEFAULT, "_ZN14LanguageFilterI6VolumeED1Ev");
    void *deleting = dlsym(RTLD_DEFAULT, "_ZN14LanguageFilterI6VolumeED0Ev");
    void *base_type = dlsym(RTLD_DEFAULT, "_ZTI10FilterBaseI6VolumeE");
    QObject *sample = filter && destroy && deleting ? new_language_filter("en") : 0;
    if (!sample) {
        nk_log("My Books' filters aren't as expected, so nk.books.filters adds nothing");
        return false;
    }
    void **vtable = *reinterpret_cast<void ***>(sample);
    void **copy = static_cast<void **>(malloc((VTABLE_SLOTS + 2) * sizeof(void *)));
    memcpy(copy, vtable - 2, (VTABLE_SLOTS + 2) * sizeof(void *));
    copy += 2;
    int found = 0;
    for (int i = 0; i < VTABLE_SLOTS; i++) {
        if (copy[i] == filter) copy[i] = reinterpret_cast<void *>(any_filter), filter_slot = i, found++;
        else if (copy[i] == destroy) copy[i] = reinterpret_cast<void *>(any_destroy), found++;
        else if (copy[i] == deleting) copy[i] = reinterpret_cast<void *>(any_delete), found++;
    }
    delete sample;
    if (found != 3 || filter_slot < 0) {
        nk_log("LanguageFilter isn't as expected, so nk.books.filters adds nothing");
        free(copy - 2);
        return false;
    }
    if (base_type) copy[-1] = base_type;
    any_vtable = copy;
    if (!parts_of) parts_of = new QHash<void *, QList<QObject *> >;
    return true;
}

static void *new_any_filter(const Entry &entry) {
    if (!learn_vtable()) return 0;
    QList<QObject *> parts;
    foreach (const QByteArray &language, entry.languages) {
        QObject *part = new_language_filter(language);
        if (part) parts << part;
    }
    void *self = operator new(0x40);
    memset(self, 0, 0x40);
    new (self) QObject(0);
    *static_cast<void ***>(self) = any_vtable;
    parts_of->insert(self, parts);
    return self;
}

/* ---- Kobo's filter menu ---- */

/* An enum, as Kobo's is: Qt keeps a QList of enums as pointers to each, and a QList of ints in
 * place, so a list of ints would read as nonsense to Kobo. */
enum LibrarySubFilter { NO_FILTER = 0 };
typedef void (*ListFn)(QList<LibrarySubFilter> *out);
typedef void *(*CreateFn)(int id);
typedef void (*NameFn)(QString *out, int id);
typedef int (*FromNameFn)(const QString &saved);
static ListFn real_book_filters;
static CreateFn real_create_filter;
static NameFn real_display, real_short_display, real_serialized;
static FromNameFn real_deserialized;

static void hooked_book_filters(QList<LibrarySubFilter> *out) {
    real_book_filters(out);
    ask_for_entries();
    for (int i = 0; i < entries->size(); i++) *out << LibrarySubFilter(first_id() + i);
}

static bool ours(int id) { return id >= first_id() && id < first_id() + 0x100; }

/* Kobo uses the filter without checking it, so one of ours is never null: an entry that has gone
 * matches nothing. */
static void *hooked_create_filter(int id) {
    if (!ours(id)) return real_create_filter(id);
    const Entry *entry = entry_of(id);
    void *made = new_any_filter(entry ? *entry : Entry());
    return made ? made : real_create_filter(1);
}

static void hooked_display(QString *out, int id) {
    const Entry *entry = entry_of(id);
    if (entry) new (out) QString(entry->name);
    else real_display(out, id);
}

static void hooked_short_display(QString *out, int id) {
    const Entry *entry = entry_of(id);
    if (entry) new (out) QString(entry->short_name);
    else real_short_display(out, id);
}

static void hooked_serialized(QString *out, int id) {
    const Entry *entry = entry_of(id);
    if (entry) new (out) QString(saved_name(entry->name));
    else real_serialized(out, id);
}

/* What Kobo reads back may hold more than the filter, as Kobo's own does, so ours is looked for
 * inside it. */
static int hooked_deserialized(const QString &saved) {
    QString ours = QStringLiteral("nkFilter(" NK_NAME ":");
    int at = saved.indexOf(ours);
    if (at < 0) return real_deserialized(saved);
    int start = at + ours.size(), end = saved.indexOf(QLatin1Char(')'), start);
    QString name = saved.mid(start, end < 0 ? -1 : end - start);
    ask_for_entries();
    for (int i = 0; i < entries->size(); i++)
        if (name == entries->at(i).name) return first_id() + i;
    /* An entry that's gone: what Kobo reads back for a filter it doesn't know. */
    return real_deserialized(QString());
}

static bool hook_filters() {
    if (real_book_filters) return true;
    static const char *const symbols[] = {
        "_ZN17LibrarySubFilters11bookFiltersEv", "_ZN17LibrarySubFilters12createFilterE16LibrarySubFilter",
        "_ZN17LibrarySubFilters13displayStringE16LibrarySubFilter", "_ZN17LibrarySubFilters18shortDisplayStringE16LibrarySubFilter",
        "_ZN17LibrarySubFilters10serializedE16LibrarySubFilter", "_ZN17LibrarySubFilters12deserializedERK7QString",
    };
    for (size_t i = 0; i < sizeof symbols / sizeof *symbols; i++) {
        if (dlsym(RTLD_DEFAULT, symbols[i])) continue;
        nk_log("My Books' filters aren't as expected, so nk.books.filters adds nothing");
        return false;
    }
    if (!language_filter_ctor()) {
        nk_log("there's no LanguageFilter on this firmware, so nk.books.filters adds nothing");
        return false;
    }
    /* All of them or none, so no entry is half Kobo's. */
    void *originals[6];
    void *const ours[6] = {reinterpret_cast<void *>(hooked_book_filters), reinterpret_cast<void *>(hooked_create_filter),
                           reinterpret_cast<void *>(hooked_display), reinterpret_cast<void *>(hooked_short_display),
                           reinterpret_cast<void *>(hooked_serialized), reinterpret_cast<void *>(hooked_deserialized)};
    for (int i = 5; i >= 0; i--) originals[i] = nk_hook(symbols[i], ours[i]);
    real_create_filter = reinterpret_cast<CreateFn>(originals[1]);
    real_display = reinterpret_cast<NameFn>(originals[2]);
    real_short_display = reinterpret_cast<NameFn>(originals[3]);
    real_serialized = reinterpret_cast<NameFn>(originals[4]);
    real_deserialized = reinterpret_cast<FromNameFn>(originals[5]);
    /* The menu only shows the entries once this is set, after the rest. */
    real_book_filters = reinterpret_cast<ListFn>(originals[0]);
    if (!entries) entries = new QList<Entry>;
    return true;
}

/* ---- Lua ---- */

/* nk.books.filters(fn): entries for My Books' filter menu, after Kobo's. fn() returns a list of
 * {name = , short_name = , languages = {...}}: name is shown in the menu, short_name (by default the
 * name) once it's chosen, and the entry shows the books whose language is any of languages, as Kobo
 * stores them, such as "ru" or "ru-RU". fn is asked each time Kobo builds the menu or reads back the
 * choice it saved, never while the mod loads, and has to answer straight away. Returns false if
 * this firmware's filters aren't as expected. Call it from main.lua; calling it again replaces fn. */
static int l_filters(lua_State *co) {
    luaL_checktype(co, 1, LUA_TFUNCTION);
    if (!hook_filters()) {
        lua_pushboolean(co, 0);
        return 1;
    }
    if (provider != LUA_NOREF) luaL_unref(co, LUA_REGISTRYINDEX, provider);
    lua_pushvalue(co, 1);
    provider = luaL_ref(co, LUA_REGISTRYINDEX);
    lua_pushboolean(co, 1);
    return 1;
}

/* nk.books.languages(): the languages of the books on the Kobo, each once, as Kobo stores them
 * from the books themselves, such as "ru", "ru-RU" or "rus". Books without one are left out. Not
 * while the mod loads: from a callback, or from nk.books.filters' fn. */
static int l_languages(lua_State *co) {
    QList<QByteArray> found;
    QString connection = QStringLiteral("nk-books-" NK_NAME);
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(QStringLiteral("/mnt/onboard/.kobo/KoboReader.sqlite"));
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (db.open()) {
            QSqlQuery query(db);
            if (query.exec(QStringLiteral("SELECT DISTINCT Language FROM content WHERE ContentType = 6 AND Language IS NOT NULL AND Language != ''")))
                while (query.next()) found << query.value(0).toString().toUtf8();
            else nk_log("couldn't read the books' languages");
        } else nk_log("couldn't open Kobo's database to read the books' languages");
    }
    QSqlDatabase::removeDatabase(connection);
    lua_createtable(co, found.size(), 0);
    for (int i = 0; i < found.size(); i++) {
        lua_pushlstring(co, found[i].constData(), found[i].size());
        lua_rawseti(co, -2, i + 1);
    }
    return 1;
}

void kit_open_books(lua_State *L) {
    static const luaL_Reg books[] = {{"filters", l_filters}, {"languages", l_languages}, {0, 0}};
    luaL_newlib(L, books);
    lua_setfield(L, -2, "books");
}
