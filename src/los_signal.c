#include "lauxlib.h"
#include "lua.h"

#include <signal.h>
#include <string.h>
#include "lcwd.h"
#include "lutil.h"

#ifdef _WIN32
CRITICAL_SECTION SignalCriticalSection;
#include <windows.h>

#else
#include <unistd.h>
#endif

#define SIGNAL_QUEUE_MAX 25
static volatile sig_atomic_t signal_pending = 0, defer_signal = 0, processing_signals = 0;
static volatile sig_atomic_t signal_count = 0;
static volatile sig_atomic_t signals[SIGNAL_QUEUE_MAX];

static lua_State* mainL = NULL;
static int handlersRef = LUA_NOREF;

static void call_lua_callback(lua_State* L, lua_Debug* ar);
static void trigger_lua_callback(lua_State* L, int signum);

#ifdef _WIN32
BOOL WINAPI
windows_ctrl_handler(DWORD signum) {
    // convert windows signals to posix signals
    switch (signum) {
        case CTRL_C_EVENT: signum = SIGINT; break;
        case CTRL_BREAK_EVENT: signum = SIGBREAK; break;
        case CTRL_CLOSE_EVENT: signum = SIGTERM; break;
        case CTRL_LOGOFF_EVENT: signum = SIGTERM; break;
        case CTRL_SHUTDOWN_EVENT: signum = SIGTERM; break;
    }
    trigger_lua_callback(mainL, signum);
    return TRUE; // Indicate that the handler handled the event.
}
#endif

void
standard_signal_handler(int signum) {
    trigger_lua_callback(mainL, signum);
}

/*
** Hook set by signal function to stop the interpreter.
*/
static void
lua_interrupt(lua_State* L, lua_Debug* ar) {
    (void)ar;                   /* unused arg. */
    lua_sethook(L, NULL, 0, 0); /* reset hook */
    luaL_error(L, "interrupted!");
}

/*
** Function to be called at a C signal. Because a C signal cannot
** just change a Lua state (as there is no proper synchronization),
** this function only sets a hook that, when called, will stop the
** interpreter.
*/
static void
default_lua_sigint_handler(int i) {
    int flag = LUA_MASKCALL | LUA_MASKRET | LUA_MASKLINE | LUA_MASKCOUNT;
    signal(i, SIG_DFL); /* if another SIGINT happens, terminate process */
    lua_sethook(mainL, lua_interrupt, flag, 1);
}

static void
call_lua_callback(lua_State* L, lua_Debug* ar) {
    (void)ar;                   /* unused arg. */
    lua_sethook(L, NULL, 0, 0); /* reset hook */

// copy signals to cleanup queue and allow new signals to be queued
#if defined(_WIN32)
    EnterCriticalSection(&SignalCriticalSection);
#endif
    processing_signals = 1;
    sig_atomic_t count = signal_count;
    sig_atomic_t queued[SIGNAL_QUEUE_MAX];
    memcpy(queued, signals, sizeof(sig_atomic_t) * count);
    signal_count = 0;
    processing_signals = 0;
#if defined(_WIN32)
    LeaveCriticalSection(&SignalCriticalSection);
#endif

    lua_rawgeti(L, LUA_REGISTRYINDEX, handlersRef);

    while (count--) {
        int signum = queued[count];
        lua_rawgeti(L, -1, signum);
        lua_pushinteger(L, signum);
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            lua_writestringerror("error calling signal handler: %s\n", lua_tostring(L, -1));
        }
    }
}

// inspired by https://github.com/luaposix/luaposix/blob/aa2c8bf5af2eef5dd1e3de5f6ca55b90427c1b58/ext/posix/signal.c#L158
// and lua.c#70 (laction)
static void
trigger_lua_callback(lua_State* L, int signum) {
    if (defer_signal || processing_signals) {
        signal_pending = signum;
        return;
    }
    if (signal_count == SIGNAL_QUEUE_MAX) {
        return;
    }
#if defined(_WIN32)
    EnterCriticalSection(&SignalCriticalSection);
#endif
    defer_signal++;
    signals[signal_count++] = signum;
    lua_sethook(mainL, call_lua_callback, LUA_MASKCALL | LUA_MASKRET | LUA_MASKLINE | LUA_MASKCOUNT, 1);
    defer_signal--;
#if defined(_WIN32)
    LeaveCriticalSection(&SignalCriticalSection);
#endif
    if (defer_signal == 0 && signal_pending != 0) {
        raise(signal_pending);
    }
}

static int
eli_os_signal_handle(lua_State* L) {
    int signum = luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
#ifdef _WIN32
    if (!SetConsoleCtrlHandler(windows_ctrl_handler, TRUE)) {
        return push_error(L, "failed to set signal handler");
    }
    if (signal(signum, standard_signal_handler) == SIG_ERR) {
        return push_error(L, "failed to set signal handler");
    }
#elif defined(LUA_USE_POSIX)
    struct sigaction sa;
    sa.sa_handler = standard_signal_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask); /* do not mask any signal */
    //sigaction(sig, &sa, NULL);
    if (sigaction(signum, &sa, NULL) == -1) {
        return push_error(L, "failed to set signal handler");
    }
#else
    if (signal(signum, standard_signal_handler) == SIG_ERR) {
        return push_error(L, "failed to set signal handler");
    }
#endif

    lua_rawgeti(L, LUA_REGISTRYINDEX, handlersRef);
    lua_pushvalue(L, 2);
    lua_rawseti(L, -2, signum);
    lua_pop(L, 1);

    return 0;
}

static int
eli_os_signal_reset(lua_State* L) {
    int signum = luaL_checkinteger(L, 1);

#ifdef _WIN32
    if (!SetConsoleCtrlHandler(NULL, FALSE)) {
        return push_error(L, "failed to reset signal handler");
    }
    if (signal(signum, SIG_DFL) == SIG_ERR) {
        return push_error(L, "failed to reset signal handler");
    }
#elif defined(LUA_USE_POSIX)
    struct sigaction sa;
    sa.sa_handler = signum == SIGINT ? default_lua_sigint_handler : SIG_DFL;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask); /* do not mask any signal */
    if (sigaction(signum, &sa, NULL) == -1) {
        return push_error(L, "failed to reset signal handler");
    }
#else
    if (signum == SIGINT) {
        if (signal(signum, default_lua_sigint_handler) == SIG_ERR) {
            return push_error(L, "failed to reset signal handler");
        }
    } else {
        if (signal(signum, SIG_DFL) == SIG_ERR) {
            return push_error(L, "failed to reset signal handler");
        }
    }
#endif
    return 0;
}

static int
eli_os_signal_handlers(lua_State* L) {
    // list handlers stored in registry
    // return copy to avoid modification
    lua_rawgeti(L, LUA_REGISTRYINDEX, handlersRef);
    lua_newtable(L);
    lua_pushnil(L);
    while (lua_next(L, -3) != 0) {
        lua_pushvalue(L, -2);
        lua_pushvalue(L, -2);
        lua_rawset(L, -6);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    return 1;
}

static int
eli_os_signal_raise(lua_State* L) {
    int signum = luaL_checkinteger(L, 1);
    raise(signum);
    return 0;
}

static const struct luaL_Reg eliOsSignal[] = {
    {"handle", eli_os_signal_handle},
    {"reset", eli_os_signal_reset},
    {"handlers", eli_os_signal_handlers},
    {"raise", eli_os_signal_raise},
    {NULL, NULL},
};

// NOTE: do not load/open "os.signal" outside of main thread/main lua state
int
luaopen_eli_os_signal(lua_State* L) {
    mainL = L;
    lua_newtable(L);
    handlersRef = luaL_ref(L, LUA_REGISTRYINDEX);

    lua_newtable(L);
    luaL_setfuncs(L, eliOsSignal, 0);

    // add signals - SIGTERM, SIGKILL and SIGINT
    lua_pushinteger(L, SIGTERM);
    lua_setfield(L, -2, "SIGTERM");
    lua_pushinteger(L, 9 /*SIGKILL*/);
    lua_setfield(L, -2, "SIGKILL");
    lua_pushinteger(L, SIGINT);
    lua_setfield(L, -2, "SIGINT");
    return 1;
}
