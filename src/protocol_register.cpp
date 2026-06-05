#include "protocol_register.h"

#include <windows.h>

namespace vsjump {

namespace {

bool SetRegSz(HKEY root, const wchar_t* sub_key, const wchar_t* value_name,
              const std::wstring& value, std::wstring* err) {
    HKEY h = nullptr;
    LONG rc = ::RegCreateKeyExW(root, sub_key, 0, nullptr, 0,
                                KEY_WRITE, nullptr, &h, nullptr);
    if (rc != ERROR_SUCCESS) {
        if (err) *err = L"RegCreateKeyEx failed for: " + std::wstring(sub_key);
        return false;
    }
    rc = ::RegSetValueExW(
        h, value_name, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    ::RegCloseKey(h);
    if (rc != ERROR_SUCCESS) {
        if (err) *err = L"RegSetValueEx failed for: " + std::wstring(sub_key);
        return false;
    }
    return true;
}

bool DeleteTreeQuiet(HKEY root, const wchar_t* sub_key) {
    LONG rc = ::RegDeleteTreeW(root, sub_key);
    return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
}

} // namespace

bool RegisterProtocol(const std::wstring& exe_path, std::wstring* err) {
    if (exe_path.empty()) {
        if (err) *err = L"Empty exe path";
        return false;
    }

    const wchar_t* kRoot = L"Software\\Classes\\vsjump";
    const wchar_t* kIcon = L"Software\\Classes\\vsjump\\DefaultIcon";
    const wchar_t* kCmd  = L"Software\\Classes\\vsjump\\shell\\open\\command";

    if (!SetRegSz(HKEY_CURRENT_USER, kRoot, L"",            L"URL:VS Jump Protocol", err)) return false;
    if (!SetRegSz(HKEY_CURRENT_USER, kRoot, L"URL Protocol", L"",                    err)) return false;

    SetRegSz(HKEY_CURRENT_USER, kIcon, L"", L"\"" + exe_path + L"\",0", nullptr);

    std::wstring cmd = L"\"" + exe_path + L"\" \"%1\"";
    if (!SetRegSz(HKEY_CURRENT_USER, kCmd, L"", cmd, err)) return false;

    return true;
}

bool UnregisterProtocol(std::wstring* err) {
    bool ok_new = DeleteTreeQuiet(HKEY_CURRENT_USER, L"Software\\Classes\\vsjump");
    /* legacy */  DeleteTreeQuiet(HKEY_CURRENT_USER, L"Software\\Classes\\vs");
    if (!ok_new) {
        if (err) *err = L"Failed to remove HKCU\\Software\\Classes\\vsjump";
        return false;
    }
    return true;
}

} // namespace vsjump
