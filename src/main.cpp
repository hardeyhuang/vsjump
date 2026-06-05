// VsJump — open a file in a running Visual Studio.
//
// URL shape (VS Code-style):
//   vsjump://file/D:\path\to\file.cpp:1234
//   vsjump://file/D:\path\to\file.cpp:1234:8
//   vsjump://file/D:/path/with%20space/file.cpp:1234
//
// Usage:
//   vsjump.exe register              Register the vsjump:// protocol
//   vsjump.exe unregister            Remove the vsjump:// registration
//   vsjump.exe "vsjump://file/...:1234:1"
//   vsjump.exe --help

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

void PrintConsole(const std::wstring& s) {
    if (::AttachConsole(ATTACH_PARENT_PROCESS) || ::AllocConsole()) {
        DWORD written = 0;
        ::WriteConsoleW(::GetStdHandle(STD_OUTPUT_HANDLE),
                        s.c_str(), static_cast<DWORD>(s.size()),
                        &written, nullptr);
        ::WriteConsoleW(::GetStdHandle(STD_OUTPUT_HANDLE),
                        L"\r\n", 2, &written, nullptr);
    }
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
        L"  vsjump://file/D:/proj/with%20space/foo.cpp:42\r\n");
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

int CmdOpenUrl(const std::wstring& url) {
    vsjump::VsUrl parsed = vsjump::ParseVsUrl(url);
    if (!parsed.valid) {
        Msg(L"VsJump",
            L"Could not parse the URL:\n" + url +
            L"\n\nExpected:\n"
            L"  vsjump://file/<absolute path>:<line>[:<col>]",
            MB_ICONERROR);
        return 3;
    }

    std::wstring abs_file = vsjump::NormalizePath(parsed.file);
    if (abs_file.empty()) {
        Msg(L"VsJump",
            L"Could not resolve file path:\n" + parsed.file,
            MB_ICONERROR);
        return 3;
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

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        PrintHelp();
        return 1;
    }

    std::wstring a1 = argv[1];

    if (EqIgnoreCase(a1, L"register") || EqIgnoreCase(a1, L"--register") ||
        EqIgnoreCase(a1, L"-r")) {
        return CmdRegister();
    }
    if (EqIgnoreCase(a1, L"unregister") || EqIgnoreCase(a1, L"--unregister")) {
        return CmdUnregister();
    }
    if (EqIgnoreCase(a1, L"--help") || EqIgnoreCase(a1, L"-h") ||
        EqIgnoreCase(a1, L"/?")) {
        PrintHelp();
        return 0;
    }

    return CmdOpenUrl(a1);
}
