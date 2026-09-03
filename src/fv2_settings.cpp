#include "fv2_settings.h"

#include <shlobj.h>
#include <stdio.h>

#include <vector>

namespace fv2 {

namespace {

std::wstring widen(const std::string& s)
{
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::string narrow(const std::wstring& w)
{
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                      nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

Settings::Settings()
{
    // Every parameter starts where the base voice's own .tav puts it, expressed
    // as a percentage of that parameter's range, so opening the utility for the
    // first time shows the voice as Mindmaker tuned it rather than a row of
    // sliders at 50.
    for (int i = 0; i < P_COUNT; ++i) {
        percent[i] = (int)(valueToPercent((ParamId)i, kParams[i].fallback) + 0.5);
    }
}

Settings SettingsStore::defaults() { return Settings(); }

std::wstring SettingsStore::app_dir()
{
    wchar_t appdata[MAX_PATH] = {0};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) {
        return L".";
    }
    std::wstring dir = std::wstring(appdata) + L"\\FlexVoice2SAPI";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring SettingsStore::path()
{
    return app_dir() + L"\\settings.ini";
}

bool SettingsStore::file_time(FILETIME& ft)
{
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (!GetFileAttributesExW(path().c_str(), GetFileExInfoStandard, &fad)) return false;
    ft = fad.ftLastWriteTime;
    return true;
}

Settings SettingsStore::load()
{
    Settings s;
    const std::wstring file = path();

    wchar_t buf[512];

    GetPrivateProfileStringW(L"voice", L"baseVoice", L"Julie", buf, 512, file.c_str());
    s.baseVoice = narrow(buf);
    if (s.baseVoice.empty()) s.baseVoice = "Julie";

    for (int i = 0; i < P_COUNT; ++i) {
        const std::wstring key = widen(kParams[i].tavKey);
        const int def = s.percent[i];
        s.percent[i] = clampi(
            GetPrivateProfileIntW(L"speech", key.c_str(), def, file.c_str()), 0, 100);
    }

    s.sampleRate = GetPrivateProfileIntW(L"options", L"sampleRate", 16000, file.c_str());
    switch (s.sampleRate) {
    case 8000: case 11025: case 16000: case 22050: case 32000: case 44100: break;
    default: s.sampleRate = 16000; break;
    }

    s.debugLogging = GetPrivateProfileIntW(L"logging", L"debug", 1, file.c_str()) != 0;
    s.verboseLogging = GetPrivateProfileIntW(L"logging", L"verbose", 0, file.c_str()) != 0;
    return s;
}

bool SettingsStore::save(const Settings& s)
{
    const std::wstring file = path();
    bool ok = true;
    wchar_t num[32];

    ok = WritePrivateProfileStringW(L"voice", L"baseVoice",
                                    widen(s.baseVoice).c_str(), file.c_str()) && ok;

    for (int i = 0; i < P_COUNT; ++i) {
        swprintf_s(num, L"%d", clampi(s.percent[i], 0, 100));
        ok = WritePrivateProfileStringW(L"speech", widen(kParams[i].tavKey).c_str(),
                                        num, file.c_str()) && ok;
    }

    swprintf_s(num, L"%d", s.sampleRate);
    ok = WritePrivateProfileStringW(L"options", L"sampleRate", num, file.c_str()) && ok;

    ok = WritePrivateProfileStringW(L"logging", L"debug",
                                    s.debugLogging ? L"1" : L"0", file.c_str()) && ok;
    ok = WritePrivateProfileStringW(L"logging", L"verbose",
                                    s.verboseLogging ? L"1" : L"0", file.c_str()) && ok;

    // WritePrivateProfileString caches; without this a reader in another
    // process can see the old file for some time, which for us means the next
    // utterance still uses the old voice.
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, file.c_str());
    return ok;
}

const Settings& SettingsStore::current()
{
    static Settings cached = load();
    static FILETIME stamp = {};
    static bool have = file_time(stamp);
    static CRITICAL_SECTION cs;
    static bool init = (InitializeCriticalSection(&cs), true);
    (void)init;

    FILETIME now = {};
    const bool exists = file_time(now);
    if (exists != have || (exists && CompareFileTime(&now, &stamp) != 0)) {
        EnterCriticalSection(&cs);
        cached = load();
        stamp = now;
        have = exists;
        LeaveCriticalSection(&cs);
    }
    return cached;
}

}  // namespace fv2
