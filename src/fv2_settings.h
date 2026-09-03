// fv2_settings.h -- persistent user settings, in
// %APPDATA%\FlexVoice2SAPI\settings.ini.
//
// The SAPI DLLs reload the file whenever its timestamp changes, so a change in
// the configuration utility takes effect on the very next utterance without
// restarting the screen reader. That matters more than it sounds: a
// screen-reader user adjusting a voice cannot restart NVDA to hear each change.

#pragma once

#include <windows.h>

#include <string>

#include "fv2_speaker.hpp"

namespace fv2 {

struct Settings {
    // [voice] -- these apply to the FlexVoice2 Custom Voice only. The named
    // voices keep their own character, or every one of them would sound alike.
    std::string baseVoice = "Julie";     // which built-in voice it starts from

    // [speech] -- one whole percentage per parameter, 0 = minimum, 100 = maximum,
    // exactly as the configuration utility presents them.
    int percent[P_COUNT];

    // [options]
    int  sampleRate = 16000;             // 8000/11025/16000/22050/32000/44100

    // [logging]
    bool debugLogging = true;            // lifecycle and errors
    bool verboseLogging = false;         // per-utterance detail

    Settings();
};

class SettingsStore {
public:
    static std::wstring path();          // %APPDATA%\FlexVoice2SAPI\settings.ini
    static std::wstring app_dir();       // %APPDATA%\FlexVoice2SAPI

    static Settings load();
    static bool     save(const Settings& s);
    static Settings defaults();

    // Cached; reloaded when settings.ini changes underneath.
    static const Settings& current();

private:
    static bool file_time(FILETIME& ft);
};

}  // namespace fv2
