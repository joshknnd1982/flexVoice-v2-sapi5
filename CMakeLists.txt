cmake_minimum_required(VERSION 3.20)
project(FlexVoice2SAPI CXX)

# FlexVoice_2_00_010.dll is a 32-bit DLL and no 64-bit build of it exists, so
# only the SAPI server is built both ways. Everything that touches the engine
# -- the host and the probes -- is 32-bit only, and the 64-bit SAPI DLL reaches
# the engine through the host over a named pipe.
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(FV2_X86 TRUE)
else()
    set(FV2_X86 FALSE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

add_compile_definitions(
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)
add_compile_options(/W3 /EHsc /GR)

set(FV2_ENGINE_LIB ${CMAKE_SOURCE_DIR}/sdk/fv2lib/FlexVoice_2_00_010.lib)

# ---------------------------------------------------------------------------
# Shared sources
# ---------------------------------------------------------------------------

set(FV2_COMMON
    src/fv2_speaker.cpp
    src/fv2_settings.cpp
)

# ---------------------------------------------------------------------------
# The SAPI 5 in-process server, built for both architectures
# ---------------------------------------------------------------------------

add_library(FlexVoice2SAPI SHARED
    src/sapi_main.cpp
    src/ISpTTSEngineImpl.cpp
    src/ISpDataKeyImpl.cpp
    src/com.cpp
    src/registry.cpp
    src/fv2_client.cpp
    src/fv2_text.cpp
    ${FV2_COMMON}
    src/version.rc
)
target_link_options(FlexVoice2SAPI PRIVATE "/DEF:${CMAKE_SOURCE_DIR}/src/fv2_sapi.def")
target_link_libraries(FlexVoice2SAPI PRIVATE
    sapi ole32 oleaut32 advapi32 shlwapi shell32 user32)
set_target_properties(FlexVoice2SAPI PROPERTIES OUTPUT_NAME "FlexVoice2SAPI")

# ---------------------------------------------------------------------------
# 32-bit only: the engine host, the configuration utility and the probes
# ---------------------------------------------------------------------------

if(FV2_X86)
    add_executable(fv2_host
        src/fv2_host.cpp
        src/fv2_engine.cpp
        ${FV2_COMMON}
        src/version.rc
    )

    # Everything 32-bit lands in one directory, so one copy of the engine DLL
    # beside it serves the host, the probes and the tests. Without this they
    # build fine and then exit silently at startup, because a missing DLL is a
    # loader failure and never reaches main().
    add_custom_command(TARGET fv2_host POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/engine/FlexVoice_2_00_010.dll"
                "$<TARGET_FILE_DIR:fv2_host>/FlexVoice_2_00_010.dll"
        COMMENT "Copying the FlexVoice 2.0 engine next to the 32-bit binaries")
    target_link_libraries(fv2_host PRIVATE ${FV2_ENGINE_LIB} shell32 ole32)
    # std::auto_ptr is in EngineFactory::createEngine's mangled return type, so
    # the name only matches if the type really is std::auto_ptr -- gone in C++17.
    target_compile_features(fv2_host PRIVATE cxx_std_14)
    set_target_properties(fv2_host PROPERTIES CXX_STANDARD 14)

    add_executable(FlexVoice2Config WIN32
        src/config/FlexVoice2Config.cpp
        src/fv2_client.cpp
        ${FV2_COMMON}
        src/config/FlexVoice2Config.rc
        src/version.rc
    )
    target_link_libraries(FlexVoice2Config PRIVATE
        comctl32 winmm ole32 oleaut32 oleacc shlwapi shell32 user32 advapi32 gdi32)

    # --- tests ------------------------------------------------------------
    add_executable(speaker_test test/speaker_test.cpp src/fv2_speaker.cpp)

    add_executable(engine_test
        test/engine_test.cpp src/fv2_engine.cpp ${FV2_COMMON})
    target_link_libraries(engine_test PRIVATE ${FV2_ENGINE_LIB} shell32 ole32)
    set_target_properties(engine_test PROPERTIES CXX_STANDARD 14)

    add_executable(client_test
        test/client_test.cpp src/fv2_client.cpp ${FV2_COMMON})
    target_link_libraries(client_test PRIVATE shlwapi shell32 ole32)

    add_executable(sapi_probe test/sapi_probe.cpp src/registry.cpp)
    target_link_libraries(sapi_probe PRIVATE sapi ole32 oleaut32 advapi32 user32)

    add_executable(a11y_probe test/a11y_probe.cpp)
    target_link_libraries(a11y_probe PRIVATE user32 ole32 oleaut32 oleacc)

    add_executable(fv2_render
        probe/fv2_render.cpp src/fv2_speaker.cpp)
    target_link_libraries(fv2_render PRIVATE ${FV2_ENGINE_LIB})
    set_target_properties(fv2_render PROPERTIES CXX_STANDARD 14)
endif()
