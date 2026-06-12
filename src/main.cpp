// VsJump — open a file in a running Visual Studio.
//
// URL shapes:
//   vsjump://file/D:\path\to\file.cpp:1234:8           (direct, VS Code-style)
//   vsjump://file/D:/path/with%20space/file.cpp:42
//   vsjump://match/?srcfile=<remote>:<line>[:<col>]&destdir=<localdir>
//                                                      (resolve a non-local
//                                                       source path against
//                                                       one or more local
//                                                       source roots; falls
//                                                       back to Everything
//                                                       global search if no
//                                                       destdir match)
//
// Usage:
//   vsjump.exe register              Register the vsjump:// protocol
//   vsjump.exe unregister            Remove the vsjump:// registration
//   vsjump.exe "vsjump://file/...:1234:1"
//   vsjump.exe "vsjump://match/?srcfile=...&destdir=..."
//   vsjump.exe --help

#include "everything_search.h"
#include "path_utils.h"
#include "picker_dialog.h"
#include "protocol_register.h"
#include "url_parser.h"
#include "vs_locator.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

void Msg(const std::wstring& title, const std::wstring& body, UINT flags) {
    ::MessageBoxW(nullptr, body.c_str(), title.c_str(),
                  flags | MB_SETFOREGROUND);
}

// Print to the parent console *if* we were launched from one (cmd / pwsh).
// We never AllocConsole — we run under /SUBSYSTEM:WINDOWS specifically to
// avoid a black flash when the shell / browser invokes the protocol handler.
// When there is no parent console (typical protocol launch), `s` is shown in
// a message box instead so the user still gets the help text.
void PrintConsole(const std::wstring& s) {
    if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
        HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
        if (out && out != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            ::WriteConsoleW(out, s.c_str(),
                            static_cast<DWORD>(s.size()), &written, nullptr);
            ::WriteConsoleW(out, L"\r\n", 2, &written, nullptr);
        }
        ::FreeConsole();
        return;
    }
    ::MessageBoxW(nullptr, s.c_str(), L"VsJump",
                  MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
}

void PrintHelp() {
    PrintConsole(
        L"VsJump — jump a running Visual Studio to a (file, line)\r\n"
        L"\r\n"
        L"Commands:\r\n"
        L"  vsjump register           Register the vsjump:// URL protocol\r\n"
        L"  vsjump unregister         Remove the vsjump:// protocol registration\r\n"
        L"  vsjump <url>              Resolve the URL and jump VS to it\r\n"
        L"\r\n"
        L"URL formats:\r\n"
        L"  vsjump://file/D:\\proj\\src\\foo.cpp:1234:8     (VS Code style)\r\n"
        L"  vsjump://file/D:/proj/src/foo.cpp:1234\r\n"
        L"  vsjump://file/D:/proj/with%20space/foo.cpp:42\r\n"
        L"\r\n"
        L"  vsjump://match/?srcfile=<src>:<line>[:<col>]&destdir=<dir>\r\n"
        L"  Looks up a (typically WinDbg-reported) source path inside one\r\n"
        L"  or more local source roots by trailing-segment match. Multiple\r\n"
        L"  destdir= values are allowed.  If no destdir match is found and\r\n"
        L"  Everything.exe is running, VsJump falls back to a global\r\n"
        L"  basename search via the Everything IPC SDK.\r\n");
}

int CmdRegister() {
    std::wstring exe = vsjump::GetExecutablePath();
    std::wstring err;
    if (!vsjump::RegisterProtocol(exe, &err)) {
        Msg(L"VsJump", L"Failed to register vsjump:// protocol:\n" + err, MB_ICONERROR);
        return 2;
    }
    Msg(L"VsJump",
        L"vsjump:// protocol registered for the current user.\n\nHandler: " + exe,
        MB_ICONINFORMATION);
    return 0;
}

int CmdUnregister() {
    std::wstring err;
    if (!vsjump::UnregisterProtocol(&err)) {
        Msg(L"VsJump", L"Failed to unregister:\n" + err, MB_ICONERROR);
        return 2;
    }
    Msg(L"VsJump", L"vsjump:// protocol removed.", MB_ICONINFORMATION);
    return 0;
}

// Human-readable label for an instance (for picker / messages).
std::wstring InstanceLabel(const vsjump::VsInstance& v) {
    if (!v.solution_path.empty()) return v.solution_path;
    return L"(no solution loaded)";
}

enum class PickKind {
    None,
    BySolutionDir,
    OnlyOne,
    Foreground,
    UserChosen,
    UserCancelled,
};

struct PickResult {
    PickKind kind  = PickKind::None;
    int      index = -1;
};

PickResult PickVsForFile(const std::vector<vsjump::VsInstance>& instances,
                         const std::wstring& abs_file) {
    PickResult r;
    if (instances.empty()) return r;

    int    by_dir_idx = -1;
    size_t by_dir_len = 0;
    for (size_t i = 0; i < instances.size(); ++i) {
        const auto& ins = instances[i];
        if (ins.solution_dir.empty()) continue;
        if (vsjump::IsPathUnder(abs_file, ins.solution_dir)) {
            if (ins.solution_dir.size() > by_dir_len) {
                by_dir_len = ins.solution_dir.size();
                by_dir_idx = static_cast<int>(i);
            }
        }
    }
    if (by_dir_idx >= 0) {
        r.kind = PickKind::BySolutionDir;
        r.index = by_dir_idx;
        return r;
    }

    if (instances.size() == 1) {
        r.kind = PickKind::OnlyOne;
        r.index = 0;
        return r;
    }

    int fg = vsjump::FindForegroundInstance(instances);
    if (fg >= 0) {
        r.kind = PickKind::Foreground;
        r.index = fg;
        return r;
    }

    std::vector<std::wstring> opts;
    opts.reserve(instances.size());
    for (const auto& ins : instances) {
        opts.push_back(InstanceLabel(ins));
    }
    int chosen = vsjump::PickFromList(
        L"VsJump — Choose Visual Studio",
        L"Select which Visual Studio should open this file:",
        L"File:\n" + abs_file,
        opts,
        /*default_index=*/0);

    if (chosen < 0) {
        r.kind = PickKind::UserCancelled;
        return r;
    }
    r.kind = PickKind::UserChosen;
    r.index = chosen;
    return r;
}

// Resolve `parsed` (which may be Match-kind) into an absolute local file path.
// Returns empty wstring on failure (and shows a diagnostic message box).
// `out_rc` receives the exit code to use on failure.
std::wstring ResolveTargetFile(const vsjump::VsUrl& parsed, int* out_rc) {
    if (parsed.kind == vsjump::VsUrlKind::Direct) {
        std::wstring abs_file = vsjump::NormalizePath(parsed.file);
        if (abs_file.empty()) {
            Msg(L"VsJump",
                L"Could not resolve file path:\n" + parsed.file,
                MB_ICONERROR);
            *out_rc = 3;
        }
        return abs_file;
    }

    // --- Match kind ---------------------------------------------------------
    // Step 1: try the user-specified destdir roots via stat-based trailing
    //         suffix probe (no recursive scans).
    vsjump::MatchCandidate              best;
    std::vector<vsjump::MatchCandidate> ties;
    bool found_in_dirs =
        !parsed.match_dirs.empty() &&
        vsjump::FindBestSourceMatch(parsed.file, parsed.match_dirs,
                                    &best, &ties);

    if (found_in_dirs) {
        // Unique best — open directly.
        if (ties.size() == 1) {
            return vsjump::NormalizePath(best.path);
        }
        // Multiple destdir roots produced files tied for the highest
        // trailing-segment match — ask the user which one they want.
        std::vector<std::wstring> opts;
        opts.reserve(ties.size());
        for (const auto& c : ties) opts.push_back(c.path);

        int chosen = vsjump::PickFromList(
            L"VsJump — Multiple matches",
            L"Several local files match the source path equally well. "
            L"Pick which one to open:",
            L"Source:\n" + parsed.file,
            opts,
            /*default_index=*/0);
        if (chosen < 0) {
            *out_rc = 0;  // user cancelled
            return {};
        }
        return vsjump::NormalizePath(ties[static_cast<size_t>(chosen)].path);
    }

    // Step 2: no destdir match — fall back to a global Everything lookup.
    //         Unlike the destdir path, we ALWAYS show a picker here (even for
    //         a single result), because Everything matches any file with the
    //         same basename anywhere on disk and the user should confirm.
    vsjump::EverythingSearchResult ev =
        vsjump::SearchWithEverything(parsed.file);

    if (!ev.succeeded || ev.ties.empty()) {
        std::wstring roots;
        if (parsed.match_dirs.empty()) {
            roots = L"  (none provided)";
        } else {
            for (const auto& d : parsed.match_dirs) {
                if (!roots.empty()) roots += L"\n";
                roots += L"  " + d;
            }
        }
        std::wstring extra;
        if (!ev.attempted) {
            extra = L"\n\nGlobal lookup skipped: " + ev.unavailable_reason +
                    L"\n(Run Everything.exe to enable a system-wide "
                    L"basename search.)";
        } else {
            extra = L"\n\nEverything was queried but returned no files "
                    L"with a matching basename.";
        }
        Msg(L"VsJump — no match",
            L"No file matching the source path was found.\n\nSource:\n  " +
            parsed.file + L"\n\nSearched roots:\n" + roots + extra,
            MB_ICONWARNING);
        *out_rc = 7;
        return {};
    }

    // Everything returned candidates — already sorted by trailing-segment
    // match length (highest first). Show all of them and let the user pick.
    std::vector<std::wstring> opts;
    opts.reserve(ev.ties.size());
    for (const auto& c : ev.ties) {
        // Annotate the option with its trailing-match score so the user can
        // tell at a glance which candidates share the most context.
        wchar_t scorebuf[32];
        ::swprintf_s(scorebuf, L" [match: %d]", c.matched_segments);
        opts.push_back(c.path + scorebuf);
    }

    int chosen = vsjump::PickFromList(
        L"VsJump — Everything global matches",
        L"No file matched under the specified destdir roots. "
        L"Pick a file from the global Everything index:",
        L"Source:\n" + parsed.file +
        L"\n\nResults are ordered by how many trailing path segments match "
        L"the source — longest match on top.",
        opts,
        /*default_index=*/0);
    if (chosen < 0) {
        *out_rc = 0;  // user cancelled
        return {};
    }
    return vsjump::NormalizePath(
        ev.ties[static_cast<size_t>(chosen)].path);
}

int CmdOpenUrl(const std::wstring& url) {
    vsjump::VsUrl parsed = vsjump::ParseVsUrl(url);
    if (!parsed.valid()) {
        Msg(L"VsJump",
            L"Could not parse the URL:\n" + url +
            L"\n\nExpected one of:\n"
            L"  vsjump://file/<absolute path>:<line>[:<col>]\n"
            L"  vsjump://match/?srcfile=<path>:<line>[:<col>]&destdir=<dir>",
            MB_ICONERROR);
        return 3;
    }

    int          rc_resolve = 0;
    std::wstring abs_file   = ResolveTargetFile(parsed, &rc_resolve);
    if (abs_file.empty()) {
        return rc_resolve;
    }

    if (FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        Msg(L"VsJump", L"CoInitializeEx failed.", MB_ICONERROR);
        return 4;
    }

    auto instances = vsjump::EnumerateVsInstances();
    int rc = 0;

    if (instances.empty()) {
        Msg(L"VsJump — no Visual Studio running",
            L"No running Visual Studio instance was detected.\n\n"
            L"Please start Visual Studio (and load the relevant solution) "
            L"and try again.\n\nFile:\n" + abs_file,
            MB_ICONWARNING);
        rc = 5;
    } else {
        PickResult pick = PickVsForFile(instances, abs_file);

        if (pick.kind == PickKind::UserCancelled) {
            rc = 0;
        } else if (pick.index < 0) {
            Msg(L"VsJump", L"Could not select a Visual Studio instance.",
                MB_ICONWARNING);
            rc = 5;
        } else {
            const auto& chosen = instances[pick.index];
            std::wstring err;
            if (!vsjump::OpenFileInVs(chosen.dte, abs_file,
                                      parsed.line, parsed.column, &err)) {
                Msg(L"VsJump",
                    L"Failed to open file in Visual Studio:\n" + abs_file +
                    L"\n\nTarget: " + InstanceLabel(chosen) +
                    L"\n\n" + err,
                    MB_ICONERROR);
                rc = 6;
            } else {
                vsjump::ActivateVsMainWindow(chosen.dte);
            }
        }
    }

    vsjump::FreeInstances(instances);
    ::CoUninitialize();
    return rc;
}

bool EqIgnoreCase(const std::wstring& a, const wchar_t* b) {
    size_t n = wcslen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; ++i) {
        wchar_t x = a[i], y = b[i];
        if (x >= L'A' && x <= L'Z') x = static_cast<wchar_t>(x - L'A' + L'a');
        if (y >= L'A' && y <= L'Z') y = static_cast<wchar_t>(y - L'A' + L'a');
        if (x != y) return false;
    }
    return true;
}

} // namespace

int APIENTRY wWinMain(HINSTANCE /*hInstance*/, HINSTANCE /*hPrevInstance*/,
                      LPWSTR /*lpCmdLine*/, int /*nShowCmd*/) {
    int      argc  = 0;
    LPWSTR*  argv  = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (!argv) {
        Msg(L"VsJump", L"Failed to parse command line.", MB_ICONERROR);
        return 1;
    }

    auto cleanup = [&](int rc) -> int {
        ::LocalFree(argv);
        return rc;
    };

    if (argc < 2) {
        PrintHelp();
        return cleanup(1);
    }

    std::wstring a1 = argv[1];

    if (EqIgnoreCase(a1, L"register") || EqIgnoreCase(a1, L"--register") ||
        EqIgnoreCase(a1, L"-r")) {
        return cleanup(CmdRegister());
    }
    if (EqIgnoreCase(a1, L"unregister") || EqIgnoreCase(a1, L"--unregister")) {
        return cleanup(CmdUnregister());
    }
    if (EqIgnoreCase(a1, L"--help") || EqIgnoreCase(a1, L"-h") ||
        EqIgnoreCase(a1, L"/?")) {
        PrintHelp();
        return cleanup(0);
    }

    return cleanup(CmdOpenUrl(a1));
}
