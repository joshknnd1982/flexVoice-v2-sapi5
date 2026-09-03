// flexvoice_log.h -- append-only debug log shared by the SAPI DLLs, the host
// and the configuration utility.
//
// Three processes append into the same directory, so: compose the whole line
// and write it once, and open with _SH_DENYNO -- an exclusive open would let
// the first process to log lock every other one out.
//
// Never open with a "ccs=UTF-8" mode. The CRT fail-fasts inside fprintf on a
// ccs-mode handle in some configurations, which leaves a 3-byte BOM-only file
// and a dead process. Plain "a" plus UTF-8 bytes we encode ourselves.

#pragma once

#include <windows.h>
#include <share.h>
#include <stdarg.h>
#include <stdio.h>
#include <string>

namespace FlexVoice {
namespace log {

// Set once at startup by each component: "sapi32", "sapi64", "host", "config".
inline std::wstring& component()
{
    static std::wstring name = L"flexvoice";
    return name;
}

inline bool& enabled()
{
    static bool on = false;
    return on;
}

// %APPDATA%\FlexVoiceSAPI\logs, created on demand.
std::wstring log_dir();

inline std::wstring log_path()
{
    return log_dir() + L"\\" + component() + L".log";
}

// 4 MB cap with a single .1 predecessor, so a chatty session cannot fill a disk.
inline void rotate_if_needed(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return;
    const ULONGLONG size = (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    if (size < 4ull * 1024 * 1024) return;
    const std::wstring prev = path + L".1";
    DeleteFileW(prev.c_str());
    MoveFileW(path.c_str(), prev.c_str());
}

inline void write_line(const char* fmt, ...)
{
    if (!enabled()) return;

    char body[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);

    char line[4608];
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "%04u-%02u-%02u %02u:%02u:%02u.%03u [pid %lu tid %lu] %s\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                st.wMilliseconds,
                static_cast<unsigned long>(GetCurrentProcessId()),
                static_cast<unsigned long>(GetCurrentThreadId()), body);

    const std::wstring path = log_path();
    rotate_if_needed(path);
    FILE* f = _wfsopen(path.c_str(), L"a", _SH_DENYNO);
    if (!f) return;
    fputs(line, f);          // one write per line: the file is shared
    fclose(f);
}

}  // namespace log
}  // namespace FlexVoice

#define FV_LOG(...) ::FlexVoice::log::write_line(__VA_ARGS__)
