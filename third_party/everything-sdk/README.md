# Everything SDK (vendored)

This directory contains a minimal vendored slice of the **Everything SDK**
that VsJump uses to perform a global filename lookup when the user-supplied
`destdir` roots fail to match a `vsjump://match/?...` source path.

## What is included

| File | Purpose |
|---|---|
| `include/Everything.h` | Public C API header (used at compile time) |
| `dll/Everything64.dll` | x64 IPC client DLL (loaded at run time) |

VsJump **does not link** against `Everything64.lib`. The DLL is loaded
lazily via `LoadLibraryW` + `GetProcAddress`, so:

- The `.lib` file is not required and is intentionally not vendored.
- VsJump runs fine on machines that don't have Everything installed —
  the global-search step is simply skipped.

The DLL is automatically copied next to `vsjump.exe` by CMake so the
deployed binary is self-contained.

## Source / license

- Upstream: <https://www.voidtools.com/support/everything/sdk/>
- License: MIT (see the copyright header in `include/Everything.h`)

Only the files needed to issue an IPC search query against a running
`Everything.exe` are vendored here; the upstream SDK additionally ships
32-bit / ARM builds, language bindings, and example projects that VsJump
does not need.
