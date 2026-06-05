#include "picker_dialog.h"

#include <windows.h>
#include <commctrl.h>

#include <vector>

namespace vsjump {

int PickFromList(const std::wstring& title,
                 const std::wstring& main_instruction,
                 const std::wstring& content,
                 const std::vector<std::wstring>& options,
                 int default_index) {
    if (options.empty()) return -1;

    // TASKDIALOG_BUTTON refers to its text via a pointer; we have to keep the
    // backing strings alive for the duration of TaskDialogIndirect.
    std::vector<TASKDIALOG_BUTTON> buttons;
    buttons.reserve(options.size());

    // Radio button IDs start at 1000 so they don't collide with common-button
    // IDs (IDOK, IDCANCEL, ...).
    constexpr int kFirstId = 1000;

    for (size_t i = 0; i < options.size(); ++i) {
        TASKDIALOG_BUTTON b{};
        b.nButtonID = kFirstId + static_cast<int>(i);
        b.pszButtonText = options[i].c_str();
        buttons.push_back(b);
    }

    if (default_index < 0 ||
        default_index >= static_cast<int>(options.size())) {
        default_index = 0;
    }

    TASKDIALOGCONFIG cfg{};
    cfg.cbSize             = sizeof(cfg);
    cfg.hwndParent         = nullptr;
    cfg.hInstance          = ::GetModuleHandleW(nullptr);
    cfg.dwFlags            = TDF_ALLOW_DIALOG_CANCELLATION |
                             TDF_POSITION_RELATIVE_TO_WINDOW;
    cfg.dwCommonButtons    = TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON;
    cfg.pszWindowTitle     = title.c_str();
    cfg.pszMainInstruction = main_instruction.c_str();
    cfg.pszContent         = content.empty() ? nullptr : content.c_str();
    cfg.cRadioButtons      = static_cast<UINT>(buttons.size());
    cfg.pRadioButtons      = buttons.data();
    cfg.nDefaultRadioButton = kFirstId + default_index;

    // Built-in TaskDialog "info" icon (a TD_*_ICON pseudo-handle).
    cfg.pszMainIcon        = TD_INFORMATION_ICON;

    int btn = 0;
    int radio = 0;
    HRESULT hr = ::TaskDialogIndirect(&cfg, &btn, &radio, nullptr);
    if (FAILED(hr)) return -1;

    // If the user pressed Cancel, IDCANCEL is returned in `btn`. Treat any
    // non-OK button as cancellation.
    if (btn != IDOK) return -1;

    int idx = radio - kFirstId;
    if (idx < 0 || idx >= static_cast<int>(options.size())) return -1;
    return idx;
}

} // namespace vsjump
