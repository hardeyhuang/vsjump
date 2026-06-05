#pragma once

#include <string>
#include <vector>

#include <windows.h>
#include <oaidl.h>

namespace vsjump {

// A running Visual Studio instance.
struct VsInstance {
    IDispatch*   dte = nullptr;          // EnvDTE.DTE  (AddRef'd, must be Release()d)
    std::wstring solution_path;          // Full path to the .sln (may be empty if no solution).
    std::wstring solution_dir;           // Directory of the sln (or empty).
    std::wstring rot_display_name;       // e.g. "!VisualStudio.DTE.17.0:12345"
    HWND         main_hwnd = nullptr;    // VS main window (may be NULL).
};

// Enumerates running Visual Studio instances via the COM Running Object Table.
// Caller owns the IDispatch pointers and must Release() them (or call FreeInstances).
std::vector<VsInstance> EnumerateVsInstances();

void FreeInstances(std::vector<VsInstance>& instances);

// Opens `file` (must be an absolute path) inside the given DTE instance,
// then jumps to `line` (1-based; 0 to skip). Returns true on success.
bool OpenFileInVs(IDispatch* dte, const std::wstring& file, int line, int column,
                  std::wstring* err = nullptr);

// Tries to bring the VS main window to the foreground.
void ActivateVsMainWindow(IDispatch* dte);

// Returns true if `hwnd` (or any window in its top-level chain) is currently
// the foreground window's process / window. Accepts NULL → false.
bool IsWindowForeground(HWND hwnd);

// Walks `instances` and returns the index of the one whose main window is
// currently in the foreground, or -1 if none is.
int FindForegroundInstance(const std::vector<VsInstance>& instances);

} // namespace vsjump
