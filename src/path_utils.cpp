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

} // namespace vsjump
