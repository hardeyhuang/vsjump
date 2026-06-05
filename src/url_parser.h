#pragma once

#include <string>

namespace vsjump {

struct VsUrl {
    std::wstring file;       // Absolute file path, e.g. "D:\proj\src\foo.cpp"
    int          line   = 0; // 1-based; 0 means unspecified.
    int          column = 0; // 1-based; 0 means unspecified.
    bool         valid  = false;
};

// Parses a vsjump URL (VS Code-style):
//
//   vsjump://file/D:\path\to\foo.cpp:123:1
//   vsjump://file/D:/path/to/foo.cpp:123
//   vsjump://file/D:/path/to/foo.cpp
//
// Spaces and other special characters in the path may be percent-encoded
// (e.g. `%20`).  The "file/" authority is optional but recommended.
VsUrl ParseVsUrl(const std::wstring& raw);

} // namespace vsjump
