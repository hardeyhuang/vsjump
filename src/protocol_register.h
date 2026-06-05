#pragma once

#include <string>

namespace vsjump {

// Registers the `vsjump://` URL scheme in HKEY_CURRENT_USER so it points to
// the given executable. Returns true on success.
//
// Keys written:
//   HKCU\Software\Classes\vsjump\(default)            = "URL:VS Jump Protocol"
//   HKCU\Software\Classes\vsjump\URL Protocol         = ""
//   HKCU\Software\Classes\vsjump\shell\open\command\(default) = "\"<exe>\" \"%1\""
bool RegisterProtocol(const std::wstring& exe_path, std::wstring* err = nullptr);

// Removes the `vsjump://` registration installed by RegisterProtocol.
bool UnregisterProtocol(std::wstring* err = nullptr);

} // namespace vsjump
