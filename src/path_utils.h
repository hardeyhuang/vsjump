#pragma once

#include <string>

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

} // namespace vsjump
