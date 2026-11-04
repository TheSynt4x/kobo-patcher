/* What lua.cpp shares with the kit's other Lua files. */
#ifndef NK_LUA_KIT_H
#define NK_LUA_KIT_H

extern "C" {
#include "vendor/lua/lua.h"
#include "vendor/lua/lauxlib.h"
}

#define NK_FOLDER "/mnt/onboard/.adds/" NK_NAME

/* The mod's Lua state. */
lua_State *kit_lua();
/* A coroutine that will run the function the reference `fn` holds. Push its arguments, then resume. */
lua_State *kit_task(int fn);
/* Run a coroutine until it ends or waits again, with `args` values pushed on it. */
void kit_resume(lua_State *co, int args);
/* Raises an error unless co is a callback's coroutine that can wait. */
void kit_must_wait(lua_State *co);
/* A reference to the function at `index`. */
int kit_ref(lua_State *co, int index);

/* tools.cpp: adds the tools to the nk table on top of the stack. */
void kit_open_tools(lua_State *L);
/* patch.cpp: adds nk.patch the same way. */
void kit_open_patch(lua_State *L);
/* ui.cpp: adds nk.tab and nk.toast the same way, and its blocks to pages. */
void kit_open_ui(lua_State *L);

/* ---- pages, shared by lua.cpp and ui.cpp ---- */

class QObject;
class QString;
class QLabel;
class QWidget;
class PagingContainer;

/* A callback for a widget: the function, freed when the widget goes. */
struct KitHook;
KitHook *kit_hook(lua_State *co, int index, QObject *widget);
/* A coroutine for the hook's function, to push arguments on and resume. 0 if the widget has gone. */
lua_State *kit_hook_task(KitHook *hook);

/* The page that's argument 1, or a Lua error if its screen has closed. */
PagingContainer *kit_check_page(lua_State *co);
/* A handle on a widget for Lua, of a type made with kit_add_type, which goes stale with the widget. */
void kit_push_handle(lua_State *co, QObject *widget, const char *type);
QObject *kit_check_handle(lua_State *co, int index, const char *type);
void kit_add_type(const char *type, const luaL_Reg *methods);
/* Adds methods to pages. */
void kit_add_page_methods(const luaL_Reg *methods);
/* Runs build(page) for a page whose blocks go in paging. A tab's page is refreshed by opening the
 * tab again, a screen's by opening the screen again. */
void kit_build_page(PagingContainer *paging, int build, bool tab);
/* Before a place opens: false, with the error shown, if the mod couldn't start. In development it
 * runs main.lua again first if a script has changed. */
bool kit_ready_to_open();
/* One of the mod's own files, built into the .so: a script, or icons/<name>.png. */
bool kit_embedded(const char *name, const unsigned char **data, unsigned *size);
/* Opens a settings-style screen built by the function at build_index, kept until it has gone. */
void kit_lua_open_screen(lua_State *co, const char *title, const char *small_title, int build_index);
/* ui.cpp: opens the tab that's showing again, built afresh. */
void kit_reopen_tab();
/* ui.cpp: if one of the mod's tabs is showing, back to its first page, built afresh. False if none is. */
bool kit_reload_tab();

/* ui.cpp: a label's text, for Kobo's elided labels too. */
void kit_set_text(QLabel *label, const QString &text);

/* This Kobo's model, from the start of its serial, like "N306", and its firmware, like
 * "4.38.23852". Empty when .kobo/version can't be read. */
const char *kit_model();
const char *kit_firmware();

#endif
