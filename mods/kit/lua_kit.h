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

#endif
