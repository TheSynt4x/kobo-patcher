/* nk.dictionaries: which dictionary Kobo opens for a book.
 *
 * - DictionaryParser::bestDictionary() picks one from a list of dictionaries, which holds those
 *   Kobo offers for download as well as those on the Kobo. Without the mod, a book in English
 *   looks nothing up when Kobo's English isn't downloaded, even with an English dictionary in
 *   .kobo/custom-dict.
 * - installed_first() hands it the same list with the dictionaries on the Kobo first, Kobo's and
 *   custom alike, in Kobo's order, then the rest. Kobo's own choosing does the rest, so a language
 *   with no dictionary on the Kobo goes on as it would without the mod.
 * - A Dictionary holds a QSharedDataPointer<DictionaryPrivate>, as its exported symbols show: one
 *   pointer. The reordered list holds the same pointers without taking references: the list Kobo
 *   passed holds them for as long as the call lasts, and what Kobo returns takes its own. */
#include <QVector>
#include <dlfcn.h>
#include "kit.h"
#include "lua_kit.h"

typedef void (*BestFn)(void *out, const void *all, const void *book, const void *interface_language, const void *chosen);
typedef bool (*DictionaryCheck)(const void *dictionary);
static BestFn real_best;
static DictionaryCheck installed, synced;

/* Its file is on the Kobo, and not one Kobo is still downloading. */
static bool on_the_kobo(const void *dictionary) { return installed(dictionary) && synced(dictionary); }

static void hooked_best(void *out, const void *all, const void *book, const void *interface_language, const void *chosen) {
    const QVector<void *> &dictionaries = *static_cast<const QVector<void *> *>(all);
    QVector<void *> ordered;
    ordered.reserve(dictionaries.size());
    for (int i = 0; i < dictionaries.size(); i++)
        if (on_the_kobo(&dictionaries.at(i))) ordered << dictionaries.at(i);
    for (int i = 0; i < dictionaries.size(); i++)
        if (!on_the_kobo(&dictionaries.at(i))) ordered << dictionaries.at(i);
    real_best(out, &ordered, book, interface_language, chosen);
}

/* nk.dictionaries.installed_first(): a book's dictionary is one on the Kobo whenever there's one
 * in its language, Kobo's own or one in .kobo/custom-dict, before one only in Kobo's catalogue.
 * The same goes for a dictionary chosen in a book. Returns false if this firmware's dictionaries
 * aren't as expected. Call it from main.lua. */
static int l_installed_first(lua_State *co) {
    if (real_best) {
        lua_pushboolean(co, 1);
        return 1;
    }
    static const char *const best = "_ZN16DictionaryParser14bestDictionaryERK7QVectorI10DictionaryERK7QStringS7_S7_";
    installed = reinterpret_cast<DictionaryCheck>(dlsym(RTLD_DEFAULT, "_ZNK10Dictionary9installedEv"));
    synced = reinterpret_cast<DictionaryCheck>(dlsym(RTLD_DEFAULT, "_ZNK10Dictionary8isSyncedEv"));
    if (!installed || !synced || !dlsym(RTLD_DEFAULT, best)) {
        nk_log("Kobo's dictionaries aren't as expected, so nk.dictionaries.installed_first does nothing");
        lua_pushboolean(co, 0);
        return 1;
    }
    /* real_best is set before the hook goes in, as Kobo may call it straight away. */
    real_best = reinterpret_cast<BestFn>(kit_got(best));
    if (!nk_hook(best, reinterpret_cast<void *>(hooked_best))) {
        real_best = 0;
        lua_pushboolean(co, 0);
        return 1;
    }
    lua_pushboolean(co, 1);
    return 1;
}

void kit_open_dictionaries(lua_State *L) {
    static const luaL_Reg dictionaries[] = {{"installed_first", l_installed_first}, {0, 0}};
    luaL_newlib(L, dictionaries);
    lua_setfield(L, -2, "dictionaries");
}
