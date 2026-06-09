# VsJump

A tiny Windows utility that registers a `vsjump://` URL handler so any
application — a browser, a chat client, a log viewer, your own tool — can ask
**the Visual Studio instance that already has the matching solution open** to
jump to a specific file and line.

```
vsjump://file/D:/proj/src/main.cpp:1343:8
```

No new IDE is launched. If a VS with the right solution is already running,
that one opens the file. Otherwise the tool tells you and stops.

---

## Features

- **Single-purpose URL protocol** — register once, use from anywhere that can
  launch a URL (`start`, `ShellExecute`, an `<a href>`, etc.).
- **VS Code–style direct URLs**:
  - `vsjump://file/D:/proj/src/foo.cpp:1234:8`
  - `vsjump://file/D:\proj\src\foo.cpp:1234`
  - `vsjump://file/D:/proj/src/foo.cpp`
- **Source-tree match URLs** — turn a build-server / WinDbg-style path into a
  local file by recursive name + tail-segment matching against one or more
  local source roots (see [Match form](#match-form)):
  - `vsjump://match/?srcfile=c:\build\agent\src\foo.cpp:1234:8&destdir=D:\repo`
- **Smart routing** — enumerates running VS instances via the COM Running
  Object Table (`!VisualStudio.DTE.*`) and picks the right one (see
  [How the target VS is chosen](#how-the-target-vs-is-chosen)).
- **Per-user install** — writes only to `HKEY_CURRENT_USER`, no admin rights
  required.
- **Self-contained `.exe`** — statically linked MSVC runtime, no extra DLLs.
- **Bitness-agnostic** — a 32-bit `vsjump.exe` can drive a 64-bit VS (COM
  marshals across the ROT).

---

## Build

Requirements: **CMake ≥ 3.15** and **MSVC** (Visual Studio 2019/2022).

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

Output: `build\Release\vsjump.exe`.

---

## Install

Place `vsjump.exe` where it should live permanently (the registration stores
its absolute path), then run:

```powershell
.\vsjump.exe register
```

This writes to `HKCU\Software\Classes\vsjump`:

| Value | Data |
|---|---|
| `(default)` | `URL:VS Jump Protocol` |
| `URL Protocol` | *(empty)* |
| `DefaultIcon\(default)` | `"<exe>",0` |
| `shell\open\command\(default)` | `"<exe>" "%1"` |

To remove:

```powershell
.\vsjump.exe unregister
```

---

## Usage

Once registered, any URL launcher works:

```powershell
# From a shell:
start vsjump://file/D:/proj/src/main.cpp:42:8

# From C/C++:
ShellExecuteW(nullptr, L"open",
              L"vsjump://file/D:/proj/src/main.cpp:42",
              nullptr, nullptr, SW_SHOW);
```

You can also invoke the tool directly, bypassing the shell:

```powershell
.\vsjump.exe "vsjump://file/D:/proj/src/main.cpp:42"
.\vsjump.exe --help
```

### URL grammar

VsJump accepts two URL kinds:

```
vsjump://file/<absolute-path>[:<line>[:<column>]]                        (direct)
vsjump://match/?srcfile=<src-path>[:<line>[:<column>]]&destdir=<dir>...  (match)
```

Common rules:

- Paths may use either `/` or `\`.
- Percent-encoded sequences (`%20`, etc.) are decoded as UTF-8.
- `line` and `column` are **1-based**; both are optional.

#### Direct form examples

| URL | Meaning |
|---|---|
| `vsjump://file/D:/proj/src/foo.cpp:1234:8` | foo.cpp, line 1234, column 8 |
| `vsjump://file/D:\proj\src\foo.cpp:42` | foo.cpp, line 42 |
| `vsjump://file/D:/proj/with%20space/foo.cpp:1` | percent-decoded path |

#### Match form

Designed for tools (typically **WinDbg**, crash dump analyzers, CI log
viewers) that emit source paths from the **build machine** — paths that do
not exist on the developer's box. VsJump resolves them against one or more
local source roots before opening.

```
vsjump://match/?srcfile=<src-path>[:<line>[:<col>]]&destdir=<localdir>[&destdir=<localdir2>...]
```

Resolution algorithm:

1. Take the **basename** of `srcfile` (e.g. `foo.cpp`).
2. Recursively scan every `destdir` for files with that basename
   (case-insensitive, follows no symlinks/junctions).
3. Score each hit by the number of **trailing path segments** it shares with
   `srcfile` (case-insensitive). Highest score wins.
4. If multiple hits tie for the top score, a picker dialog asks the user.
5. If no file with that basename exists under any `destdir`, an error is
   shown and exit code `7` is returned.

Examples:

```text
# WinDbg reports a path like c:\build\agent\xyz\src\modules\foo.cpp:1843
# Match it against the local checkout at D:\dev\repo:
vsjump://match/?srcfile=c%3A%5Cbuild%5Cagent%5Cxyz%5Csrc%5Cmodules%5Cfoo.cpp%3A1843&destdir=D%3A%5Cdev%5Crepo

# Multiple search roots are allowed:
vsjump://match/?srcfile=/builds/x/src/foo.cpp:42&destdir=D:\repoA&destdir=D:\repoB
```

> Tip: keep `destdir` reasonably scoped (a project tree, not all of `C:\`)
> so the recursive scan stays fast. The scan does not prune `node_modules`,
> `.git`, build outputs, etc.

---

## How the target VS is chosen

For each running Visual Studio, VsJump reads `DTE.Solution.FullName` plus the
main-window `HWND`, then picks one in this order:

1. **Solution-directory match** — if the file is under some VS's solution
   directory (case-insensitive), that VS wins. With multiple matches the
   **deepest / most specific** directory wins.
2. **Single VS** — if no solution matches but only one VS is running, use it.
3. **Foreground VS** — if multiple VS are running and one is the foreground
   window, use that ("the IDE the user is actively in").
4. **User picker** — otherwise a small TaskDialog appears with one radio
   button per running solution; the user picks one (or cancels).

If **no** VS is running, a warning dialog is shown and **no new IDE is
started** — VsJump is intentionally a router, not a launcher.

---

## Exit codes

| Code | Meaning |
|---|---|
| 0 | Success (or user-cancelled picker) |
| 1 | No arguments / help shown |
| 2 | Register / unregister failed |
| 3 | URL could not be parsed or path could not be resolved |
| 4 | `CoInitializeEx` failed |
| 5 | No VS running, or no instance could be selected |
| 6 | Found a VS but `OpenFile` / `GotoLine` failed |
| 7 | Match URL: no local file matched `srcfile` under any `destdir` |

---

## Known limitations

- Only files under the **solution directory tree** participate in
  solution-match routing. Linked items added from elsewhere fall back to the
  single-VS / foreground / picker rules. To match by project membership
  instead, walk `Solution.Projects` in `vs_locator.cpp`.
- Visual Studio Code is **not** supported (it is not a DTE host). Use VS
  Code's built-in `vscode://file/...` URL for that.
- Routing relies on the VS instance being registered in the ROT, which the
  IDE does automatically once a solution finishes loading.

---

## License

[MIT](LICENSE)
