#include "lauxlib.h"
#include "lua.h"

#include <string.h>
#include "lcwd.h"

#ifdef _WIN32
#include <windows.h>

#define _lsleep          Sleep
#define SLEEP_MULTIPLIER 1e3
#else
#include <unistd.h>

#define _lsleep          usleep
#define SLEEP_MULTIPLIER 1e6
#endif

/*
---#DES 'os.sleep'
---
---Sleep n secods or less if divider specified
---@param n integer
---@param divider integer?

seconds --
interval units --
*/
static int
eli_sleep(lua_State* L) {
    lua_Number interval = luaL_checknumber(L, 1);
    lua_Number units = luaL_optnumber(L, 2, 1);
    _lsleep(SLEEP_MULTIPLIER * interval / units);
    return 0;
}

static const struct luaL_Reg eliOsExtra[] = {
    {"sleep", eli_sleep},
    {"chdir", eli_chdir},
    {"cwd", eli_cwd},
    {NULL, NULL},
};

int
luaopen_eli_os_extra(lua_State* L) {
    lua_newtable(L);
    luaL_setfuncs(L, eliOsExtra, 0);
    return 1;
}
