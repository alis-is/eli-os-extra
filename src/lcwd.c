#include "lua.h"
#include "lauxlib.h"
#include <errno.h>

#include "lutil.h"
#include <stdlib.h>

#ifdef _WIN32
#include <direct.h>
/* MAX_PATH seems to be 260. Seems kind of small. Is there a better one? */
#define LMAXPATHLEN MAX_PATH
#else
#include <unistd.h>
#ifdef MAXPATHLEN
#define LMAXPATHLEN MAXPATHLEN
#else
#include <limits.h> /* for _POSIX_PATH_MAX */
#define LMAXPATHLEN _POSIX_PATH_MAX
#endif
#endif

#ifdef _WIN32
#define _lget_cwd _getcwd
#define _lchdir _chdir
#else
#define _lget_cwd getcwd
#define _lchdir chdir
#endif

/*
** This function returns the current directory
** If unable to get the current directory, it returns nil
**  and a string describing the error
*/
int eli_cwd(lua_State *L)
{
#ifdef NO_GETCWD
    lua_pushnil(L);
    lua_pushstring(L, "Function 'getcwd' not provided by system");
    return 2;
#else
    char *path = NULL;
    /* Passing (NULL, 0) is not guaranteed to work. Use a temp buffer and size instead. */
    size_t size = LMAXPATHLEN; /* initial buffer size */
    int result;
    while (1)
    {
        char *path2 = realloc(path, size);
        if (!path2) /* failed to allocate */
        {
            result = push_error(L, "get_dir realloc() failed");
            break;
        }
        path = path2;
        if (_lget_cwd(path, size) != NULL)
        {
            /* success, push the path to the Lua stack */
            lua_pushstring(L, path);
            result = 1;
            break;
        }
        if (errno != ERANGE)
        { /* unexpected error */
            result = push_error(L, "get_dir getcwd() failed");
            break;
        }
        /* ERANGE = insufficient buffer capacity, double size and retry */
        size *= 2;
    }
    free(path);
    return result;
#endif
}

/*
** This function changes the working (current) directory
*/
int eli_chdir(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    if (_lchdir(path))
    {
        return push_error(L, "Unable to change working directory");
    }

    lua_pushboolean(L, 1);
    return 1;
}
