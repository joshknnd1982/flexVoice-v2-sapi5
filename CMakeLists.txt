cmake_minimum_required(VERSION 3.15)
project(FlexVoiceSAPI VERSION 1.0.3 LANGUAGES C CXX)

# The single source of truth for the version is src/version.h; keep the
# project() line, the installer's MyAppVersion and that header in step.

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(BUILD_X64 TRUE)
    message(STATUS "FlexVoice: building x64 (SAPI DLL only; the engine is 32-bit)")
else()
    set(BUILD_X64 FALSE)
    message(STATUS "FlexVoice: building x86 (SAPI DLL, engine host, configuration utility)")
endif()

set(COMMON_DEFS _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

# The MindMaker FlexVoice 3.01 SDK. Not redistributable as source; the headers
# and import library live under bin/ and are excluded from the repository.
set(FV_SDK ${CMAKE_CURRENT_SOURCE_DIR}/bin/mmfvapi)

# ---------------------------------------------------------------------------
# SAPI5 in-process COM server (both architectures)
# ---------------------------------------------------------------------------
add_library(FlexVoiceSAPI SHARED
    src/sapi_main.cpp
    src/com.cpp
    src/registry.cpp
    src/ISpDataKeyImpl.cpp
    src/IEnumSpObjectTokensImpl.cpp
    src/ISpTTSEngineImpl.cpp
    src/voice_token.cpp
    src/settings.cpp
    src/engine_client.cpp
    src/text_normalize.cpp
    src/version.rc
    src/flexvoice_sapi.def
)
target_include_directories(FlexVoiceSAPI PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(FlexVoiceSAPI PRIVATE ole32 oleaut32 advapi32 shell32 shlwapi)
target_compile_definitions(FlexVoiceSAPI PRIVATE ${COMMON_DEFS}
    FV_FILE_DESCRIPTION="FlexVoice SAPI5 speech engine"
    FV_INTERNAL_NAME="FlexVoiceSAPI"
    FV_ORIGINAL_FILENAME="FlexVoiceSAPI.dll"
    FV_FILETYPE=VFT_DLL)
set_property(TARGET FlexVoiceSAPI PROPERTY CXX_STANDARD 17)

# Pipe-protocol exerciser, built for both architectures so the 64->32 bridge
# can be tested without a SAPI host.
add_executable(client_test
    test/client_test.cpp
    src/engine_client.cpp
    src/settings.cpp
    src/text_normalize.cpp
    src/version.rc
)
target_include_directories(client_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(client_test PRIVATE advapi32 shell32 shlwapi winmm)
target_compile_definitions(client_test PRIVATE ${COMMON_DEFS}
    FV_FILE_DESCRIPTION="FlexVoice SAPI5 diagnostics"
    FV_INTERNAL_NAME="flexvoice_diag")
set_property(TARGET client_test PROPERTY CXX_STANDARD 17)

# Drives write_voice_tokens and remove_voice_tokens themselves, against a
# sandbox key in HKCU. sapi_probe registers and unregisters with a helper of
# its own, so the product's uninstall path had never been run by a test -- and
# it did not work. Built for both architectures because that path deletes from
# whichever registry view the caller happens to be in.
add_executable(registry_test test/registry_test.cpp src/registry.cpp)
target_include_directories(registry_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(registry_test PRIVATE advapi32)
target_compile_definitions(registry_test PRIVATE ${COMMON_DEFS})
set_property(TARGET registry_test PROPERTY CXX_STANDARD 17)

# Exercises the real SAPI5 stack. Registers under HKCU so it can run without
# elevation, which is the only way the registry path SAPI actually takes gets
# tested before install time.
add_executable(sapi_probe test/sapi_probe.cpp)
target_link_libraries(sapi_probe PRIVATE ole32 oleaut32 sapi advapi32)
target_compile_definitions(sapi_probe PRIVATE ${COMMON_DEFS})
set_property(TARGET sapi_probe PROPERTY CXX_STANDARD 17)

if(NOT BUILD_X64)
    # -----------------------------------------------------------------------
    # Engine host. 32-bit only: the FlexVoice engine DLL is x86.
    # EngineFactory::createEngine returns std::auto_ptr, which C++17 removed --
    # _HAS_AUTO_PTR_ETC=1 puts it back without dropping the target to C++14,
    # and the mangled name is the same either way, so the 2003 import library
    # still resolves.
    # -----------------------------------------------------------------------
    add_executable(flexvoice_host
        src/flexvoice_host.cpp
        src/fv_engine.cpp
        src/settings.cpp
        src/text_normalize.cpp
        src/version.rc
    )
    target_include_directories(flexvoice_host PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${FV_SDK}/include
    )
    target_link_directories(flexvoice_host PRIVATE ${FV_SDK}/lib)
    target_link_libraries(flexvoice_host PRIVATE
        FlexVoice_3_01_001.lib advapi32 shell32 shlwapi)
    target_compile_definitions(flexvoice_host PRIVATE ${COMMON_DEFS} _HAS_AUTO_PTR_ETC=1
        FV_FILE_DESCRIPTION="FlexVoice SAPI5 engine host"
        FV_INTERNAL_NAME="flexvoice_host"
        FV_ORIGINAL_FILENAME="flexvoice_host.exe")
    set_property(TARGET flexvoice_host PROPERTY CXX_STANDARD 17)
    # The SDK requires RTTI (Engine.h #errors without _CPPRTTI) and throws.
    target_compile_options(flexvoice_host PRIVATE /EHsc /GR)

    # Reads the configuration utility the way a screen reader does: MSAA names,
    # roles, tab order and access keys, against the live window.
    add_executable(a11y_probe test/a11y_probe.cpp)
    target_link_libraries(a11y_probe PRIVATE oleacc ole32 oleaut32 user32)
    target_compile_definitions(a11y_probe PRIVATE ${COMMON_DEFS})
    set_property(TARGET a11y_probe PROPERTY CXX_STANDARD 17)

    # -----------------------------------------------------------------------
    # Configuration utility
    # -----------------------------------------------------------------------
    add_executable(FlexVoiceConfig WIN32
        src/config/FlexVoiceConfig.cpp
        src/config/FlexVoiceConfig.rc
        src/version.rc
        src/settings.cpp
        src/engine_client.cpp
        src/text_normalize.cpp
    )
    target_include_directories(FlexVoiceConfig PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/config
    )
    target_link_libraries(FlexVoiceConfig PRIVATE
        comctl32 winmm advapi32 shell32 shlwapi ole32 oleaut32)
    target_compile_definitions(FlexVoiceConfig PRIVATE ${COMMON_DEFS}
        FV_FILE_DESCRIPTION="FlexVoice configuration utility"
        FV_INTERNAL_NAME="FlexVoiceConfig"
        FV_ORIGINAL_FILENAME="FlexVoiceConfig.exe")
    set_property(TARGET FlexVoiceConfig PROPERTY CXX_STANDARD 17)
endif()

if(MSVC)
    foreach(tgt FlexVoiceSAPI client_test registry_test sapi_probe a11y_probe flexvoice_host FlexVoiceConfig)
        if(TARGET ${tgt})
            target_compile_options(${tgt} PRIVATE /W3 /MP /utf-8)
            set_property(TARGET ${tgt} PROPERTY
                MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
    endforeach()
endif()
