#include "url_parser.h"

#include <windows.h>

#include <string>
#include <vector>

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

// Convert all '/' to '\\' in-place.
void NormalizeSeparators(std::wstring& s) {
    for (auto& c : s) {
        if (c == L'/') c = L'\\';
    }
}

// Strip a single leading "/<drive-letter>:" slash that some shells inject.
void StripBogusLeadingSlash(std::wstring& s) {
    if (!s.empty() && s.front() == L'/') {
        if (s.size() >= 3 && ((s[1] >= L'A' && s[1] <= L'Z') ||
                              (s[1] >= L'a' && s[1] <= L'z')) &&
            s[2] == L':') {
            s = s.substr(1);
        }
    }
}

// Parse the body of a Direct URL (everything after "vsjump://[file/]").
void ParseDirectBody(std::wstring s, VsUrl& out) {
    StripBogusLeadingSlash(s);

    // Trim a trailing slash that some launchers append.
    while (!s.empty() && (s.back() == L'/' || s.back() == L'\\')) {
        if (s.size() <= 3) break;  // Don't strip the root "C:\".
        s.pop_back();
    }

    s = PercentDecodeUtf8(s);

    int line = 0, col = 0;
    ExtractTrailingLineCol(s, line, col);
    out.line   = line;
    out.column = col;

    out.file = s;
    NormalizeSeparators(out.file);

    if (!out.file.empty()) {
        out.kind = VsUrlKind::Direct;
    }
}

// Split the query string ("k1=v1&k2=v2") into (key, value) pairs.
// Keys are compared case-insensitively by callers; values are returned raw
// (still percent-encoded — callers decide when to decode).
std::vector<std::pair<std::wstring, std::wstring>>
SplitQuery(const std::wstring& q) {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    size_t i = 0;
    while (i < q.size()) {
        size_t amp = q.find(L'&', i);
        std::wstring kv = (amp == std::wstring::npos)
                              ? q.substr(i)
                              : q.substr(i, amp - i);
        if (!kv.empty()) {
            size_t eq = kv.find(L'=');
            if (eq == std::wstring::npos) {
                out.emplace_back(kv, std::wstring());
            } else {
                out.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
            }
        }
        if (amp == std::wstring::npos) break;
        i = amp + 1;
    }
    return out;
}

bool EqIgnoreCaseAscii(const std::wstring& a, const wchar_t* b) {
    size_t n = wcslen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; ++i) {
        wchar_t x = a[i], y = b[i];
        if (x >= L'A' && x <= L'Z') x = static_cast<wchar_t>(x - L'A' + L'a');
        if (y >= L'A' && y <= L'Z') y = static_cast<wchar_t>(y - L'A' + L'a');
        if (x != y) return false;
    }
    return true;
}

// Parse the body of a Match URL (everything after "vsjump://match/").
//
// Expected shape: "?srcfile=<path>[:<line>[:<col>]]&destdir=<dir>[&destdir=...]"
// We are lenient about a missing leading '?'.
void ParseMatchBody(std::wstring s, VsUrl& out) {
    if (!s.empty() && s.front() == L'?') s = s.substr(1);
    if (s.empty()) return;

    auto kvs = SplitQuery(s);

    std::wstring              srcfile_raw;
    std::vector<std::wstring> dirs;
    bool                      have_srcfile = false;

    for (auto& kv : kvs) {
        const auto& key = kv.first;
        auto&       val = kv.second;
        if (EqIgnoreCaseAscii(key, L"srcfile") ||
            EqIgnoreCaseAscii(key, L"src")) {
            srcfile_raw  = val;
            have_srcfile = true;
        } else if (EqIgnoreCaseAscii(key, L"destdir") ||
                   EqIgnoreCaseAscii(key, L"dir") ||
                   EqIgnoreCaseAscii(key, L"root")) {
            std::wstring d = PercentDecodeUtf8(val);
            NormalizeSeparators(d);
            // Strip surrounding quotes if any.
            if (d.size() >= 2 && d.front() == L'"' && d.back() == L'"') {
                d = d.substr(1, d.size() - 2);
            }
            // Trim trailing separators (except drive root).
            while (d.size() > 3 && d.back() == L'\\') d.pop_back();
            if (!d.empty()) dirs.push_back(d);
        }
    }

    if (!have_srcfile || srcfile_raw.empty() || dirs.empty()) return;

    std::wstring path_part = PercentDecodeUtf8(srcfile_raw);

    int line = 0, col = 0;
    ExtractTrailingLineCol(path_part, line, col);
    out.line   = line;
    out.column = col;

    NormalizeSeparators(path_part);
    if (path_part.empty()) return;

    out.file       = path_part;
    out.match_dirs = std::move(dirs);
    out.kind       = VsUrlKind::Match;
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
        // Not a vsjump URL.
        return out;
    }

    // Authority dispatch: "match/..." vs "file/..." vs bare path.
    if (StartsWithIgnoreCase(s, L"match/")) {
        ParseMatchBody(s.substr(6), out);
        return out;
    }
    if (StartsWithIgnoreCase(s, L"match?")) {
        // Tolerate "vsjump://match?srcfile=..." (no trailing slash).
        ParseMatchBody(s.substr(5), out);
        return out;
    }

    if (StartsWithIgnoreCase(s, L"file/")) {
        s = s.substr(5);
        // Tolerate "file//" or "file///" — strip a single extra slash if a
        // drive letter follows.
        if (!s.empty() && (s.front() == L'/' || s.front() == L'\\')) {
            if (s.size() >= 3 && ((s[1] >= L'A' && s[1] <= L'Z') ||
                                  (s[1] >= L'a' && s[1] <= L'z')) &&
                s[2] == L':') {
                s = s.substr(1);
            }
        }
    } else if (StartsWithIgnoreCase(s, L"file:")) {
        s = s.substr(5);
        while (!s.empty() && (s.front() == L'/' || s.front() == L'\\')) {
            s = s.substr(1);
        }
    }

    ParseDirectBody(s, out);
    return out;
}

} // namespace vsjump
