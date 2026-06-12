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

std::vector<std::wstring> SplitPathSegments(const std::wstring& path) {
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

namespace {

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

} // namespace

int TrailingSegmentMatch(const std::vector<std::wstring>& a,
                         const std::vector<std::wstring>& b) {
    int n  = 0;
    int ia = static_cast<int>(a.size()) - 1;
    int ib = static_cast<int>(b.size()) - 1;
    while (ia >= 0 && ib >= 0 && EqIgnoreCaseAscii(a[ia], b[ib])) {
        ++n;
        --ia;
        --ib;
    }
    return n;
}

namespace {

// Fast path: try to resolve `srcfile` directly inside `root` by joining the
// root with progressively longer trailing-segment suffixes of `srcfile` and
// checking whether the resulting absolute path exists as a regular file.
//
// Example: srcfile = "G:/.../Engine/Source/Runtime/Engine/Private/Materials/Foo.cpp"
//          root    = "D:\\UnrealEngine"
//          tries:   "D:\\UnrealEngine\\Foo.cpp"
//                   "D:\\UnrealEngine\\Materials\\Foo.cpp"
//                   "D:\\UnrealEngine\\Private\\Materials\\Foo.cpp"
//                   ...
//          stops on the first hit (longest suffix that exists).
//
// On hit, appends the absolute path to `out`.  Returns true iff a hit was
// found.  This is the *only* matching strategy used against destdir roots —
// we deliberately do NOT recursively walk the tree, because UE / monorepo
// roots are routinely huge (millions of files) and a stat-based suffix probe
// finishes in O(depth) syscalls.
bool TryDirectSuffixMatch(const std::wstring&              root,
                          const std::vector<std::wstring>& src_segs,
                          std::vector<std::wstring>&       out) {
    if (src_segs.empty()) return false;

    std::wstring base = root;
    if (!base.empty() && base.back() != L'\\') base.push_back(L'\\');

    // Try suffixes from longest to shortest. Longest first means we prefer a
    // hit deep inside the tree (more specific / higher trailing-segment score).
    const size_t total = src_segs.size();
    for (size_t take = total; take >= 1; --take) {
        std::wstring candidate = base;
        for (size_t i = total - take; i < total; ++i) {
            candidate += src_segs[i];
            if (i + 1 < total) candidate.push_back(L'\\');
        }

        DWORD attr = ::GetFileAttributesW(candidate.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES &&
            !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            out.push_back(candidate);
            return true;
        }
    }
    return false;
}

} // namespace

bool FindBestSourceMatch(const std::wstring&              srcfile,
                         const std::vector<std::wstring>& roots,
                         MatchCandidate*                  out_best,
                         std::vector<MatchCandidate>*     out_ties) {
    if (out_ties) out_ties->clear();
    if (out_best) *out_best = {};

    if (srcfile.empty() || roots.empty()) return false;

    std::vector<std::wstring> src_segs = SplitPathSegments(srcfile);
    if (src_segs.empty()) return false;

    // Pre-resolve roots once, drop ones that don't exist or aren't directories.
    std::vector<std::wstring> abs_roots;
    abs_roots.reserve(roots.size());
    for (const auto& root : roots) {
        std::wstring abs_root = NormalizePath(root);
        if (abs_root.empty()) continue;
        DWORD attr = ::GetFileAttributesW(abs_root.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) continue;
        if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) continue;
        abs_roots.push_back(std::move(abs_root));
    }
    if (abs_roots.empty()) return false;

    // The only strategy: stat-based trailing-suffix probe for every root.
    // No recursive directory walks — those are pathological on UE-sized trees.
    std::vector<std::wstring> hits;
    for (const auto& abs_root : abs_roots) {
        TryDirectSuffixMatch(abs_root, src_segs, hits);
    }

    if (hits.empty()) return false;

    // De-duplicate (the same file path could in principle surface twice if
    // two destdir entries resolve to nested directories).
    {
        std::vector<std::wstring> uniq;
        uniq.reserve(hits.size());
        for (auto& h : hits) {
            std::wstring key = ToLowerAscii(h);
            bool         dup = false;
            for (const auto& u : uniq) {
                if (ToLowerAscii(u) == key) { dup = true; break; }
            }
            if (!dup) uniq.push_back(h);
        }
        hits = std::move(uniq);
    }

    // Score each hit by trailing-segment match length and return only the
    // best-scoring set (caller will disambiguate any ties via a picker).
    int                         best_score = -1;
    std::vector<MatchCandidate> ties;
    for (const auto& h : hits) {
        std::vector<std::wstring> hsegs = SplitPathSegments(h);
        int                       n     = TrailingSegmentMatch(hsegs, src_segs);
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
