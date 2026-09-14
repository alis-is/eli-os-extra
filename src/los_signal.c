#include "lauxlib.h"
#include "llimits.h"
#include "lua.h"

#include <signal.h>
#include <string.h>
#include <stdlib.h>

#include "c11threads.h"
#include "eruntime.h"
#include "lcwd.h"
#include "lenv.h"
#include "los_signal.h"
#include <errno.h>
#include "lerror.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

/*
** ===============================================================
** GLOBAL SIGNAL STATE
** ===============================================================
*/

#ifdef _WIN32
static CRITICAL_SECTION SignalCriticalSection;
static once_flag signal_once = ONCE_FLAG_INIT;
static int subscribedCtrlEvents = 0;

static void signal_runtime_init(void)
{
	InitializeCriticalSection(&SignalCriticalSection);
}
#endif

#ifndef _WIN32
static unsigned system_waiters;
static struct sigaction system_int, system_quit;

int eli_signal_setsigaction(int signum, const struct sigaction *action)
{
	int result = 0, error;
	eli_env_lock();
	if (system_waiters && (signum == SIGINT || signum == SIGQUIT)) {
		*(signum == SIGINT ? &system_int : &system_quit) = *action;
	} else {
		result = sigaction(signum, action, NULL);
	}
	error = errno;
	eli_env_unlock();
	errno = error;
	return result;
}

void eli_signal_child_sigdefault(sigset_t *defaults)
{
	sigemptyset(defaults);
	if (system_waiters) {
		if (system_int.sa_handler != SIG_IGN) sigaddset(defaults, SIGINT);
		if (system_quit.sa_handler != SIG_IGN) sigaddset(defaults, SIGQUIT);
	}
}

/* SIGINT/SIGQUIT dispositions are process-wide. Overlapping os.execute
 * calls share the saved dispositions until the last child wait finishes. */
int eli_signal_system_begin(void)
{
	if (system_waiters == 0) {
		struct sigaction ignore = { 0 };
		ignore.sa_handler = SIG_IGN;
		sigemptyset(&ignore.sa_mask);
		if (sigaction(SIGINT, &ignore, &system_int) == -1) return errno;
		if (sigaction(SIGQUIT, &ignore, &system_quit) == -1) {
			int error = errno;
			sigaction(SIGINT, &system_int, NULL);
			return error;
		}
	}
	system_waiters++;
	return 0;
}

void eli_signal_system_end(void)
{
	if (--system_waiters == 0) {
		sigaction(SIGINT, &system_int, NULL);
		sigaction(SIGQUIT, &system_quit, NULL);
	}
}
#endif

#define SIGNAL_QUEUE_MAX 50

static const char ELI_SIGNAL_IGNORE = 0;

// The "Flag": An atomic indicator that work is waiting.
// Checked by the Lua hook. Set by C signal handlers.
static volatile sig_atomic_t g_signal_pending = 0;

static volatile sig_atomic_t g_queue_count = 0;
static volatile sig_atomic_t g_signal_queue[SIGNAL_QUEUE_MAX];
#ifdef _WIN32
static volatile sig_atomic_t g_kind_queue[SIGNAL_QUEUE_MAX];
#endif

/* Per-state registry table holding Lua callback functions. Keeping it in the
 * state's registry avoids cross-state reference collisions; only the main
 * state may register handlers anyway. */
#define ELI_SIGNAL_HANDLERS_KEY "eli.os.signal.handlers"

static void push_handlers(lua_State *L)
{
	lua_getfield(L, LUA_REGISTRYINDEX, ELI_SIGNAL_HANDLERS_KEY);
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushvalue(L, -1);
		lua_setfield(L, LUA_REGISTRYINDEX, ELI_SIGNAL_HANDLERS_KEY);
	}
}

static int signal_require_main_state(lua_State *L, const char *operation)
{
	if (!eli_runtime_is_main_state(L)) {
		return luaL_error(
		   L, "os.signal.%s is only available in the main state", operation);
	}
	return 0;
}

/*
** ===============================================================
** C-SIDE HANDLERS (Async-Safe / Thread-Safe)
** ===============================================================
*/

// Adds a signal to the queue safely.
// On POSIX: Called from Signal Context (interrupt).
// On Windows: Called from a separate Thread.
static void enqueue_signal(int signum, int kind)
{
	// Acquire Lock (Windows only)
#ifdef _WIN32
	EnterCriticalSection(&SignalCriticalSection);
#else
	// POSIX note: We rely on the atomicity of sig_atomic_t and the fact that
	// standard_signal_handler interrupts the main thread linearly.
#endif

	// Add to Queue
	if (g_queue_count < SIGNAL_QUEUE_MAX) {
		g_signal_queue[g_queue_count] = signum;
#ifdef _WIN32
		g_kind_queue[g_queue_count] = kind;
#endif
		g_queue_count++;

		// Set the global flag so Lua knows to check the queue
		g_signal_pending = 1;
	}

	// Release Lock
#ifdef _WIN32
	LeaveCriticalSection(&SignalCriticalSection);
#endif
}

#ifdef _WIN32
// WINDOWS: Runs on a separate thread created by the OS.
// NO LUA API CALLS ALLOWED HERE.
BOOL WINAPI windows_ctrl_handler(DWORD signum)
{
	int is_subscribed = 0;

	EnterCriticalSection(&SignalCriticalSection);
	if (subscribedCtrlEvents & (1 << signum)) {
		is_subscribed = 1;
	}
	LeaveCriticalSection(&SignalCriticalSection);

	// If we are not subscribed to this specific event, return FALSE.
	// This allows the next handler (or the system default) to process it.
	// e.g. If user handles SIGTERM but not SIGINT, Ctrl+C will now correctly terminate the app.
	if (!is_subscribed) {
		return FALSE;
	}

	int mapped_sig = SIGTERM;
	switch (signum) {
	case CTRL_C_EVENT:
		mapped_sig = SIGINT;
		break;
	case CTRL_BREAK_EVENT:
		mapped_sig = SIGBREAK;
		break;
	case CTRL_CLOSE_EVENT:
		mapped_sig = SIGTERM;
		break;
	case CTRL_LOGOFF_EVENT:
		mapped_sig = SIGTERM;
		break;
	case CTRL_SHUTDOWN_EVENT:
		mapped_sig = SIGTERM;
		break;
	}
	enqueue_signal(mapped_sig, 1);
	return TRUE;
}
#endif

// POSIX: Runs in the same thread, interrupting execution.
// this include windows signal handlers
void standard_signal_handler(int signum)
{
	enqueue_signal(signum, 0);
}

/*
** ===============================================================
** LUA HOOK
** ===============================================================
*/

// This hook runs every N instructions inside Lua.
// It checks the atomic flag. If set, it drains the queue.
static void check_signal_hook(lua_State *L, lua_Debug *ar)
{
	(void)ar; // Unused

	// Capture the queue safely into local variables
	int count = 0;
	int queued_sigs[SIGNAL_QUEUE_MAX];
#ifdef _WIN32
	int queued_kinds[SIGNAL_QUEUE_MAX];

	EnterCriticalSection(&SignalCriticalSection);
	// Fast exit if nothing to do (under the lock: the flag is cross-thread)
	if (!g_signal_pending) {
		LeaveCriticalSection(&SignalCriticalSection);
		return;
	}
	count = g_queue_count;
	if (count > 0) {
		memcpy(queued_sigs, (void *)g_signal_queue,
		       sizeof(int) * count);
		memcpy(queued_kinds, (void *)g_kind_queue, sizeof(int) * count);
		g_queue_count = 0;
		g_signal_pending = 0; // Clear flag inside lock
	}
	LeaveCriticalSection(&SignalCriticalSection);
#else
	// Fast exit if nothing to do (very cheap check)
	if (!g_signal_pending) {
		return;
	}

	// POSIX: We must block signals while reading the queue to prevent
	// a signal handler from modifying it while we copy.
	sigset_t mask, old_mask;
	sigfillset(&mask);
	sigprocmask(SIG_BLOCK, &mask, &old_mask);

	count = g_queue_count;
	if (count > 0) {
		memcpy(queued_sigs, (void *)g_signal_queue,
		       sizeof(int) * count);
		g_queue_count = 0;
		g_signal_pending = 0;
	}

	sigprocmask(SIG_SETMASK, &old_mask, NULL);
#endif

	// Dispatch to Lua Handlers
	if (count > 0) {
		// We need the handlers table
		push_handlers(L);

		for (int i = 0; i < count; i++) {
			int sig = queued_sigs[i];

			lua_rawgeti(L, -1, sig); // Push function
			if (lua_isnil(L, -1)) {
				lua_pop(L, 1);
				continue;
			}

			lua_pushinteger(L, sig);
#ifdef _WIN32
			lua_pushboolean(L, queued_kinds[i]);
#else
			lua_pushboolean(L, 0);
#endif
			// Pcall: If handler errors, we log and continue
			if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
				const char *err = lua_tostring(L, -1);
				fprintf(stderr,
					"[os.signal] Error in handler: %s\n",
					err);
				lua_pop(L, 1);
			}
		}
		lua_pop(L, 1); // Pop registry table
	}
}

/*
** ===============================================================
** LUA API
** ===============================================================
*/

// Set the polling interval (hook count).
// Default is usually sufficient (2000), but tight loops might need tuning.
static int eli_os_signal_poll(lua_State *L)
{
	if (signal_require_main_state(L, "poll")) {
		return 0; /* not reached; luaL_error long jumps */
	}
	int count = (int)luaL_checkinteger(L, 1);
	if (count <= 0)
		count = 2000;

	// Reset the hook with the new count
	lua_sethook(L, check_signal_hook, LUA_MASKCOUNT, count);
	return 0;
}

static int eli_os_signal_handle(lua_State *L)
{
	if (signal_require_main_state(L, "handle")) {
		return 0; /* not reached; luaL_error long jumps */
	}

	// Check if the 2nd arg is our IGNORE atom
	int is_ignore = 0;
	if (lua_type(L, 2) == LUA_TLIGHTUSERDATA) {
		if (lua_touserdata(L, 2) == (void *)&ELI_SIGNAL_IGNORE) {
			is_ignore = 1;
		}
	}
	// If ignoring and the signal number is nil, just return (no-op)
	if (is_ignore && lua_isnil(L, 1)) {
		return 0;
	}

	int signum = (int)luaL_checkinteger(L, 1);
	// If it's NOT ignore, it MUST be a function
	if (!is_ignore) {
		luaL_checktype(L, 2, LUA_TFUNCTION);
	}

#ifdef _WIN32
	int event = -1;
	switch (signum) {
	case SIGINT:
		event = CTRL_C_EVENT;
		break;
	case SIGBREAK:
		event = CTRL_BREAK_EVENT;
		break;
	case SIGTERM:
		event = CTRL_CLOSE_EVENT;
		break;
	}
#endif

	if (is_ignore) {
		// Remove any existing Lua handler for this signal so we don't leak memory
		push_handlers(L);
		lua_pushvalue(L, 1); // Key: signum
		lua_pushnil(L); // Value: nil (removes entry)
		lua_rawset(L, -3);
		lua_pop(L, 1);

#ifdef _WIN32
		EnterCriticalSection(&SignalCriticalSection);
		if (event > -1 && (subscribedCtrlEvents & (1 << event))) {
			subscribedCtrlEvents &= ~(1 << event);

			// If we are no longer listening to ANY Windows events, remove the handler entirely
			if (subscribedCtrlEvents == 0) {
				SetConsoleCtrlHandler(windows_ctrl_handler,
						      FALSE);
			}
		}
		LeaveCriticalSection(&SignalCriticalSection);

		// On Windows, 'signal(SIG_IGN)' works for CRT signals like SIGINT.
		// For Ctrl handlers, we might simply NOT handle it in our C handler,
		// letting the default Windows behavior take over, or explicitly return FALSE.
		// But commonly, setting SIG_IGN on the CRT level is sufficient for CLI tools.
		if (signal(signum, SIG_IGN) == SIG_ERR) {
			return push_error(L, "failed to set signal to ignore");
		}
#else
		struct sigaction sa;
		sa.sa_handler = SIG_IGN;
		sa.sa_flags = 0;
		sigemptyset(&sa.sa_mask);
		if (eli_signal_setsigaction(signum, &sa) == -1) {
			return push_error(L, "failed to set signal to ignore");
		}
#endif
		// We are done. We do NOT enable the hook because no Lua code needs to run.
		return 0;
	}

	// Register OS Handler
#ifdef _WIN32
	if (event > -1) {
		EnterCriticalSection(&SignalCriticalSection);
		if (subscribedCtrlEvents == 0) {
			if (!SetConsoleCtrlHandler(windows_ctrl_handler,
						   TRUE)) {
				LeaveCriticalSection(&SignalCriticalSection);
				return push_error(
					L,
					"failed to set windows ctrl handler");
			}
		}
		subscribedCtrlEvents |= (1 << event);
		LeaveCriticalSection(&SignalCriticalSection);
	}
	// Also set CRT handler for completeness (SIGINT/SIGTERM)
	if (signal(signum, standard_signal_handler) == SIG_ERR) {
		return push_error(L, "failed to set signal handler");
	}
#else
	struct sigaction sa;
	sa.sa_handler = standard_signal_handler;
	sa.sa_flags = SA_RESTART;
	/* Block every signal while enqueueing so a second handled signal cannot
	 * interleave the bounds-check/store/increment sequence. */
	sigfillset(&sa.sa_mask);
	if (eli_signal_setsigaction(signum, &sa) == -1) {
		return push_error(L, "failed to set signal handler");
	}
#endif

	// Store Lua Callback in the state registry
	push_handlers(L);
	lua_pushvalue(L, 1); // Key: signum
	lua_pushvalue(L, 2); // Value: function
	lua_rawset(L, -3);
	lua_pop(L, 1);

	// Ensure the Polling Hook is active!.
	// We default to checking every 2000 instructions.
	// This is low overhead but responsive enough for signals.
	if (lua_gethookmask(L) & LUA_MASKCOUNT) {
		// A hook already exists. We assume it is ours or compatible.
		// If there are other hooks, they might override ours.
	} else {
		lua_sethook(L, check_signal_hook, LUA_MASKCOUNT, 2000);
	}

	return 0;
}

static int eli_os_signal_reset(lua_State *L)
{
	if (signal_require_main_state(L, "reset")) {
		return 0; /* not reached; luaL_error long jumps */
	}
	int signum = (int)luaL_checkinteger(L, 1);

	// Reset OS Handler
#ifdef _WIN32
	int event = -1;
	switch (signum) {
	case SIGINT:
		event = CTRL_C_EVENT;
		break;
	case SIGBREAK:
		event = CTRL_BREAK_EVENT;
		break;
	case SIGTERM:
		event = CTRL_CLOSE_EVENT;
		break;
	}
	EnterCriticalSection(&SignalCriticalSection);
	if (event > -1 && subscribedCtrlEvents > 0) {
		subscribedCtrlEvents &= ~(1 << event);
		if (subscribedCtrlEvents == 0) {
			SetConsoleCtrlHandler(windows_ctrl_handler, FALSE);
		}
	}
	LeaveCriticalSection(&SignalCriticalSection);
	if (signal(signum, SIG_DFL) == SIG_ERR) {
		return push_error(L, "failed to reset signal handler");
	}
#else
	struct sigaction sa = { 0 };
	sa.sa_handler = SIG_DFL;
	sigemptyset(&sa.sa_mask);
	if (eli_signal_setsigaction(signum, &sa) == -1) {
		return push_error(L, "failed to reset signal handler");
	}
#endif

	// Remove Lua Callback
	push_handlers(L);
	lua_pushvalue(L, 1);
	lua_pushnil(L);
	lua_rawset(L, -3);
	lua_pop(L, 1);

	return 0;
}

static int eli_os_signal_handlers(lua_State *L)
{
	push_handlers(L); /* handlers */
	lua_newtable(L);  /* copy */
	lua_pushnil(L);
	while (lua_next(L, -3) != 0) {
		lua_pushvalue(L, -2); /* key */
		lua_pushvalue(L, -2); /* value */
		lua_rawset(L, -5);    /* copy[key] = value */
		lua_pop(L, 1);
	}
	lua_remove(L, -2); /* drop handlers, keep copy */
	return 1;
}

static int eli_os_signal_raise(lua_State *L)
{
	int signum = (int)luaL_checkinteger(L, 1);
	if (signum != 0 && signal_require_main_state(L, "raise")) {
		return 0; /* not reached; luaL_error long jumps */
	}
#ifdef _WIN32
	int res = raise(signum);
#else
	/* process-directed so the signal is delivered to the main thread; worker
	 * threads keep handled signals blocked */
	int res = kill(getpid(), signum);
#endif
	lua_pushboolean(L, res == 0);
	return 1;
}

static const struct luaL_Reg eliOsSignal[] = {
	{ "handle", eli_os_signal_handle },
	{ "reset", eli_os_signal_reset },
	{ "handlers", eli_os_signal_handlers },
	{ "raise", eli_os_signal_raise },
	{ "poll", eli_os_signal_poll }, // New function to tune hook speed
	{ NULL, NULL },
};

int eli_os_signal_open(lua_State *L)
{
#ifdef _WIN32
	call_once(&signal_once, signal_runtime_init);
#endif
	push_handlers(L);
	lua_pop(L, 1);

	lua_newtable(L);
	luaL_setfuncs(L, eliOsSignal, 0);

	lua_pushlightuserdata(L, (void *)&ELI_SIGNAL_IGNORE);
	lua_setfield(L, -2, "IGNORE_SIGNAL");

	// Constants
	lua_pushinteger(L, SIGTERM);
	lua_setfield(L, -2, "SIGTERM");
	lua_pushinteger(L, 9);
	lua_setfield(L, -2, "SIGKILL");
	lua_pushinteger(L, SIGINT);
	lua_setfield(L, -2, "SIGINT");
#ifndef _WIN32
	lua_pushinteger(L, SIGPIPE);
	lua_setfield(L, -2, "SIGPIPE");
	lua_pushinteger(L, SIGUSR1);
	lua_setfield(L, -2, "SIGUSR1");
	lua_pushinteger(L, SIGUSR2);
	lua_setfield(L, -2, "SIGUSR2");
#else
	lua_pushinteger(L, 21);
	lua_setfield(L, -2, "SIGBREAK");
#endif

	return 1;
}
