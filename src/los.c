#include "lauxlib.h"
#include "lua.h"

#include "lcwd.h"
#include "lsleep.h"
#include "lenv.h"
#include "los_signal.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

/*
---#DES 'os.sleep'
---
---Sleep duration ms (default) or adjusted by unit/divider.
---@param duration integer
---@param unit_or_divider '"s"' | '"ms"' | integer | nil

*/
static int eli_sleep(lua_State *L)
{
	lua_Number duration = luaL_checknumber(L, 1);
	double divider = get_ms_divider_from_state(L, 2, 1.0);
	double final_seconds = (double)duration / divider;

	sleep_ms(final_seconds);

	return 0;
}

static const struct luaL_Reg eliOsExtra[] = {
	{ "sleep", eli_sleep },
	{ "chdir", eli_chdir },
	{ "cwd", eli_cwd },
	{ NULL, NULL },
};

static void eli_os_set_field(lua_State *L, const char *name, int value)
{
	lua_getglobal(L, "os");
	if (lua_istable(L, -1)) {
		lua_pushvalue(L, value);
		lua_setfield(L, -2, name);
	}
	lua_pop(L, 1);
}

int luaopen_eli_os_extra(lua_State *L)
{
	int extra;
	int signal;

	luaL_newlib(L, eliOsExtra);
	extra = lua_gettop(L);

	eli_env_install(L);

	eli_os_signal_open(L);
	signal = lua_gettop(L);
	lua_pushvalue(L, signal);
	lua_setfield(L, extra, "signal");
	eli_os_set_field(L, "signal", signal);
	lua_pop(L, 1);

	return 1;
}
