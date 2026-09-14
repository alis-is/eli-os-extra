#ifndef ELI_ENV_H
#define ELI_ENV_H

#include "lua.h"

int eli_getenv(lua_State *L);
int eli_setenv(lua_State *L);
int eli_environ(lua_State *L);

/* Process-wide environment lock. All bundled reads/writes/enumeration and
 * subprocess snapshots share it so independent Lua states can observe a
 * consistent environment. */
void eli_env_lock(void);
void eli_env_unlock(void);


/* Route the state's standard os.getenv/os.execute/io.popen through the synchronized
 * implementation. Safe to call repeatedly and from any state. */
void eli_env_install(lua_State *L);

#endif
