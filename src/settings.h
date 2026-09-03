#pragma once

#include <windows.h>
#include <string>

#include "voice_data.hpp"

// Persistent user settings, in %APPDATA%\FlexVoiceSAPI\settings.ini.
// The SAPI DLLs reload the file whenever its timestamp changes, so a change in
// the configuration utility takes effect on the very next utterance without
// restarting the screen reader.

namespace FlexVoice {

struct Settings {
    // [voice] -- these apply to the FlexVoice Custom Voice only. The named
    // presets keep their own character, or every one of them would sound alike.
    int  baseVoice = 0;                  // index into voices::kBaseVoices
    int  languageIndex = 0;              // index into voices::kLanguages

    // [speech] -- one whole percentage per parameter, 0 = minimum, 100 = maximum
    int  percent[FVP_COUNT];

    // [options]
    int  sampleRate = 16000;             // 8000/11025/16000/22050/32000/44100
    bool applyToAllVoices = false;       // let the utility's rate/volume also
                                         // scale the named presets

    // [logging]
    bool debugLogging = false;

    Settings();
};

class SettingsStore {
public:
    static std::wstring path();          // %APPDATA%\FlexVoiceSAPI\settings.ini
    static std::wstring log_dir();       // %APPDATA%\FlexVoiceSAPI\logs
    static std::wstring app_dir();       // %APPDATA%\FlexVoiceSAPI

    static Settings load();
    static bool     save(const Settings& s);
    static Settings defaults();

    // Cached; reloaded when settings.ini changes underneath.
    static const Settings& current();

private:
    static bool file_time(FILETIME& ft);
};

}  // namespace FlexVoice
