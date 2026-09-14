/* Native integration test: real overlapping shell waits, Lua signal setters,
 * and proc's actual post-fork child path. No worker library or full eli build.
 * Build/run: cmake -S deps/eli-os-extra/tests -B /tmp/opencode/eli-signals
 *           cmake --build /tmp/opencode/eli-signals -j2
 *           ctest --test-dir /tmp/opencode/eli-signals --output-on-failure
 */
#include <spawn.h>
#include <errno.h>
#include <string.h>

static int test_spawn(pid_t *pid, const char *path,
                     const posix_spawn_file_actions_t *actions,
                     const posix_spawnattr_t *attr, char *const argv[],
                     char *const envp[])
{
    if (argv[3] && strcmp(argv[3], "__fail_spawn__") == 0) return EAGAIN;
    return posix_spawn(pid, path, actions, attr, argv, envp);
}

#define posix_spawn test_spawn
#include "../src/lenv.c"
#undef posix_spawn
#include "../src/los_signal.c"
#include "../src/los.h"
#include "../../eli-proc-extra/src/lspawn.c"
#include "lualib.h"
#undef NDEBUG
#include <assert.h>

static const char *executable;

static void run(lua_State *L, const char *code)
{
    if (luaL_dostring(L, code) != LUA_OK) {
        fprintf(stderr, "%s\n", lua_tostring(L, -1));
        abort();
    }
}

static void check_action(int signum, void (*handler)(int))
{
    struct sigaction action;
    assert(sigaction(signum, NULL, &action) == 0);
    assert(action.sa_handler == handler);
    if (handler == standard_signal_handler) {
        assert(action.sa_flags & SA_RESTART);
        assert(sigismember(&action.sa_mask, SIGTERM) == 1);
    }
}

static void check_mask(int blocked)
{
    sigset_t mask;
    assert(pthread_sigmask(SIG_SETMASK, NULL, &mask) == 0);
    assert(sigismember(&mask, SIGUSR1) == blocked);
    assert(sigismember(&mask, SIGTERM) == blocked);
    assert(sigismember(&mask, SIGINT) == blocked);
    assert(sigismember(&mask, SIGQUIT) == blocked);
    assert(sigismember(&mask, SIGCHLD) == 0);
}

static void set_mask(int blocked)
{
    sigset_t mask;
    sigemptyset(&mask);
    if (blocked) {
        sigaddset(&mask, SIGUSR1);
        sigaddset(&mask, SIGTERM);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGQUIT);
        /* Shells may normalize SIGCHLD themselves; test worker blocks on
         * the user-facing signals and removal of system's temporary block. */
    }
    assert(pthread_sigmask(SIG_SETMASK, &mask, NULL) == 0);
}

struct waiter {
    pthread_t thread;
    int ready[2], gate[2];
    char command[4096];
};

static void *execute(void *data)
{
    struct waiter *w = data;
    set_mask(1); /* representative worker mask */
    assert(eli_env_system(w->command) == 0);
    check_mask(1);
    return NULL;
}

static void start(struct waiter *w)
{
    char byte;
    assert(pipe(w->ready) == 0 && pipe(w->gate) == 0);
    assert(snprintf(w->command, sizeof w->command, "exec '%s' --wait %d %d",
                    executable, w->ready[1], w->gate[0]) < sizeof w->command);
    assert(pthread_create(&w->thread, NULL, execute, w) == 0);
    assert(read(w->ready[0], &byte, 1) == 1);
}

static void finish(struct waiter *w)
{
    assert(write(w->gate[1], "x", 1) == 1);
    assert(pthread_join(w->thread, NULL) == 0);
    close(w->ready[0]); close(w->ready[1]);
    close(w->gate[0]); close(w->gate[1]);
}

static void children(lua_State *L, int ignore_int, int ignore_quit)
{
    char command[4096], lua[8192], int_arg[2], quit_arg[2];
    int status, error_pipe[2];
    pid_t pid;
    spawn_params p = { 0 };
    int_arg[0] = '0' + ignore_int; int_arg[1] = 0;
    quit_arg[0] = '0' + ignore_quit; quit_arg[1] = 0;
    const char *argv[] = { executable, "--probe", int_arg, quit_arg, "0", NULL };
    p.argv = argv;
    p.envp = (const char **)environ;
    p.redirect[0] = p.redirect[1] = p.redirect[2] = -1;

    set_mask(1);
    eli_env_lock();
    p.candidates = execve_candidate_list(executable);
    assert(p.candidates && pipe(error_pipe) == 0);
    pid = fork();
    assert(pid != -1);
    if (pid == 0) {
        close(error_pipe[0]);
        child_init(error_pipe[1], -1, -1, -1, &p);
        _exit(99);
    }
    eli_env_unlock();
    close(error_pipe[1]);
    assert(read(error_pipe[0], &status, sizeof status) == 0);
    close(error_pipe[0]);
    assert(waitpid(pid, &status, 0) == pid && status == 0);
    execve_candidate_list_free(p.candidates);
    check_mask(1);

    assert(snprintf(command, sizeof command, "exec '%s' --probe %d %d 1",
                    executable, ignore_int, ignore_quit) < sizeof command);
    lua_pushstring(L, command);
    lua_setglobal(L, "command");
    run(L, "assert(os.execute(command)); local p = assert(io.popen(command)); "
           "p:read('a'); assert(p:close())");
    check_mask(1);
    set_mask(0);
    /* Also verify unblocked shell callers, rather than masking disposition bugs. */
    assert(snprintf(lua, sizeof lua,
        "command = [[exec '%s' --probe %d %d 0]]; assert(os.execute(command)); "
        "local p = assert(io.popen(command)); p:read('a'); assert(p:close())",
        executable, ignore_int, ignore_quit) < sizeof lua);
    run(L, lua);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--wait") == 0) {
        char byte;
        assert(write(atoi(argv[2]), "r", 1) == 1);
        assert(read(atoi(argv[3]), &byte, 1) == 1);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--probe") == 0) {
        check_action(SIGINT, atoi(argv[2]) ? SIG_IGN : SIG_DFL);
        check_action(SIGQUIT, atoi(argv[3]) ? SIG_IGN : SIG_DFL);
        check_mask(atoi(argv[4]));
        return 0;
    }
    executable = argv[0];
    alarm(25);
    set_mask(0);
    lua_State *L = luaL_newstate();
    assert(L);
    luaL_requiref(L, "_G", luaopen_base, 1); lua_pop(L, 1);
    luaL_requiref(L, "os", luaopen_os, 1); lua_pop(L, 1);
    luaL_requiref(L, "io", luaopen_io, 1); lua_pop(L, 1);
    luaL_requiref(L, "eli.os.extra", luaopen_eli_os_extra, 1);
    assert(lua_istable(L, -1));
    lua_pop(L, 1);
    lua_getglobal(L, "os");
    lua_getfield(L, -1, "get_env");
    assert(lua_isfunction(L, -1));
    lua_getfield(L, -2, "set_env");
    assert(lua_isfunction(L, -1));
    lua_getfield(L, -3, "setenv");
    assert(lua_isfunction(L, -1));
    lua_getfield(L, -4, "environment");
    assert(lua_isfunction(L, -1));
    lua_pop(L, 5);
    eli_runtime_set_main_state(L);
    lua_getglobal(L, "os");
    lua_getfield(L, -1, "signal");
    lua_setglobal(L, "signal");
    lua_pop(L, 1);
    lua_pushinteger(L, SIGINT); lua_setglobal(L, "INT");
    lua_pushinteger(L, SIGQUIT); lua_setglobal(L, "QUIT");
    run(L, "hits = 0; function handler() hits = hits + 1 end");

    for (int round = 0; round < 3; round++) {
        struct waiter a, b;
        run(L, round == 1 ?
            "signal.handle(INT, signal.IGNORE_SIGNAL); signal.handle(QUIT, signal.IGNORE_SIGNAL)" :
            "signal.reset(INT); signal.handle(QUIT, handler)");
        children(L, round == 1, round == 1);
        start(&a); start(&b);
        children(L, round == 1, round == 1);
        run(L, round == 0 ? "signal.handle(INT, handler); signal.reset(QUIT)" :
               round == 1 ? "signal.reset(INT); signal.handle(QUIT, handler)" :
               "signal.handle(INT, signal.IGNORE_SIGNAL); signal.handle(QUIT, signal.IGNORE_SIGNAL)");
        check_action(SIGINT, SIG_IGN); check_action(SIGQUIT, SIG_IGN);
        children(L, round == 2, round == 2);
        assert(eli_env_system("__fail_spawn__") == -1 && errno == EAGAIN);
        finish(&a);
        check_action(SIGINT, SIG_IGN); check_action(SIGQUIT, SIG_IGN);
        children(L, round == 2, round == 2);
        finish(&b);
        assert(eli_env_system("__fail_spawn__") == -1 && errno == EAGAIN);
        check_action(SIGINT, round == 0 ? standard_signal_handler : round == 1 ? SIG_DFL : SIG_IGN);
        check_action(SIGQUIT, round == 1 ? standard_signal_handler : round == 0 ? SIG_DFL : SIG_IGN);
        children(L, round == 2, round == 2);
        if (round < 2) {
            assert(raise(round == 0 ? SIGINT : SIGQUIT) == 0);
            check_signal_hook(L, NULL);
            run(L, "assert(hits == 1); hits = 0");
        }
        check_mask(0);
    }
    run(L, "signal.reset(INT); signal.reset(QUIT)");
    lua_close(L);

    /* A worker must not be able to deliver a process-wide signal to the
     * application's main state. */
    L = luaL_newstate();
    assert(L);
    eli_runtime_set_worker_state(L);
    luaL_requiref(L, "_G", luaopen_base, 1); lua_pop(L, 1);
    luaL_requiref(L, "eli.os.extra", luaopen_eli_os_extra, 0);
    lua_getfield(L, -1, "signal");
    lua_setglobal(L, "signal");
    lua_pop(L, 1);
    run(L, "assert(not pcall(function() signal.raise(signal.SIGTERM) end))");
    lua_close(L);

    puts("signal coordination: ok");
    return 0;
}
