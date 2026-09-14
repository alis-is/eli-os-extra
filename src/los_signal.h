#ifndef LUA_OS_EXTRA_SIGNAL_H
#define LUA_OS_EXTRA_SIGNAL_H

#include "lua.h"

int eli_os_signal_open(lua_State *L);

#ifndef _WIN32
#include <signal.h>

/* Signal setters take the environment lock internally. During os.execute,
 * SIGINT/SIGQUIT changes take effect when the last waiter finishes. */
int eli_signal_setsigaction(int signum, const struct sigaction *action);

/* Call under eli_env_lock, including in its forked child (no locking here).
 * Only temporary system() ignores are reset; deliberate ignores survive exec. */
void eli_signal_child_sigdefault(sigset_t *defaults);

/* Call under eli_env_lock. End each successful begin after the child wait.
 * Begin returns an error number, or zero on success. */
int eli_signal_system_begin(void);
void eli_signal_system_end(void);
#endif

#endif // LUA_OS_EXTRA_SIGNAL_H
