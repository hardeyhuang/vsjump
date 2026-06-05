# VsJump

A tiny Windows utility that registers a `vsjump://` URL handler so that any
application can ask **the Visual Studio instance that already has the matching
solution open** to jump to a particular file and line.

```
vsjump://D:/path/to/file.cpp#1343
```

## Features

1. **`register` / `unregister`** — installs/removes a `vsjump://` protocol handler
   in `HKEY_CURRENT_USER\Software\Classes\vsjump` (no admin rights required).
2. **URL parsing** — accepts both `D:/foo/bar.cpp` and `D:\foo\bar.cpp` styles,
   percent-decoded, with a `#<line>` (or `#L<line>:<col>`) fragment.
3. **Smart routing** — enumerates all running Visual Studio instances via the
   COM Running Object Table (`!VisualStudio.DTE.*`), reads each one's loaded
   `Solution.FullName`, and picks the instance whose solution directory
   contains the requested file. If none matches, a message box is shown and
   *no* new VS is started.
4. **Jumping** — uses `EnvDTE`'s `ItemOperations.OpenFile` + `Selection.GotoLine`
   to open the file and move the caret, then activates VS's main window.

## Build

Requires CMake 3.15+ and MSVC (x64 recommended):

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

The resulting executable is `build\Release\vsjump.exe`.

## Install / register the protocol

```powershell
# Run once after building, from where you want vsjump.exe to live permanently.
.\build\Release\vsjump.exe register
```

This writes:

```
HKCU\Software\Classes\vsjump\(default)              = "URL:VS Jump Protocol"
HKCU\Software\Classes\vsjump\URL Protocol           = ""
HKCU\Software\Classes\vsjump\DefaultIcon\(default)  = "<exe>",0
HKCU\Software\Classes\vsjump\shell\open\command\(default) = "<exe>" "%1"
```

To remove:

```powershell
.\vsjump.exe unregister
```

## Usage

After registering, anything that can launch a URL works:

```powershell
# From a shell:
start vsjump://D:/proj/src/main.cpp#42

# From another program (CreateProcess / ShellExecute):
ShellExecuteW(nullptr, L"open", L"vsjump://D:/proj/src/main.cpp#42", nullptr, nullptr, SW_SHOW);
```

You can also invoke the parser directly without going through the shell:

```powershell
.\vsjump.exe "vsjump://D:/proj/src/main.cpp#42"
```

### URL grammar

```
vsjump://<absolute-path>[#[L]<line>[:<column>]]
```

Examples:

* `vsjump://D:/proj/src/foo.cpp#1234`
* `vsjump://D:\proj\src\foo.cpp#L1234`
* `vsjump://D:/proj/src/foo.cpp#1234:8`

The path may use either `/` or `\`. URL-encoded sequences (`%20`, etc.) are
decoded as UTF-8.

## How the target VS is chosen

For each running VS the tool reads `DTE.Solution.FullName` and the main-window
`HWND`. The chosen VS is decided in this order:

1. **Solution-directory match** — if the file's absolute path is under (or
   equal to) some VS's solution directory (case-insensitive), that VS wins.
   With multiple matches the **deepest** (most specific) directory wins.
2. **Single VS** — if no solution dir matches but only one VS is running, use
   it directly.
3. **Foreground VS** — if multiple VS are running and one of them is currently
   in the foreground, use that one (the assumption being "the IDE the user is
   actively working in").
4. **User picker** — if multiple VS are running and none of them is in the
   foreground, a small TaskDialog appears with one radio button per running
   solution; the user picks one (or cancels).

If no VS is running, a warning is shown and **no new IDE is started**.

## Known limitations

* Only files inside the **solution directory tree** are considered. Files
  added to a solution from elsewhere (linked items) won't be matched. Adapt
  `vs_locator.cpp` to walk `Solution.Projects` if you need that.
* Project enumeration via `Solution.Projects` is not done — it adds a lot of
  COM plumbing for marginal benefit. The directory heuristic covers the
  typical "src tree under the sln" layout.
* Visual Studio Code is **not** supported (it is not a DTE host).
* A 32-bit `vsjump.exe` can still talk to a 64-bit VS through the ROT — there
  is no bitness coupling, COM does the marshalling.

## License

MIT
