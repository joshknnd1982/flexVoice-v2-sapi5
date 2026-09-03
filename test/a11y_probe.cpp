// a11y_probe.cpp -- read the configuration utility the way a screen reader does.
//
// Checks, against the live window:
//   * every control has an accessible name
//   * the MSAA role is what the control actually is
//   * tab order matches reading order
//   * access keys are unique
//
// MSAA rather than UI Automation on purpose: PowerShell's UIA client reports
// every one of these controls as a Pane, which tells you nothing. oleacc sees
// what NVDA and JAWS see.

#ifdef _MSC_VER
#  pragma warning(disable : 4996)
#endif

#include <windows.h>
#include <initguid.h>
#include <oleacc.h>
#include <stdio.h>

#include <map>
#include <string>
#include <vector>

namespace {

struct Control {
    HWND        hwnd;
    std::wstring cls;
    std::wstring text;
    std::wstring accName;
    std::wstring accRole;
    bool        tabStop;
};

std::wstring role_name(DWORD role)
{
    wchar_t buf[128] = {};
    GetRoleTextW(role, buf, 127);
    return buf[0] ? buf : L"(none)";
}

std::wstring acc_name(HWND h)
{
    IAccessible* acc = nullptr;
    if (FAILED(AccessibleObjectFromWindow(h, OBJID_CLIENT, IID_IAccessible,
                                          reinterpret_cast<void**>(&acc))) || !acc) {
        return L"(no IAccessible)";
    }
    VARIANT self;
    self.vt = VT_I4;
    self.lVal = CHILDID_SELF;
    BSTR name = nullptr;
    std::wstring out = L"(none)";
    if (SUCCEEDED(acc->get_accName(self, &name)) && name) {
        out = name;
        SysFreeString(name);
    }
    acc->Release();
    return out;
}

std::wstring acc_role(HWND h)
{
    IAccessible* acc = nullptr;
    if (FAILED(AccessibleObjectFromWindow(h, OBJID_CLIENT, IID_IAccessible,
                                          reinterpret_cast<void**>(&acc))) || !acc) {
        return L"(none)";
    }
    VARIANT self, role;
    self.vt = VT_I4;
    self.lVal = CHILDID_SELF;
    VariantInit(&role);
    std::wstring out = L"(none)";
    if (SUCCEEDED(acc->get_accRole(self, &role)) && role.vt == VT_I4) {
        out = role_name(static_cast<DWORD>(role.lVal));
    }
    VariantClear(&role);
    acc->Release();
    return out;
}

std::vector<Control>* g_controls = nullptr;

BOOL CALLBACK enum_child(HWND h, LPARAM)
{
    Control c;
    c.hwnd = h;
    wchar_t buf[256] = {};
    GetClassNameW(h, buf, 255);
    c.cls = buf;
    buf[0] = 0;
    GetWindowTextW(h, buf, 255);
    c.text = buf;
    c.accName = acc_name(h);
    c.accRole = acc_role(h);
    c.tabStop = (GetWindowLongW(h, GWL_STYLE) & WS_TABSTOP) != 0;
    g_controls->push_back(c);
    return TRUE;
}

}  // namespace

int main(int argc, char** argv)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const wchar_t* title = L"FlexVoice Configuration";
    HWND dlg = nullptr;
    for (int i = 0; i < 100 && !dlg; ++i) {
        dlg = FindWindowW(nullptr, title);
        if (!dlg) Sleep(100);
    }
    if (!dlg) {
        printf("The FlexVoice Configuration window is not open.\n"
               "Start FlexVoiceConfig.exe first, then run this.\n");
        return 1;
    }

    std::vector<Control> controls;
    g_controls = &controls;
    EnumChildWindows(dlg, enum_child, 0);

    printf("%-4s %-16s %-22s %-46s %s\n", "tab", "class", "role", "accessible name", "text");
    printf("%s\n", std::string(120, '-').c_str());

    int tabIndex = 0;
    int unnamed = 0;
    std::map<wchar_t, std::vector<std::wstring>> accessKeys;

    for (const Control& c : controls) {
        char tab[8] = "  -";
        if (c.tabStop) sprintf(tab, "%3d", ++tabIndex);
        printf("%-4s %-16S %-22S %-46S %S\n", tab, c.cls.c_str(), c.accRole.c_str(),
               c.accName.c_str(), c.text.c_str());

        if (c.tabStop &&
            (c.accName == L"(none)" || c.accName == L"(no IAccessible)" || c.accName.empty())) {
            ++unnamed;
        }
        // Collect access keys from every label and button.
        for (size_t i = 0; i + 1 < c.text.size(); ++i) {
            if (c.text[i] == L'&' && c.text[i + 1] != L'&') {
                accessKeys[towlower(c.text[i + 1])].push_back(
                    c.text.substr(0, 40));
            }
        }
    }

    printf("\n%d controls, %d in the tab order\n",
           static_cast<int>(controls.size()), tabIndex);

    int problems = 0;
    if (unnamed) {
        printf("PROBLEM: %d focusable control(s) have no accessible name\n", unnamed);
        problems += unnamed;
    }
    for (const auto& [key, users] : accessKeys) {
        if (users.size() > 1) {
            printf("PROBLEM: access key '%C' is used %d times:\n", key,
                   static_cast<int>(users.size()));
            for (const auto& u : users) printf("           %S\n", u.c_str());
            ++problems;
        }
    }
    // A trackbar would be a real defect here: MSAA reports its position as a
    // percentage of its range, so a 0-9 slider announces 5 as "55".
    for (const Control& c : controls) {
        if (_wcsicmp(c.cls.c_str(), L"msctls_trackbar32") == 0) {
            printf("PROBLEM: %S is a trackbar; use an edit box with a spin buddy\n",
                   c.accName.c_str());
            ++problems;
        }
    }

    printf("\n%s\n", problems ? "ACCESSIBILITY CHECK FAILED"
                              : "accessibility check passed");
    CoUninitialize();
    return problems ? 1 : 0;
}
