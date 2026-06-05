#include "vs_locator.h"

#include <windows.h>
#include <objbase.h>
#include <oleauto.h>

#include <string>

namespace vsjump {

namespace {

// ---------- small COM helpers ----------

struct VariantGuard {
    VARIANT v{};
    VariantGuard() { ::VariantInit(&v); }
    ~VariantGuard() { ::VariantClear(&v); }
    VariantGuard(const VariantGuard&) = delete;
    VariantGuard& operator=(const VariantGuard&) = delete;
};

HRESULT GetDispId(IDispatch* disp, const wchar_t* name, DISPID* out) {
    LPOLESTR n = const_cast<LPOLESTR>(name);
    return disp->GetIDsOfNames(IID_NULL, &n, 1, LOCALE_USER_DEFAULT, out);
}

// Invoke a property-get with N args; the result is stored in `result`.
HRESULT InvokeGet(IDispatch* disp, const wchar_t* name, VARIANT* result,
                  VARIANT* args = nullptr, UINT nargs = 0) {
    DISPID id;
    HRESULT hr = GetDispId(disp, name, &id);
    if (FAILED(hr)) return hr;

    DISPPARAMS dp{};
    dp.cArgs = nargs;
    dp.rgvarg = args;
    return disp->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT,
                        DISPATCH_PROPERTYGET, &dp, result, nullptr, nullptr);
}

// Invoke a method with N args. Caller passes args in REVERSE order
// (DISPPARAMS convention).
HRESULT InvokeMethod(IDispatch* disp, const wchar_t* name, VARIANT* result,
                     VARIANT* rev_args = nullptr, UINT nargs = 0) {
    DISPID id;
    HRESULT hr = GetDispId(disp, name, &id);
    if (FAILED(hr)) return hr;

    DISPPARAMS dp{};
    dp.cArgs = nargs;
    dp.rgvarg = rev_args;
    return disp->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT,
                        DISPATCH_METHOD, &dp, result, nullptr, nullptr);
}

// Get a property that returns IDispatch*. Caller must Release.
IDispatch* GetDispProp(IDispatch* parent, const wchar_t* name) {
    if (!parent) return nullptr;
    VariantGuard r;
    HRESULT hr = InvokeGet(parent, name, &r.v);
    if (FAILED(hr)) return nullptr;
    if (r.v.vt != VT_DISPATCH || !r.v.pdispVal) return nullptr;
    IDispatch* d = r.v.pdispVal;
    d->AddRef();
    return d;
}

// Get a BSTR property as wstring.
std::wstring GetStrProp(IDispatch* parent, const wchar_t* name) {
    if (!parent) return {};
    VariantGuard r;
    HRESULT hr = InvokeGet(parent, name, &r.v);
    if (FAILED(hr)) return {};
    if (r.v.vt == VT_BSTR && r.v.bstrVal) {
        return std::wstring(r.v.bstrVal, ::SysStringLen(r.v.bstrVal));
    }
    return {};
}

// Get a long property.
LONG GetLongProp(IDispatch* parent, const wchar_t* name, LONG fallback = 0) {
    if (!parent) return fallback;
    VariantGuard r;
    HRESULT hr = InvokeGet(parent, name, &r.v);
    if (FAILED(hr)) return fallback;
    VariantGuard conv;
    if (FAILED(::VariantChangeType(&conv.v, &r.v, 0, VT_I4))) return fallback;
    return conv.v.lVal;
}

bool StartsWith(const std::wstring& s, const wchar_t* prefix) {
    size_t n = wcslen(prefix);
    if (s.size() < n) return false;
    return s.compare(0, n, prefix) == 0;
}

std::wstring DirectoryOf(const std::wstring& file) {
    if (file.empty()) return {};
    size_t pos = file.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return {};
    return file.substr(0, pos);
}

} // namespace

// ---------- enumeration ----------

std::vector<VsInstance> EnumerateVsInstances() {
    std::vector<VsInstance> out;

    IRunningObjectTable* rot = nullptr;
    if (FAILED(::GetRunningObjectTable(0, &rot)) || !rot) return out;

    IEnumMoniker* en = nullptr;
    if (FAILED(rot->EnumRunning(&en)) || !en) {
        rot->Release();
        return out;
    }

    IBindCtx* bind = nullptr;
    if (FAILED(::CreateBindCtx(0, &bind)) || !bind) {
        en->Release();
        rot->Release();
        return out;
    }

    IMoniker* mk = nullptr;
    while (en->Next(1, &mk, nullptr) == S_OK) {
        LPOLESTR name = nullptr;
        if (SUCCEEDED(mk->GetDisplayName(bind, nullptr, &name)) && name) {
            std::wstring display(name);
            ::CoTaskMemFree(name);

            // VS DTE monikers look like "!VisualStudio.DTE.17.0:12345".
            if (StartsWith(display, L"!VisualStudio.DTE.")) {
                IUnknown* unk = nullptr;
                if (SUCCEEDED(rot->GetObject(mk, &unk)) && unk) {
                    IDispatch* dte = nullptr;
                    if (SUCCEEDED(unk->QueryInterface(IID_IDispatch,
                                                     reinterpret_cast<void**>(&dte))) &&
                        dte) {
                        VsInstance inst;
                        inst.dte = dte;  // owned
                        inst.rot_display_name = display;

                        if (IDispatch* sln = GetDispProp(dte, L"Solution")) {
                            inst.solution_path = GetStrProp(sln, L"FullName");
                            sln->Release();
                        }
                        if (!inst.solution_path.empty()) {
                            inst.solution_dir = DirectoryOf(inst.solution_path);
                        }
                        if (IDispatch* main = GetDispProp(dte, L"MainWindow")) {
                            LONG hwnd_l = GetLongProp(main, L"HWnd", 0);
                            if (hwnd_l != 0) {
                                inst.main_hwnd = reinterpret_cast<HWND>(
                                    static_cast<LONG_PTR>(hwnd_l));
                            }
                            main->Release();
                        }
                        out.push_back(inst);
                    }
                    unk->Release();
                }
            }
        }
        mk->Release();
        mk = nullptr;
    }

    bind->Release();
    en->Release();
    rot->Release();
    return out;
}

void FreeInstances(std::vector<VsInstance>& instances) {
    for (auto& i : instances) {
        if (i.dte) {
            i.dte->Release();
            i.dte = nullptr;
        }
    }
    instances.clear();
}

// ---------- open & jump ----------

bool OpenFileInVs(IDispatch* dte, const std::wstring& file, int line, int column,
                  std::wstring* err) {
    if (!dte) {
        if (err) *err = L"DTE is null";
        return false;
    }

    // ItemOperations.OpenFile(File, ViewKind)
    IDispatch* item_ops = GetDispProp(dte, L"ItemOperations");
    if (!item_ops) {
        if (err) *err = L"DTE.ItemOperations not available";
        return false;
    }

    // ViewKind = "{7651A701-06E5-11D1-8EBD-00A0C90F26EA}" == vsViewKindPrimary == "{...}"
    // Actually the well-known constant is the GUID string vsViewKindPrimary:
    //   "{00000000-0000-0000-0000-000000000000}" -> primary
    // We pass the primary view (textual) GUID explicitly.
    BSTR file_bstr = ::SysAllocStringLen(file.c_str(), static_cast<UINT>(file.size()));
    BSTR view_bstr = ::SysAllocString(L"{7651A701-06E5-11D1-8EBD-00A0C90F26EA}");

    // DISPPARAMS rgvarg is in REVERSE order: [ViewKind, File]
    VARIANT args[2];
    ::VariantInit(&args[0]); args[0].vt = VT_BSTR; args[0].bstrVal = view_bstr; // ViewKind
    ::VariantInit(&args[1]); args[1].vt = VT_BSTR; args[1].bstrVal = file_bstr; // File

    VariantGuard window_var;
    HRESULT hr = InvokeMethod(item_ops, L"OpenFile", &window_var.v, args, 2);

    ::SysFreeString(file_bstr);
    ::SysFreeString(view_bstr);
    item_ops->Release();

    if (FAILED(hr)) {
        if (err) {
            wchar_t buf[64];
            swprintf_s(buf, L"OpenFile failed (HRESULT=0x%08X)", static_cast<unsigned>(hr));
            *err = buf;
        }
        return false;
    }

    if (line <= 0) return true;

    // Prefer the Window returned by OpenFile -> Document -> Selection.
    // This avoids races/ambiguity with DTE.ActiveDocument when the IDE is
    // still busy bringing the new editor window to the front.
    IDispatch* window = nullptr;
    if (window_var.v.vt == VT_DISPATCH && window_var.v.pdispVal) {
        window = window_var.v.pdispVal;
        window->AddRef();
    }

    IDispatch* selection = nullptr;
    IDispatch* document  = nullptr;

    if (window) {
        // Make sure the editor window is active so GotoLine targets it.
        VariantGuard r_act;
        InvokeMethod(window, L"Activate", &r_act.v);

        document = GetDispProp(window, L"Document");
        if (document) {
            selection = GetDispProp(document, L"Selection");
        }
    }

    // Fallback: try DTE.ActiveDocument if we could not get Selection from the
    // returned Window (e.g. OpenFile returned VT_EMPTY for some VS versions).
    IDispatch* active_doc = nullptr;
    if (!selection) {
        active_doc = GetDispProp(dte, L"ActiveDocument");
        if (active_doc) {
            selection = GetDispProp(active_doc, L"Selection");
        }
    }

    if (selection) {
        // GotoLine(Line As Long, [Select As Boolean = False])
        VARIANT a[2];
        ::VariantInit(&a[0]); a[0].vt = VT_BOOL; a[0].boolVal = VARIANT_FALSE; // Select
        ::VariantInit(&a[1]); a[1].vt = VT_I4;   a[1].lVal    = line;          // Line
        VariantGuard r;
        HRESULT hr_goto = InvokeMethod(selection, L"GotoLine", &r.v, a, 2);

        // Optionally move to column on the same line.
        if (SUCCEEDED(hr_goto) && column > 0) {
            VARIANT b[2];
            ::VariantInit(&b[0]); b[0].vt = VT_BOOL; b[0].boolVal = VARIANT_FALSE; // Extend
            ::VariantInit(&b[1]); b[1].vt = VT_I4;   b[1].lVal    = column;        // Column
            VariantGuard r2;
            InvokeMethod(selection, L"MoveToDisplayColumn", &r2.v, b, 2);
        }

        selection->Release();
    }

    if (document)   document->Release();
    if (active_doc) active_doc->Release();
    if (window)     window->Release();
    return true;
}

void ActivateVsMainWindow(IDispatch* dte) {
    if (!dte) return;
    IDispatch* main = GetDispProp(dte, L"MainWindow");
    if (!main) return;

    // MainWindow.Activate()
    VariantGuard r;
    InvokeMethod(main, L"Activate", &r.v);

    // Try to also bring HWnd to the foreground.
    LONG hwnd_l = GetLongProp(main, L"HWnd", 0);
    if (hwnd_l != 0) {
        HWND hwnd = reinterpret_cast<HWND>(static_cast<LONG_PTR>(hwnd_l));
        if (::IsIconic(hwnd)) {
            ::ShowWindow(hwnd, SW_RESTORE);
        }
        ::SetForegroundWindow(hwnd);
    }
    main->Release();
}

bool IsWindowForeground(HWND hwnd) {
    if (!hwnd) return false;
    HWND fg = ::GetForegroundWindow();
    if (!fg) return false;

    // Direct match.
    if (fg == hwnd) return true;

    // VS may have child popups (e.g., a modal dialog) in the foreground; treat
    // any window owned by VS's process AND belonging to the same top-level
    // window as "VS in foreground".
    HWND fg_root = ::GetAncestor(fg, GA_ROOT);
    if (fg_root == hwnd) return true;

    DWORD pid_target = 0;
    DWORD pid_fg = 0;
    ::GetWindowThreadProcessId(hwnd, &pid_target);
    ::GetWindowThreadProcessId(fg, &pid_fg);
    return pid_target != 0 && pid_target == pid_fg;
}

int FindForegroundInstance(const std::vector<VsInstance>& instances) {
    for (size_t i = 0; i < instances.size(); ++i) {
        if (IsWindowForeground(instances[i].main_hwnd)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace vsjump
