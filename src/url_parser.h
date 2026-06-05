#pragma once

#include <string>

namespace vsjump {

struct VsUrl {
    std::wstring file;       // Absolute file path, e.g. "D:\proj\src\foo.cpp"
    int          line   = 0; // 1-based; 0 means unspecified.
    int          column = 0; // 1-based; 0 means unspecified.
    bool         valid  = false;
};

// Parses a vsjump URL.  Primary (VS Code-style) form:
//
//   vsjump://file/D:\path\to\foo.cpp:123:1
//   vsjump://file/D:/path/to/foo.cpp:123
//   vsjump://file/D:/path/to/foo.cpp
//
// Spaces and other special characters in the path may be percent-encoded
// (e.g. `%20`).  The "file/" authority is optional but recommended.
//
// Legacy fragment form is still accepted for backward compatibility:
//
//   vsjump://D:/path/to/foo.cpp#123
//   vsjump://D:/path/to/foo.cpp#L123:8
VsUrl ParseVsUrl(const std::wstring& raw);

} // namespace vsjump
