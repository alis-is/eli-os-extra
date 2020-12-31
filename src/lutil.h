#include "lua.h"
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>

int push_error(lua_State *L, const char *info);
int push_result(lua_State *L, int res, const char *info);

#ifdef _WIN32
#include <ctype.h>
#include <windows.h>
int windows_pusherror(lua_State *L, DWORD error, int nresults);
#endif
