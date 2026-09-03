#include "settings.h"
#include "flexvoice_log.h"

#include <shlobj.h>
#include <stdio.h>

namespace FlexVoice {

namespace {

std::wstring app_data_dir()
{
    wchar_t buf[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, buf))) {
        GetTempPathW(MAX_PATH, buf);
    }
    std::wstring dir(buf);
    if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
    dir += L"FlexVoiceSAPI";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

int read_int(const wchar_t* section, const wchar_t* key, int def, const std::wstring& file)
{
    return static_cast<int>(GetPrivateProfileIntW(section, key, def, file.c_str()));
}

void write_int(const wchar_t* section, const wchar_t* key, int value, const std::wstring& file)
{
    wchar_t buf[32];
    swprintf_s(buf, L"%d", value);
    WritePrivateProfileStringW(section, key, buf, file.c_str());
}

int clamp_int(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

std::wstring widen(const char* s)
{
    wchar_t buf[64] = {};
    MultiByteToWideChar(CP_ACP, 0, s, -1, buf, 63);
    return buf;
}

Settings g_cached;
FILETIME g_cachedTime = {};
bool g_cacheValid = false;
CRITICAL_SECTION g_cacheLock;
INIT_ONCE g_lockInit = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK init_lock(PINIT_ONCE, PVOID, PVOID*)
{
    InitializeCriticalSection(&g_cacheLock);
    return TRUE;
}

const int kSampleRates[] = { 8000, 11025, 16000, 22050, 32000, 44100 };
const int kSampleRateCount = static_cast<int>(sizeof(kSampleRates) / sizeof(kSampleRates[0]));

int nearest_sample_rate(int hz)
{
    int best = kSampleRates[2];
    int bestDelta = 0x7fffffff;
    for (int i = 0; i < kSampleRateCount; ++i) {
        const int d = hz > kSampleRates[i] ? hz - kSampleRates[i] : kSampleRates[i] - hz;
        if (d < bestDelta) { bestDelta = d; best = kSampleRates[i]; }
    }
    return best;
}

}  // namespace

Settings::Settings()
{
    for (int i = 0; i < FVP_COUNT; ++i) {
        percent[i] = voices::param(static_cast<FlexVoiceParam>(i)).defaultPercent;
    }
}

Settings SettingsStore::defaults() { return Settings(); }

std::wstring SettingsStore::app_dir() { return app_data_dir(); }
std::wstring SettingsStore::path()    { return app_data_dir() + L"\\settings.ini"; }

std::wstring SettingsStore::log_dir()
{
    std::wstring dir = app_data_dir() + L"\\logs";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
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
    const std::wstring file = path();
    Settings s;

    wchar_t langBuf[16] = {};
    GetPrivateProfileStringW(L"voice", L"language", L"eng", langBuf, 16, file.c_str());
    char langCode[16] = {};
    WideCharToMultiByte(CP_ACP, 0, langBuf, -1, langCode, sizeof(langCode), nullptr, nullptr);
    const int li = voices::language_index_from_code(langCode);
    s.languageIndex = (li >= 0) ? li : 0;

    s.baseVoice = clamp_int(read_int(L"voice", L"baseVoice", 0, file), 0,
                            voices::kBaseVoiceCount - 1);

    for (int i = 0; i < FVP_COUNT; ++i) {
        const voices::ParamDef& d = voices::param(static_cast<FlexVoiceParam>(i));
        const std::wstring key = widen(d.iniKey);
        s.percent[i] = voices::clamp_percent(
            read_int(L"speech", key.c_str(), d.defaultPercent, file));
    }

    s.sampleRate = nearest_sample_rate(read_int(L"options", L"sampleRate", 16000, file));
    s.applyToAllVoices = read_int(L"options", L"applyToAllVoices", 0, file) != 0;
    s.debugLogging = read_int(L"logging", L"debug", 0, file) != 0;
    return s;
}

bool SettingsStore::save(const Settings& s)
{
    const std::wstring file = path();

    const int li = clamp_int(s.languageIndex, 0, voices::kLanguageCount - 1);
    WritePrivateProfileStringW(L"voice", L"language",
                               widen(voices::kLanguages[li].code).c_str(), file.c_str());
    write_int(L"voice", L"baseVoice", clamp_int(s.baseVoice, 0, voices::kBaseVoiceCount - 1), file);

    for (int i = 0; i < FVP_COUNT; ++i) {
        const voices::ParamDef& d = voices::param(static_cast<FlexVoiceParam>(i));
        write_int(L"speech", widen(d.iniKey).c_str(), voices::clamp_percent(s.percent[i]), file);
    }

    write_int(L"options", L"sampleRate", nearest_sample_rate(s.sampleRate), file);
    write_int(L"options", L"applyToAllVoices", s.applyToAllVoices ? 1 : 0, file);
    write_int(L"logging", L"debug", s.debugLogging ? 1 : 0, file);
    return true;
}

const Settings& SettingsStore::current()
{
    InitOnceExecuteOnce(&g_lockInit, init_lock, nullptr, nullptr);
    EnterCriticalSection(&g_cacheLock);

    FILETIME ft = {};
    const bool haveTime = file_time(ft);
    const bool changed = !g_cacheValid || (haveTime && CompareFileTime(&ft, &g_cachedTime) != 0);

    if (changed) {
        g_cached = load();
        if (haveTime) g_cachedTime = ft;
        g_cacheValid = true;
        log::enabled() = g_cached.debugLogging;
    }

    static thread_local Settings snapshot;
    snapshot = g_cached;
    LeaveCriticalSection(&g_cacheLock);
    return snapshot;
}

}  // namespace FlexVoice

// flexvoice_log.h declares this; settings owns where the files go.
namespace FlexVoice {
namespace log {
std::wstring log_dir() { return SettingsStore::log_dir(); }
}
}
