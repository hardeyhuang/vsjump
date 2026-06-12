#pragma once

#include <string>
#include <vector>

namespace vsjump {

// Returns the absolute, normalized path. Empty on failure.
std::wstring NormalizePath(const std::wstring& path);

// Returns true if `child` is the same as, or located under, `parent`
// (case-insensitive, after path normalization).
bool IsPathUnder(const std::wstring& child, const std::wstring& parent);

// Lower-case ASCII helper (sufficient for path drive letters / separators).
std::wstring ToLowerAscii(std::wstring s);

// Get current executable full path.
std::wstring GetExecutablePath();

// Split a path into segments, treating both '\\' and '/' as separators.
// Empty segments are dropped.
//   "C:\\foo\\bar.cpp" -> {"C:", "foo", "bar.cpp"}
//   "/builds/x/foo.cpp" -> {"builds", "x", "foo.cpp"}
std::vector<std::wstring> SplitPathSegments(const std::wstring& path);

// Number of trailing segments of `a` and `b` that match (ASCII case-insensitive).
int TrailingSegmentMatch(const std::vector<std::wstring>& a,
                         const std::vector<std::wstring>& b);

// Result of FindBestSourceMatch.
struct MatchCandidate {
    std::wstring path;             // Absolute path under one of the search roots.
    int          matched_segments; // Number of trailing path segments that match `srcfile`.
};

// Resolve `srcfile` against the directories in `roots` using ONLY a stat-based
// trailing-suffix probe — for every root we join the root with progressively
// longer trailing-segment suffixes of `srcfile` and check whether the
// resulting absolute path exists as a regular file.  The longest suffix that
// exists wins per root.
//
// We deliberately do NOT recursively walk `roots`: those are typically huge
// source trees (UE, monorepos) where a recursive walk costs many seconds,
// while a suffix probe costs O(depth) syscalls and finishes in milliseconds.
// If no destdir root yields a match, the caller is expected to fall back to
// a global filename search (e.g. via Everything).
//
// `out_best` receives the best candidate (highest matched_segments).  All
// candidates that tie for the top score are returned in `out_ties` (in the
// order encountered).  If no file matches under any root, returns false and
// `out_ties` is empty.
bool FindBestSourceMatch(const std::wstring&              srcfile,
                         const std::vector<std::wstring>& roots,
                         MatchCandidate*                  out_best,
                         std::vector<MatchCandidate>*     out_ties);

} // namespace vsjump
