#include "lua.h"
#include "lauxlib.h"
#include "lerror.h"
#include "lenv.h"
#include "los_signal.h"

#include "c11threads.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include "environ.h"
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/* Process-wide lock shared by every state. getenv/setenv/environ and
 * subprocess snapshots all use it. */
static once_flag env_once = ONCE_FLAG_INIT;
static mtx_t env_lock_handle;
static int env_runtime_failed;

static void env_runtime_init(void)
{
	env_runtime_failed = mtx_init(&env_lock_handle, mtx_plain) != thrd_success;
}

void eli_env_lock(void)
{
	call_once(&env_once, env_runtime_init);
	if (env_runtime_failed || mtx_lock(&env_lock_handle) != thrd_success) {
		abort();
	}
}

void eli_env_unlock(void)
{
	if (mtx_unlock(&env_lock_handle) != thrd_success) {
		abort();
	}
}

/* Snapshots a variable into native memory. Returns 1 when found, 0 when
 * absent, -1 on allocation failure. Call under eli_env_lock(). */
static int eli_env_snapshot(const char *name, char **value, size_t *length)
{
	*value = NULL;
	*length = 0;
#ifdef _WIN32
	{
		DWORD needed;
		char *buffer;
		DWORD copied;

		/* An existing empty variable and a missing one both return 0;
		 * only the last error distinguishes them. */
		SetLastError(ERROR_SUCCESS);
		needed = GetEnvironmentVariable(name, NULL, 0);
		if (needed == 0) {
			if (GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
				return 0;
			}
			needed = 1; /* present but empty: room for the terminator */
		}
		buffer = (char *)malloc(needed);
		if (buffer == NULL) {
			return -1;
		}
		SetLastError(ERROR_SUCCESS);
		copied = GetEnvironmentVariable(name, buffer, needed);
		if (copied == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
			free(buffer);
			return 0;
		}
		if (copied >= needed) {
			free(buffer);
			return 0;
		}
		*value = buffer;
		*length = copied;
		return 1;
	}
#else
	{
		const char *found = getenv(name);
		char *copy;

		if (found == NULL) {
			return 0;
		}
		copy = strdup(found);
		if (copy == NULL) {
			return -1;
		}
		*value = copy;
		*length = strlen(copy);
		return 1;
	}
#endif
}

typedef struct {
	const char *value;
	size_t length;
} eli_env_view;

static int eli_env_push_lstring(lua_State *L)
{
	const eli_env_view *view = (const eli_env_view *)lua_touserdata(L, 1);
	lua_pushlstring(L, view->value, view->length);
	return 1;
}

/* Pushes the snapshot as a Lua string, freeing it even when the push raises
 * an allocation error. */
static int eli_env_push_owned(lua_State *L, char *value, size_t length)
{
	eli_env_view view = { value, length };
	int status;

	lua_pushcfunction(L, eli_env_push_lstring);
	lua_pushlightuserdata(L, &view);
	status = lua_pcall(L, 1, 1, 0);
	free(value);
	if (status != LUA_OK) {
		return lua_error(L);
	}
	return 1;
}

static int eli_env_getenv_guard(lua_State *L)
{
	const char *nam = luaL_checkstring(L, 1);
	char *value = NULL;
	size_t length = 0;
	int status;

	eli_env_lock();
	status = eli_env_snapshot(nam, &value, &length);
	eli_env_unlock();
	if (status < 0) {
		return luaL_error(L, "out of memory");
	}
	if (status == 0) {
		lua_pushnil(L);
		return 1;
	}
	return eli_env_push_owned(L, value, length);
}

#ifndef _WIN32
static int
pipe_cloexec(int fds[2])
{
	int flags;

	if (pipe(fds) == -1) return -1;
	flags = fcntl(fds[0], F_GETFD);
	if (flags == -1 || fcntl(fds[0], F_SETFD, flags | FD_CLOEXEC) == -1) goto fail;
	flags = fcntl(fds[1], F_GETFD);
	if (flags == -1 || fcntl(fds[1], F_SETFD, flags | FD_CLOEXEC) == -1) goto fail;
	return 0;
fail:
	{
		int error = errno;
		close(fds[0]);
		close(fds[1]);
		errno = error;
	}
	return -1;
}

/* Spawn under the environment lock, but never hold it during the wait. */
static int eli_env_system(const char *command)
{
	sigset_t blocked, original, defaults;
	posix_spawnattr_t attr;
	pid_t pid;
	int error, status = -1;
	char *argv[] = { "sh", "-c", "--", (char *)command, NULL };

	sigemptyset(&blocked);
	sigaddset(&blocked, SIGCHLD);
	error = pthread_sigmask(SIG_BLOCK, &blocked, &original);
	if (error != 0) { errno = error; return -1; }
	error = posix_spawnattr_init(&attr);
	if (error != 0) goto restore_mask;
	eli_env_lock();
	error = eli_signal_system_begin();
	if (error != 0) goto unlock;
	eli_signal_child_sigdefault(&defaults);
	error = posix_spawnattr_setsigmask(&attr, &original);
	if (error == 0) error = posix_spawnattr_setsigdefault(&attr, &defaults);
	if (error == 0) error = posix_spawnattr_setflags(&attr,
		POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
	if (error == 0) error = posix_spawn(&pid, "/bin/sh", NULL, &attr, argv, environ);
	eli_env_unlock();
	if (error == 0) {
		while (waitpid(pid, &status, 0) == -1) {
			if (errno != EINTR) { error = errno; status = -1; break; }
		}
	}
	eli_env_lock();
	eli_signal_system_end();
unlock:
	eli_env_unlock();
	posix_spawnattr_destroy(&attr);
restore_mask:
	pthread_sigmask(SIG_SETMASK, &original, NULL);
	/* Spawn returns an error number; Lua expects it in errno. */
	errno = error;
	return status;
}

typedef struct {
	luaL_Stream stream;
	pid_t pid;
} eli_env_pipe;

static int eli_env_pclose(lua_State *L)
{
	eli_env_pipe *p = luaL_checkudata(L, 1, LUA_FILEHANDLE);
	int status, error = 0;
	errno = 0;
	if (fclose(p->stream.f) != 0) error = errno;
	while (waitpid(p->pid, &status, 0) == -1) {
		if (errno != EINTR) return luaL_execresult(L, -1);
	}
	errno = error;
	return luaL_execresult(L, error ? -1 : status);
}

static int eli_env_popen_guard(lua_State *L)
{
	const char *command = luaL_checkstring(L, 1);
	const char *mode = luaL_optstring(L, 2, "r");
	eli_env_pipe *p;
	posix_spawn_file_actions_t actions;
	posix_spawnattr_t attr;
	sigset_t defaults;
	int fds[2], error, parent = mode[0] == 'r' ? 0 : 1;
	char *argv[] = { "sh", "-c", "--", (char *)command, NULL };
	luaL_argcheck(L, (mode[0] == 'r' || mode[0] == 'w') && mode[1] == '\0',
		2, "invalid mode");
	p = lua_newuserdatauv(L, sizeof *p, 0);
	p->stream.closef = NULL;
	p->stream.f = NULL;
	luaL_setmetatable(L, LUA_FILEHANDLE);
	/* Older musl pclose closes an fd before unlinking its FILE; concurrent
	 * popen can then close a reused pipe fd via that stale list entry. Own
	 * the pid instead, so fclose and wait both stay outside the env lock.
	 * CLOEXEC also closes previous popen pipes in every new executable. */
	fflush(NULL);
	eli_env_lock();
	if (pipe_cloexec(fds) == -1) {
		error = errno;
		goto unlock;
	}
	if ((p->stream.f = fdopen(fds[parent], mode)) == NULL) {
		error = errno;
		goto close_pipe;
	}
	error = posix_spawn_file_actions_init(&actions);
	if (error == 0) {
		error = posix_spawn_file_actions_adddup2(&actions, fds[1-parent], 1-parent);
		if (error == 0) error = posix_spawnattr_init(&attr);
		if (error == 0) {
			eli_signal_child_sigdefault(&defaults);
			error = posix_spawnattr_setsigdefault(&attr, &defaults);
			/* popen inherits the caller's mask, including worker blocks. */
			if (error == 0) error = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF);
			if (error == 0) error = posix_spawn(&p->pid, "/bin/sh", &actions,
				&attr, argv, environ);
			posix_spawnattr_destroy(&attr);
		}
		posix_spawn_file_actions_destroy(&actions);
	}
close_pipe:
	close(fds[1-parent]);
	if (error != 0) {
		if (p->stream.f != NULL) fclose(p->stream.f);
		else close(fds[parent]);
		p->stream.f = NULL;
	}
unlock:
	eli_env_unlock();
	if (error == 0) p->stream.closef = eli_env_pclose;
	errno = error;
	return error != 0 ? luaL_fileresult(L, 0, command) : 1;
}

#else

/* The CRT system/_popen launch children with broad handle inheritance. Keep
 * their launch and CRT-environment snapshot behind the same lock as bundled
 * CreateProcess calls; waiting and Lua allocation must remain outside it. */
static int eli_env_system(const char *command)
{
	const char *shell;
	const char *argv[4];
	intptr_t child;
	int status, error, has_comspec;

	eli_env_lock();
	shell = getenv("COMSPEC");
	has_comspec = shell != NULL && *shell != '\0';
	if (!has_comspec) shell = "cmd.exe";
	argv[0] = shell;
	argv[1] = "/c";
	argv[2] = command;
	argv[3] = NULL;
	errno = 0;
	child = has_comspec
		? _spawnve(_P_NOWAIT, shell, argv, environ)
		: _spawnvpe(_P_NOWAIT, shell, argv, environ);
	error = errno;
	eli_env_unlock();
	errno = error;
	if (child == -1) {
		return -1;
	}
	if (_cwait(&status, child, 0) == -1) {
		return -1;
	}
	return status;
}

static int eli_env_pclose(lua_State *L)
{
	luaL_Stream *stream = luaL_checkudata(L, 1, LUA_FILEHANDLE);

	errno = 0;
	return luaL_execresult(L, _pclose(stream->f));
}

static int eli_env_popen_guard(lua_State *L)
{
	const char *command = luaL_checkstring(L, 1);
	const char *mode = luaL_optstring(L, 2, "r");
	luaL_Stream *stream;
	FILE *file;
	int error;

	luaL_argcheck(L, (mode[0] == 'r' || mode[0] == 'w') &&
		(mode[1] == '\0' || ((mode[1] == 'b' || mode[1] == 't') &&
		 mode[2] == '\0')), 2, "invalid mode");
	stream = lua_newuserdatauv(L, sizeof(*stream), 0);
	stream->f = NULL;
	stream->closef = NULL;
	luaL_setmetatable(L, LUA_FILEHANDLE);
	eli_env_lock();
	errno = 0;
	file = _popen(command, mode);
	error = errno;
	eli_env_unlock();
	errno = error;
	if (file == NULL) {
		return luaL_fileresult(L, 0, command);
	}
	stream->f = file;
	stream->closef = eli_env_pclose;
	return 1;
}
#endif

static int eli_env_execute_guard(lua_State *L)
{
	const char *command = luaL_optstring(L, 1, NULL);
	int status = eli_env_system(command == NULL ? "exit 0" : command);

	if (command != NULL) {
		return luaL_execresult(L, status);
	}
	lua_pushboolean(L, status == 0);
	return 1;
}

void eli_env_install(lua_State *L)
{
	lua_getglobal(L, "os");
	if (lua_istable(L, -1)) {
		lua_pushcfunction(L, eli_env_getenv_guard);
		lua_setfield(L, -2, "getenv");
		lua_pushcfunction(L, eli_getenv);
		lua_setfield(L, -2, "get_env");
		lua_pushcfunction(L, eli_setenv);
		lua_setfield(L, -2, "set_env");
		lua_pushcfunction(L, eli_setenv);
		lua_setfield(L, -2, "setenv");
		lua_pushcfunction(L, eli_environ);
		lua_setfield(L, -2, "environment");
		lua_pushcfunction(L, eli_env_execute_guard);
		lua_setfield(L, -2, "execute");
	}
	lua_pop(L, 1);
	lua_getglobal(L, "io");
	if (lua_istable(L, -1)) {
		lua_pushcfunction(L, eli_env_popen_guard);
		lua_setfield(L, -2, "popen");
	}
	lua_pop(L, 1);
}
/*
---#DES 'env.get_env'
---
---Gets value of env variable.
---Returns value on success or nil, error description and errno on failure.
---@param name string
---@return string?, string?, integer?

name -- value/nil
*/
int eli_getenv(lua_State *L)
{
	const char *nam = luaL_checkstring(L, 1);
	char *value = NULL;
	size_t length = 0;
	int status;

	eli_env_lock();
	status = eli_env_snapshot(nam, &value, &length);
	eli_env_unlock();
	if (status < 0) {
		return luaL_error(L, "out of memory");
	}
	if (status == 0) {
		return push_error(L, NULL);
	}
	return eli_env_push_owned(L, value, length);
}

/*
---#DES 'env.set_env'
---
---Sets or removes an env variable.
---Returns true on success or nil, error description and errno on failure.
---@param name string
---@param value string|nil
---@return boolean|nil, nil|string, nil|integer

name value -- true/nil error
name nil -- true/nil error*/
int eli_setenv(lua_State *L)
{
	const char *nam = luaL_checkstring(L, 1);
	const char *val = lua_isnoneornil(L, 2) ? NULL : luaL_checkstring(L, 2);
#ifdef _WIN32
	eli_env_lock();
	if (!SetEnvironmentVariable(nam, val)) {
		eli_env_unlock();
		return push_error(L, NULL);
	}
#else
	int err;
	eli_env_lock();
	err = val ? setenv(nam, val, 1) : unsetenv(nam);
	if (err == -1) {
		eli_env_unlock();
		return push_error(L, NULL);
	}
#endif
	eli_env_unlock();
	lua_pushboolean(L, 1);
	return 1;
}

/* 
---#DES 'env.environment'
---
---Gets environment table (all env variables).
---Returns table on success or nil, error description and errno on failure.
---@return table<string,string>?, string?, integer?

-- environment-table */
static int eli_env_build_table(lua_State *L)
{
	const eli_env_view *view = (const eli_env_view *)lua_touserdata(L, 1);
	size_t offset = 0;

	lua_newtable(L);
	while (offset < view->length) {
		const char *entry = view->value + offset;
		size_t entry_len = strlen(entry);
		const char *eq = strchr(entry, '=');

		offset += entry_len + 1;
		if (eq == NULL) {
			continue;
		}
		lua_pushlstring(L, entry, (size_t)(eq - entry));
		lua_pushlstring(L, eq + 1, entry_len - (size_t)(eq - entry) - 1);
		lua_settable(L, -3);
	}
	return 1;
}

int eli_environ(lua_State *L)
{
	eli_env_view view = { NULL, 0 };
	int status;
#ifdef _WIN32
	const char *envs;
	const char *end;
#else
	const char **env;
	char *snapshot = NULL;
	size_t total = 0;
#endif

#ifdef _WIN32
	eli_env_lock();
	envs = GetEnvironmentStrings();
	eli_env_unlock();
	if (envs == NULL) {
		return push_error(L, NULL);
	}
	end = envs;
	while (*end) {
		end += strlen(end) + 1;
	}
	view.value = envs;
	view.length = (size_t)(end - envs) + 1;
#else
	eli_env_lock();
	for (env = (const char **)environ; *env; env++) {
		total += strlen(*env) + 1;
	}
	if (total > 0) {
		size_t offset = 0;
		snapshot = (char *)malloc(total);
		if (snapshot == NULL) {
			eli_env_unlock();
			return luaL_error(L, "out of memory");
		}
		for (env = (const char **)environ; *env; env++) {
			size_t entry_len = strlen(*env) + 1;
			memcpy(snapshot + offset, *env, entry_len);
			offset += entry_len;
		}
	}
	eli_env_unlock();
	view.value = snapshot;
	view.length = total;
#endif

	lua_pushcfunction(L, eli_env_build_table);
	lua_pushlightuserdata(L, &view);
	status = lua_pcall(L, 1, 1, 0);
#ifdef _WIN32
	FreeEnvironmentStrings((char *)envs);
#else
	free(snapshot);
#endif
	if (status != LUA_OK) {
		return lua_error(L);
	}
	return 1;
}
