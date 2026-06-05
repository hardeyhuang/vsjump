#include "url_parser.h"

#include <windows.h>

#include <string>

namespace vsjump {

namespace {

int HexVal(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

// Percent-decode treating the bytes as UTF-8 (which is what browsers emit).
std::wstring PercentDecodeUtf8(const std::wstring& s) {
    std::string bytes;
    bytes.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        wchar_t c = s[i];
        if (c == L'%' && i + 2 < s.size()) {
            int hi = HexVal(s[i + 1]);
            int lo = HexVal(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                bytes.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        if (c < 0x80) {
            bytes.push_back(static_cast<char>(c));
        } else {
            // Encode wide char as UTF-8 (basic BMP only; good enough here).
            if (c < 0x800) {
                bytes.push_back(static_cast<char>(0xC0 | (c >> 6)));
                bytes.push_back(static_cast<char>(0x80 | (c & 0x3F)));
            } else {
                bytes.push_back(static_cast<char>(0xE0 | (c >> 12)));
                bytes.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
                bytes.push_back(static_cast<char>(0x80 | (c & 0x3F)));
            }
        }
    }

    if (bytes.empty()) return {};
    int wlen = ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(),
                                     static_cast<int>(bytes.size()),
                                     nullptr, 0);
    if (wlen <= 0) {
        // Fallback to ANSI.
        wlen = ::MultiByteToWideChar(CP_ACP, 0, bytes.data(),
                                     static_cast<int>(bytes.size()),
                                     nullptr, 0);
        if (wlen <= 0) return {};
        std::wstring out(static_cast<size_t>(wlen), L'\0');
        ::MultiByteToWideChar(CP_ACP, 0, bytes.data(),
                              static_cast<int>(bytes.size()),
                              &out[0], wlen);
        return out;
    }
    std::wstring out(static_cast<size_t>(wlen), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(),
                          static_cast<int>(bytes.size()),
                          &out[0], wlen);
    return out;
}

bool StartsWithIgnoreCase(const std::wstring& s, const std::wstring& prefix) {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        wchar_t a = s[i], b = prefix[i];
        if (a >= L'A' && a <= L'Z') a = static_cast<wchar_t>(a - L'A' + L'a');
        if (b >= L'A' && b <= L'Z') b = static_cast<wchar_t>(b - L'A' + L'a');
        if (a != b) return false;
    }
    return true;
}

bool IsAllDigits(const std::wstring& s) {
    if (s.empty()) return false;
    for (wchar_t c : s) {
        if (c < L'0' || c > L'9') return false;
    }
    return true;
}

int ParseInt(const std::wstring& s) {
    int v = 0;
    for (wchar_t c : s) v = v * 10 + (c - L'0');
    return v;
}

// Strip a trailing ":line" or ":line:col" from `path_part`, populating
// `out_line` / `out_col`.  Drive-letter colons (e.g. "D:") must NOT be
// consumed, so we only treat segments composed entirely of digits as
// line/column numbers.
void ExtractTrailingLineCol(std::wstring& path_part,
                             int& out_line, int& out_col) {
    // Try to peel off the last ":<digits>".  Up to two times (col then line).
    for (int round = 0; round < 2; ++round) {
        auto pos = path_part.rfind(L':');
        if (pos == std::wstring::npos) return;
        std::wstring tail = path_part.substr(pos + 1);
        if (!IsAllDigits(tail)) return;

        // Don't eat the drive-letter colon ("D:" at the very start).
        // After peeling we must still leave a non-empty path.
        if (pos == 0) return;

        int v = ParseInt(tail);
        path_part.resize(pos);

        if (round == 0) {
            // First peel: could be either line (only one number) or column
            // (two numbers).  We won't know until the second round, so
            // tentatively treat it as line; if a second peel succeeds the
            // first becomes column.
            out_line = v;
        } else {
            out_col  = out_line;
            out_line = v;
        }
    }
}

} // namespace

VsUrl ParseVsUrl(const std::wstring& raw) {
    VsUrl out;
    if (raw.empty()) return out;

    std::wstring s = raw;

    // Strip surrounding quotes if any.
    if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') {
        s = s.substr(1, s.size() - 2);
    }

    // Strip the scheme.
    if (StartsWithIgnoreCase(s, L"vsjump://")) {
        s = s.substr(9);
    } else if (StartsWithIgnoreCase(s, L"vsjump:")) {
        // Tolerate a missing "//" (some shells / launchers strip it).
        s = s.substr(7);
    } else {
        // Not a vsjump URL — caller might have passed a raw path; we won't accept it.
        return out;
    }

    // Optional "file/" authority (VS Code-style: vsjump://file/D:\...).
    if (StartsWithIgnoreCase(s, L"file/")) {
        s = s.substr(5);
        // Tolerate "file//" or "file///".
        while (!s.empty() && (s.front() == L'/' || s.front() == L'\\')) {
            // Don't strip if what follows looks like a UNC "//server".
            // But here we only strip up to one extra slash to be safe.
            if (s.size() >= 3 && ((s[1] >= L'A' && s[1] <= L'Z') ||
                                  (s[1] >= L'a' && s[1] <= L'z')) &&
                s[2] == L':') {
                s = s.substr(1);
                break;
            }
            break;
        }
    } else if (StartsWithIgnoreCase(s, L"file:")) {
        // "vsjump://file:..." — uncommon but handle it.
        s = s.substr(5);
        while (!s.empty() && (s.front() == L'/' || s.front() == L'\\')) {
            s = s.substr(1);
        }
    }

    // Some shells turn "C:\..." into "/C:/..." after the //.
    if (!s.empty() && s.front() == L'/') {
        // Only strip if the next chars look like a drive letter.
        if (s.size() >= 3 && ((s[1] >= L'A' && s[1] <= L'Z') ||
                              (s[1] >= L'a' && s[1] <= L'z')) &&
            s[2] == L':') {
            s = s.substr(1);
        }
    }

    std::wstring path_part = s;

    // Trim a trailing slash that some launchers append.
    while (!path_part.empty() && (path_part.back() == L'/' ||
                                   path_part.back() == L'\\')) {
        // Don't strip the root "C:\".
        if (path_part.size() <= 3) break;
        path_part.pop_back();
    }

    // Decode percent-escapes BEFORE peeling line/col (so that a `%3A` inside
    // a filename doesn't fool us, and so that real `:` separators are still
    // visible).  Note: percent-decoding will not introduce or remove `:`
    // characters except where the user explicitly encoded them.
    path_part = PercentDecodeUtf8(path_part);

    // Pull a trailing ":line[:col]" off the path, before doing the slash
    // normalization (so `D:` at the start can be distinguished by position).
    int line = 0, col = 0;
    ExtractTrailingLineCol(path_part, line, col);
    out.line   = line;
    out.column = col;

    out.file = path_part;

    // Normalize separators to backslash.
    for (auto& c : out.file) {
        if (c == L'/') c = L'\\';
    }

    out.valid = !out.file.empty();
    return out;
}

} // namespace vsjump
