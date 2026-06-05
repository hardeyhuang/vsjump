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
- **VS Code–style URLs**:
  - `vsjump://file/D:/proj/src/foo.cpp:1234:8`
  - `vsjump://file/D:\proj\src\foo.cpp:1234`
  - `vsjump://file/D:/proj/src/foo.cpp`
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

```
vsjump://file/<absolute-path>[:<line>[:<column>]]
```

- The path may use either `/` or `\`.
- Percent-encoded sequences (`%20`, etc.) are decoded as UTF-8.
- `line` and `column` are **1-based**; both are optional.

Examples:

| URL | Meaning |
|---|---|
| `vsjump://file/D:/proj/src/foo.cpp:1234:8` | foo.cpp, line 1234, column 8 |
| `vsjump://file/D:\proj\src\foo.cpp:42` | foo.cpp, line 42 |
| `vsjump://file/D:/proj/with%20space/foo.cpp:1` | percent-decoded path |

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
