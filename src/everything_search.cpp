#include "everything_search.h"

#include "path_utils.h"

#include <windows.h>

#include <algorithm>
#include <utility>
#include <vector>

// We deliberately don't link against Everything64.lib — every entry point we
// need is resolved at runtime via GetProcAddress, so VsJump still works on
// machines where Everything is not installed.

namespace vsjump {

namespace {

// Subset of the Everything SDK API that we actually call.
using Fn_Reset            = void(__stdcall*)(void);
using Fn_CleanUp          = void(__stdcall*)(void);
using Fn_SetSearchW       = void(__stdcall*)(LPCWSTR);
using Fn_SetMatchPath     = void(__stdcall*)(BOOL);
using Fn_SetMatchCase     = void(__stdcall*)(BOOL);
using Fn_SetMatchWholeWord= void(__stdcall*)(BOOL);
using Fn_SetRegex         = void(__stdcall*)(BOOL);
using Fn_SetMax           = void(__stdcall*)(DWORD);
using Fn_SetRequestFlags  = void(__stdcall*)(DWORD);
using Fn_QueryW           = BOOL(__stdcall*)(BOOL);
using Fn_GetLastError     = DWORD(__stdcall*)(void);
using Fn_GetNumResults    = DWORD(__stdcall*)(void);
using Fn_IsFileResult     = BOOL(__stdcall*)(DWORD);
using Fn_IsVolumeResult   = BOOL(__stdcall*)(DWORD);
using Fn_IsFolderResult   = BOOL(__stdcall*)(DWORD);
using Fn_GetResultFullPathNameW = DWORD(__stdcall*)(DWORD, LPWSTR, DWORD);
using Fn_GetResultFileNameW     = LPCWSTR(__stdcall*)(DWORD);

// Mirrored from <Everything.h> so we don't need the header at translation
// time of this TU's callers (and we avoid any chance of accidental implicit
// linkage if someone adds the .lib later).
constexpr DWORD kEverythingErrorIpc            = 2;
constexpr DWORD kEverythingRequestFullPathName = 0x00000004; // FULL_PATH_AND_FILE_NAME
constexpr DWORD kEverythingRequestFileName     = 0x00000001;

struct EvApi {
    HMODULE                  dll = nullptr;
    Fn_Reset                 Reset = nullptr;
    Fn_CleanUp               CleanUp = nullptr;
    Fn_SetSearchW            SetSearchW = nullptr;
    Fn_SetMatchPath          SetMatchPath = nullptr;
    Fn_SetMatchCase          SetMatchCase = nullptr;
    Fn_SetMatchWholeWord     SetMatchWholeWord = nullptr;
    Fn_SetRegex              SetRegex = nullptr;
    Fn_SetMax                SetMax = nullptr;
    Fn_SetRequestFlags       SetRequestFlags = nullptr;
    Fn_QueryW                QueryW = nullptr;
    Fn_GetLastError          GetLastError_ = nullptr;
    Fn_GetNumResults         GetNumResults = nullptr;
    Fn_IsFileResult          IsFileResult = nullptr;
    Fn_GetResultFullPathNameW GetResultFullPathNameW = nullptr;
    Fn_GetResultFileNameW    GetResultFileNameW = nullptr;

    bool ok() const {
        return dll && Reset && CleanUp && SetSearchW && SetMatchPath &&
               SetMatchCase && SetMax && SetRequestFlags && QueryW &&
               GetLastError_ && GetNumResults && IsFileResult &&
               GetResultFullPathNameW && GetResultFileNameW;
    }
};

// RAII wrapper to make sure FreeLibrary always runs.
class EvDllHandle {
public:
    EvDllHandle() = default;
    explicit EvDllHandle(HMODULE h) : h_(h) {}
    ~EvDllHandle() { if (h_) ::FreeLibrary(h_); }
    EvDllHandle(const EvDllHandle&)            = delete;
    EvDllHandle& operator=(const EvDllHandle&) = delete;
    EvDllHandle(EvDllHandle&& o) noexcept : h_(o.h_) { o.h_ = nullptr; }
    HMODULE get() const { return h_; }

private:
    HMODULE h_ = nullptr;
};

// Look for `Everything64.dll` next to vsjump.exe first, then via the OS
// search path.  Returns nullptr if not found / failed to load.
HMODULE LoadEverythingDll() {
    std::wstring exe = GetExecutablePath();
    if (!exe.empty()) {
        size_t slash = exe.find_last_of(L"\\/");
        std::wstring dir = (slash == std::wstring::npos) ? std::wstring()
                                                         : exe.substr(0, slash + 1);
        std::wstring candidate = dir + L"Everything64.dll";
        HMODULE h = ::LoadLibraryW(candidate.c_str());
        if (h) return h;
    }
    return ::LoadLibraryW(L"Everything64.dll");
}

template <typename Fn>
bool BindProc(HMODULE dll, const char* name, Fn& out) {
    out = reinterpret_cast<Fn>(::GetProcAddress(dll, name));
    return out != nullptr;
}

bool BindApi(HMODULE dll, EvApi& a) {
    a.dll = dll;
    bool ok = true;
    ok &= BindProc(dll, "Everything_Reset",                 a.Reset);
    ok &= BindProc(dll, "Everything_CleanUp",               a.CleanUp);
    ok &= BindProc(dll, "Everything_SetSearchW",            a.SetSearchW);
    ok &= BindProc(dll, "Everything_SetMatchPath",          a.SetMatchPath);
    ok &= BindProc(dll, "Everything_SetMatchCase",          a.SetMatchCase);
    ok &= BindProc(dll, "Everything_SetMatchWholeWord",     a.SetMatchWholeWord);
    ok &= BindProc(dll, "Everything_SetRegex",              a.SetRegex);
    ok &= BindProc(dll, "Everything_SetMax",                a.SetMax);
    ok &= BindProc(dll, "Everything_SetRequestFlags",       a.SetRequestFlags);
    ok &= BindProc(dll, "Everything_QueryW",                a.QueryW);
    ok &= BindProc(dll, "Everything_GetLastError",          a.GetLastError_);
    ok &= BindProc(dll, "Everything_GetNumResults",         a.GetNumResults);
    ok &= BindProc(dll, "Everything_IsFileResult",          a.IsFileResult);
    ok &= BindProc(dll, "Everything_GetResultFullPathNameW",a.GetResultFullPathNameW);
    ok &= BindProc(dll, "Everything_GetResultFileNameW",    a.GetResultFileNameW);
    return ok;
}

bool EqIgnoreCaseAscii(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        wchar_t x = a[i], y = b[i];
        if (x >= L'A' && x <= L'Z') x = static_cast<wchar_t>(x - L'A' + L'a');
        if (y >= L'A' && y <= L'Z') y = static_cast<wchar_t>(y - L'A' + L'a');
        if (x != y) return false;
    }
    return true;
}

// Quick "is Everything running?" probe via its IPC window class.
// Cheap; doesn't load the DLL.
bool IsEverythingRunning() {
    // Both class names are registered by Everything.exe; checking either is
    // enough.  EVERYTHING_IPC_WNDCLASS is L"EVERYTHING" in the SDK.
    if (::FindWindowW(L"EVERYTHING", nullptr)) return true;
    if (::FindWindowW(L"EVERYTHING_TASKBAR_NOTIFICATION", nullptr)) return true;
    return false;
}

} // namespace

EverythingSearchResult SearchWithEverything(const std::wstring& srcfile) {
    EverythingSearchResult r;

    if (srcfile.empty()) {
        r.unavailable_reason = L"Empty source path.";
        return r;
    }

    if (!IsEverythingRunning()) {
        r.unavailable_reason = L"Everything.exe is not running.";
        return r;
    }

    EvDllHandle dll(LoadEverythingDll());
    if (!dll.get()) {
        r.unavailable_reason = L"Could not load Everything64.dll.";
        return r;
    }

    EvApi api{};
    if (!BindApi(dll.get(), api) || !api.ok()) {
        r.unavailable_reason =
            L"Everything64.dll is missing required exports (version mismatch?).";
        return r;
    }

    r.attempted = true;

    std::vector<std::wstring> src_segs = SplitPathSegments(srcfile);
    if (src_segs.empty()) {
        r.unavailable_reason = L"Could not derive a basename from source path.";
        return r;
    }
    const std::wstring& basename = src_segs.back();

    api.Reset();
    api.SetMatchPath(FALSE);     // search by name only
    api.SetMatchCase(FALSE);
    if (api.SetMatchWholeWord) api.SetMatchWholeWord(FALSE);
    if (api.SetRegex)          api.SetRegex(FALSE);
    api.SetMax(1024);
    api.SetRequestFlags(kEverythingRequestFullPathName | kEverythingRequestFileName);

    // Wrap the basename in quotes so spaces are treated as a single token.
    std::wstring search = L"\"" + basename + L"\"";
    api.SetSearchW(search.c_str());

    if (!api.QueryW(TRUE)) {
        DWORD last = api.GetLastError_();
        if (last == kEverythingErrorIpc) {
            r.attempted = false;  // service unexpectedly went away
            r.unavailable_reason = L"Everything IPC unavailable.";
        } else {
            r.unavailable_reason = L"Everything query failed.";
        }
        api.CleanUp();
        return r;
    }

    DWORD count = api.GetNumResults();

    std::vector<MatchCandidate> all;
    all.reserve(count);
    for (DWORD i = 0; i < count; ++i) {
        if (!api.IsFileResult(i)) continue;

        // Filter to *exact* basename (Everything is substring-by-default).
        LPCWSTR fname = api.GetResultFileNameW(i);
        if (!fname) continue;
        if (!EqIgnoreCaseAscii(fname, basename)) continue;

        wchar_t buf[MAX_PATH * 4] = {};
        DWORD   wrote = api.GetResultFullPathNameW(i, buf,
                            static_cast<DWORD>(_countof(buf)));
        if (wrote == 0) continue;

        std::wstring full(buf);
        std::vector<std::wstring> hsegs = SplitPathSegments(full);
        int score = TrailingSegmentMatch(hsegs, src_segs);

        MatchCandidate c{full, score};
        all.push_back(std::move(c));
    }

    api.CleanUp();

    if (all.empty()) {
        r.succeeded = false;
        return r;
    }

    // Sort by trailing-segment score, highest first.  std::stable_sort
    // preserves Everything's original ordering for ties (which is roughly
    // the on-disk indexing order — close enough to "alphabetical-ish").
    std::stable_sort(all.begin(), all.end(),
                     [](const MatchCandidate& a, const MatchCandidate& b) {
                         return a.matched_segments > b.matched_segments;
                     });

    r.ties     = std::move(all);
    r.succeeded = !r.ties.empty();
    return r;
}

} // namespace vsjump
