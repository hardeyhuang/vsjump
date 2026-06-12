#pragma once

#include "path_utils.h"

#include <string>
#include <vector>

namespace vsjump {

// Result of a global Everything-based filename lookup.
struct EverythingSearchResult {
    // True if Everything.exe was detected as running AND we managed to load
    // Everything64.dll AND the IPC query succeeded.  When false, the global
    // search step was effectively skipped — `unavailable_reason` may carry a
    // human-readable explanation for diagnostics.
    bool        attempted          = false;
    bool        succeeded          = false;
    std::wstring unavailable_reason;

    // Files (absolute paths) returned by Everything whose basename equals the
    // basename of the query path, ranked by trailing-segment match length
    // (highest first).  ALL matches are kept — the caller is expected to
    // present them to the user so they can pick the right one.  Within the
    // same score, files are listed in the order Everything returned them.
    std::vector<MatchCandidate> ties;
};

// Try to resolve `srcfile` globally using a running Everything instance.
//
// Behavior:
//   1. If `Everything.exe` is not running on this machine (heuristic: an IPC
//      window class is present), returns with `attempted=false`.
//   2. Otherwise loads `Everything64.dll` (looked up first next to the
//      executable, then via the OS search path), issues an IPC query for the
//      basename, ranks results by trailing-segment match against `srcfile`,
//      and fills `ties` with the top-scoring candidates.
//
// `srcfile` is expected to be a full path; only its basename is used for the
// initial query, but the full segments are used for ranking.
EverythingSearchResult SearchWithEverything(const std::wstring& srcfile);

} // namespace vsjump
