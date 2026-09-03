// version.h -- one place that says which build this is.
//
// Every binary carries a VERSIONINFO resource built from these, so a user can
// right-click any file and read the version, and the installer's
// Programs-and-Features entry matches. Bump FV_VERSION_PATCH for a fix,
// FV_VERSION_MINOR when behaviour changes, and remember that shipping a second
// installer under an unchanged version leaves nobody able to tell which one
// they have.

#pragma once

#define FV_VERSION_MAJOR 1
#define FV_VERSION_MINOR 0
#define FV_VERSION_PATCH 3
#define FV_VERSION_BUILD 0

#define FV_STRINGIZE2(x) #x
#define FV_STRINGIZE(x)  FV_STRINGIZE2(x)

#define FV_VERSION_STRING                                                      \
    FV_STRINGIZE(FV_VERSION_MAJOR) "." FV_STRINGIZE(FV_VERSION_MINOR) "."      \
    FV_STRINGIZE(FV_VERSION_PATCH) "." FV_STRINGIZE(FV_VERSION_BUILD)

#define FV_COMPANY     "Josh Kennedy"
#define FV_PRODUCT     "FlexVoice SAPI5"
#define FV_COPYRIGHT   "Copyright (c) 2026 Josh Kennedy. FlexVoice engine (c) Mindmaker Ltd."
