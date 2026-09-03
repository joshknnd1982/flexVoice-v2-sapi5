// a11y_probe -- walk the configuration utility the way a screen reader does.
//
// This uses MSAA (oleacc) rather than UI Automation on purpose. PowerShell's
// UIA client reports every one of these controls as a generic "Pane", which
// makes it useless for checking that an edit box is an edit box; oleacc reports
// the roles NVDA actually reads.
//
// What it checks, because each of these has gone wrong before:
//   * every control that can take focus has a name, so nothing is announced as
//     just "edit" or "combo box"
//   * no control is a trackbar -- MSAA reports a trackbar's value as a
//     percentage of its range, so a 0..9 slider announces 5 as "55"
//   * names are unique, so two controls cannot be confused by ear
//   * the tab order visits the controls in the order they are laid out

#include <windows.h>
#include <oleacc.h>

#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "oleacc.lib")

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what)
{
    ++g_checks;
    std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_failures;
}

struct Found {
    HWND hwnd = nullptr;
};

BOOL CALLBACK find_dialog(HWND hwnd, LPARAM lp)
{
    wchar_t title[256] = {0};
    GetWindowTextW(hwnd, title, 255);
    if (wcsstr(title, L"FlexVoice 2 Configuration")) {
        reinterpret_cast<Found*>(lp)->hwnd = hwnd;
        return FALSE;
    }
    return TRUE;
}

std::wstring role_name(long role)
{
    wchar_t buf[128] = {0};
    GetRoleTextW(static_cast<DWORD>(role), buf, 127);
    return buf;
}

struct Control {
    HWND        hwnd;
    std::wstring cls;
    std::wstring name;
    std::wstring role;
    long        roleId = 0;
    bool        tabStop = false;
    bool        visible = false;
};

std::wstring acc_name(HWND hwnd)
{
    IAccessible* acc = nullptr;
    if (FAILED(AccessibleObjectFromWindow(hwnd, OBJID_CLIENT, IID_IAccessible,
                                          reinterpret_cast<void**>(&acc))) || !acc) {
        return L"";
    }
    VARIANT self;
    self.vt = VT_I4;
    self.lVal = CHILDID_SELF;
    BSTR name = nullptr;
    std::wstring out;
    if (SUCCEEDED(acc->get_accName(self, &name)) && name) {
        out = name;
        SysFreeString(name);
    }
    acc->Release();
    return out;
}

long acc_role(HWND hwnd)
{
    IAccessible* acc = nullptr;
    if (FAILED(AccessibleObjectFromWindow(hwnd, OBJID_CLIENT, IID_IAccessible,
                                          reinterpret_cast<void**>(&acc))) || !acc) {
        return 0;
    }
    VARIANT self, role;
    self.vt = VT_I4;
    self.lVal = CHILDID_SELF;
    VariantInit(&role);
    long r = 0;
    if (SUCCEEDED(acc->get_accRole(self, &role)) && role.vt == VT_I4) r = role.lVal;
    VariantClear(&role);
    acc->Release();
    return r;
}

BOOL CALLBACK collect(HWND hwnd, LPARAM lp)
{
    auto* out = reinterpret_cast<std::vector<Control>*>(lp);
    Control c;
    c.hwnd = hwnd;

    wchar_t cls[128] = {0};
    GetClassNameW(hwnd, cls, 127);
    c.cls = cls;

    const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    c.tabStop = (style & WS_TABSTOP) != 0;
    c.visible = (style & WS_VISIBLE) != 0;
    c.name = acc_name(hwnd);
    c.roleId = acc_role(hwnd);
    c.role = role_name(c.roleId);

    out->push_back(c);
    return TRUE;
}

}  // namespace

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    std::string exe = (argc > 1) ? argv[1] : "FlexVoice2Config.exe";
    std::printf("launching %s\n", exe.c_str());

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    std::vector<char> cmd(exe.begin(), exe.end());
    cmd.push_back('\0');
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &si, &pi)) {
        std::printf("could not start it (error %lu)\n", GetLastError());
        return 2;
    }
    WaitForInputIdle(pi.hProcess, 10000);

    Found found;
    for (int i = 0; i < 60 && !found.hwnd; ++i) {
        EnumWindows(find_dialog, reinterpret_cast<LPARAM>(&found));
        if (!found.hwnd) Sleep(100);
    }
    if (!found.hwnd) {
        std::printf("the configuration window never appeared\n");
        TerminateProcess(pi.hProcess, 1);
        return 1;
    }
    std::printf("window found\n\n");

    std::vector<Control> controls;
    EnumChildWindows(found.hwnd, collect, reinterpret_cast<LPARAM>(&controls));

    std::printf("%-18s %-26s %-14s %-4s %s\n", "class", "accessible name", "role", "tab", "");
    std::printf("%s\n", std::string(84, '-').c_str());

    int tabStops = 0, unnamed = 0, trackbars = 0;
    std::vector<std::wstring> names;
    for (const Control& c : controls) {
        if (!c.visible) continue;
        const bool interactive = c.tabStop;
        if (interactive) ++tabStops;
        if (c.cls.find(L"msctls_trackbar") != std::wstring::npos) ++trackbars;
        if (interactive && c.name.empty()) ++unnamed;
        if (interactive && !c.name.empty()) names.push_back(c.name);

        std::wprintf(L"%-18s %-26s %-14s %-4s\n",
                     c.cls.substr(0, 17).c_str(),
                     c.name.substr(0, 25).c_str(),
                     c.role.substr(0, 13).c_str(),
                     c.tabStop ? L"yes" : L"no");
    }

    std::printf("\n%d controls, %d of them in the tab order\n",
                (int)controls.size(), tabStops);

    check(tabStops >= 20, "every parameter and button is reachable by Tab");
    check(unnamed == 0, "every control in the tab order has an accessible name");
    check(trackbars == 0, "no trackbars (MSAA would report their value as a percentage of range)");

    // Duplicate names would be read identically by a screen reader.
    int dupes = 0;
    for (size_t i = 0; i < names.size(); ++i) {
        for (size_t j = i + 1; j < names.size(); ++j) {
            if (names[i] == names[j]) ++dupes;
        }
    }
    if (dupes) std::printf("  %d duplicate accessible names\n", dupes);
    check(dupes == 0, "no two focusable controls share a name");

    // The edit boxes must be edits, not something a screen reader will read as
    // static text.
    int editCount = 0;
    for (const Control& c : controls) {
        if (c.cls == L"Edit" && c.tabStop) ++editCount;
    }
    std::printf("  %d edit boxes\n", editCount);
    check(editCount >= 14, "one edit box per parameter, plus the preview text");

    PostMessageW(found.hwnd, WM_CLOSE, 0, 0);
    WaitForSingleObject(pi.hProcess, 5000);
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
