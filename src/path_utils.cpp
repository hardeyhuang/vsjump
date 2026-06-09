#include "path_utils.h"

#include <windows.h>
#include <shlwapi.h>

#include <algorithm>
#include <vector>

namespace vsjump {

std::wstring ToLowerAscii(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) {
        if (c >= L'A' && c <= L'Z') return static_cast<wchar_t>(c - L'A' + L'a');
        return c;
    });
    return s;
}

static std::wstring NormalizeSeparators(std::wstring s) {
    for (auto& c : s) {
        if (c == L'/') c = L'\\';
    }
    return s;
}

std::wstring NormalizePath(const std::wstring& path) {
    if (path.empty()) return {};

    std::wstring input = NormalizeSeparators(path);

    // GetFullPathNameW resolves ".", ".." and relative paths.
    DWORD needed = ::GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
    if (needed == 0) return {};

    std::vector<wchar_t> buf(needed);
    DWORD written = ::GetFullPathNameW(input.c_str(), needed, buf.data(), nullptr);
    if (written == 0 || written >= needed) return {};

    std::wstring full(buf.data(), written);

    // Strip trailing backslash (except for drive root like "C:\").
    while (full.size() > 3 && full.back() == L'\\') {
        full.pop_back();
    }
    return full;
}

bool IsPathUnder(const std::wstring& child, const std::wstring& parent) {
    if (child.empty() || parent.empty()) return false;

    std::wstring c = ToLowerAscii(NormalizePath(child));
    std::wstring p = ToLowerAscii(NormalizePath(parent));
    if (c.empty() || p.empty()) return false;

    if (c == p) return true;

    // Ensure parent ends with separator so "C:\foo" doesn't match "C:\foobar\..."
    if (p.back() != L'\\') p.push_back(L'\\');

    if (c.size() < p.size()) return false;
    return c.compare(0, p.size(), p) == 0;
}

std::wstring GetExecutablePath() {
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        DWORD n = ::GetModuleFileNameW(nullptr, buf.data(),
                                       static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) return std::wstring(buf.data(), n);
        buf.resize(buf.size() * 2);
    }
}

// ---------------------------------------------------------------------------
// Source-tree matching (used by `vsjump://match/?...`).
// ---------------------------------------------------------------------------

namespace {

// Split a normalized path into segments by '\\'. Empty segments dropped.
// "C:\foo\bar.cpp" -> {"C:", "foo", "bar.cpp"}
// "/builds/x/foo.cpp" -> {"builds", "x", "foo.cpp"}
std::vector<std::wstring> SplitSegments(const std::wstring& path) {
    std::vector<std::wstring> segs;
    std::wstring              cur;
    for (wchar_t c : path) {
        if (c == L'\\' || c == L'/') {
            if (!cur.empty()) {
                segs.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) segs.push_back(cur);
    return segs;
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

// Number of trailing segments of `a` and `b` that match (case-insensitive).
int TrailingMatchCount(const std::vector<std::wstring>& a,
                       const std::vector<std::wstring>& b) {
    int n = 0;
    int ia = static_cast<int>(a.size()) - 1;
    int ib = static_cast<int>(b.size()) - 1;
    while (ia >= 0 && ib >= 0 && EqIgnoreCaseAscii(a[ia], b[ib])) {
        ++n;
        --ia;
        --ib;
    }
    return n;
}

// Recursively walk `dir` and append every regular file whose basename equals
// `basename` (case-insensitive) to `out`.
void CollectByBasename(const std::wstring&        dir,
                       const std::wstring&        basename,
                       std::vector<std::wstring>& out) {
    std::wstring pattern = dir;
    if (!pattern.empty() && pattern.back() != L'\\') pattern.push_back(L'\\');
    pattern.push_back(L'*');

    WIN32_FIND_DATAW fd{};
    HANDLE           h = ::FindFirstFileExW(pattern.c_str(),
                                            FindExInfoBasic,
                                            &fd,
                                            FindExSearchNameMatch,
                                            nullptr,
                                            FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        const wchar_t* name = fd.cFileName;
        if (name[0] == L'.' &&
            (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0'))) {
            continue;
        }

        std::wstring full = dir;
        if (full.empty() || full.back() != L'\\') full.push_back(L'\\');
        full += name;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // Skip reparse points to avoid following symlinks/junctions in loops.
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            CollectByBasename(full, basename, out);
        } else {
            std::wstring leaf = name;
            if (EqIgnoreCaseAscii(leaf, basename)) {
                out.push_back(full);
            }
        }
    } while (::FindNextFileW(h, &fd));

    ::FindClose(h);
}

} // namespace

bool FindBestSourceMatch(const std::wstring&              srcfile,
                         const std::vector<std::wstring>& roots,
                         MatchCandidate*                  out_best,
                         std::vector<MatchCandidate>*     out_ties) {
    if (out_ties) out_ties->clear();
    if (out_best) *out_best = {};

    if (srcfile.empty() || roots.empty()) return false;

    std::vector<std::wstring> src_segs = SplitSegments(srcfile);
    if (src_segs.empty()) return false;
    const std::wstring& basename = src_segs.back();

    // 1. Collect all files with the matching basename across all roots.
    std::vector<std::wstring> hits;
    for (const auto& root : roots) {
        std::wstring abs_root = NormalizePath(root);
        if (abs_root.empty()) continue;

        DWORD attr = ::GetFileAttributesW(abs_root.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) continue;
        if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) continue;

        CollectByBasename(abs_root, basename, hits);
    }

    if (hits.empty()) return false;

    // 2. Score each hit by trailing-segment match length.
    int                         best_score = -1;
    std::vector<MatchCandidate> ties;
    for (const auto& h : hits) {
        std::vector<std::wstring> hsegs = SplitSegments(h);
        int                       n     = TrailingMatchCount(hsegs, src_segs);
        if (n > best_score) {
            best_score = n;
            ties.clear();
            ties.push_back({h, n});
        } else if (n == best_score) {
            ties.push_back({h, n});
        }
    }

    if (out_ties) *out_ties = ties;
    if (out_best && !ties.empty()) *out_best = ties.front();
    return !ties.empty();
}

} // namespace vsjump
