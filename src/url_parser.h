#pragma once

#include <string>
#include <vector>

namespace vsjump {

enum class VsUrlKind {
    Invalid,
    // Direct file form:  vsjump://file/<absolute-path>[:<line>[:<col>]]
    Direct,
    // Match form:        vsjump://match/?srcfile=<path>[:<line>[:<col>]]
    //                                   &destdir=<localdir>[&destdir=...]
    // The `file` field holds the source-side path (typically a build-server
    // path that does not exist locally); `match_dirs` holds local search roots.
    Match,
};

struct VsUrl {
    VsUrlKind                 kind   = VsUrlKind::Invalid;
    std::wstring              file;        // Source path (Direct: absolute local; Match: build-side).
    int                       line   = 0;  // 1-based; 0 means unspecified.
    int                       column = 0;  // 1-based; 0 means unspecified.
    std::vector<std::wstring> match_dirs;  // Only for Match kind.

    bool valid() const { return kind != VsUrlKind::Invalid; }
};

// Parses a vsjump URL.
//
// Direct form (VS Code-style):
//
//   vsjump://file/D:\path\to\foo.cpp:123:1
//   vsjump://file/D:/path/to/foo.cpp:123
//   vsjump://file/D:/path/to/foo.cpp
//
// Match form (resolve a non-local source path against local source roots):
//
//   vsjump://match/?srcfile=c:\build\agent\src\foo.cpp:123:8&destdir=D:\repo
//   vsjump://match/?srcfile=/builds/x/foo.cpp:123&destdir=D:\repoA&destdir=D:\repoB
//
// Spaces and other special characters may be percent-encoded (`%20` etc.).
VsUrl ParseVsUrl(const std::wstring& raw);

} // namespace vsjump
