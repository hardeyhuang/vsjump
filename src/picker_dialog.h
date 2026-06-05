#pragma once

#include <string>
#include <vector>

namespace vsjump {

// Shows a TaskDialog with one radio button per option. Returns the selected
// index, or -1 if the user cancelled / closed the dialog.
//
// `options` must be non-empty. `default_index` is the initially-selected
// radio button (clamped to a valid range).
int PickFromList(const std::wstring& title,
                 const std::wstring& main_instruction,
                 const std::wstring& content,
                 const std::vector<std::wstring>& options,
                 int default_index = 0);

} // namespace vsjump
