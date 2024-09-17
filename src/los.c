#include "lauxlib.h"
#include "lua.h"

#include <string.h>
#include "lcwd.h"
#include "lsleep.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

/*
---#DES 'os.sleep'
---
---Sleep duration seconds (default) or less if divider/unit specified
---@param duration integer
---@param unit_or_divider '"s"' | '"ms"' | integer | nil

*/
static int eli_sleep(lua_State *L)
{
	int duration = (int)luaL_checknumber(L, 1);
	sleep_ms(sleep_duration_to_ms(duration,
				      get_sleep_divider_from_state(L, 2, 1)));
	return 0;
}

static const struct luaL_Reg eliOsExtra[] = {
	{ "sleep", eli_sleep },
	{ "chdir", eli_chdir },
	{ "cwd", eli_cwd },
	{ NULL, NULL },
};

int luaopen_eli_os_extra(lua_State *L)
{
	lua_newtable(L);
	luaL_setfuncs(L, eliOsExtra, 0);
	return 1;
}
