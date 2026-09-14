## eli-lib os posix & win32 extra api

`eli.os.extra` provides sleep and working-directory functions, installs the
environment functions directly on `os`, and exposes the nested `signal` API as
`os.signal`. Eli's `eli.env` wrapper remains available only for deprecation.
It also synchronizes standard Lua environment reads and child launches across
worker states. Environment support is included here; eli-env-extra is no longer
required.

### Dependencies
- eli-extra-utils
- c11threads
