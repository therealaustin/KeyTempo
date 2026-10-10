cmake_minimum_required(VERSION 3.22)

project(KeyTempo VERSION 0.3.0 LANGUAGES C CXX)

# ----------------------------------------------------------------------------
# Identity — change these to your own before you ship anything.
# PLUGIN_MANUFACTURER_CODE must have at least one upper-case letter, and
# PLUGIN_CODE must be exactly 4 characters with at least one upper-case letter.
# ----------------------------------------------------------------------------
set(KT_COMPANY_NAME        "KeyTempo Audio")
set(KT_MANUFACTURER_CODE   "Ktpo")
set(KT_PLUGIN_CODE         "Kt01")
set(KT_BUNDLE_ID           "com.keytempoaudio.keytempo")
set(KT_LV2_URI             "https://github.com/your-name/KeyTempo")

option(KT_BUILD_PLUGIN "Build the JUCE plug-in targets" ON)
option(KT_BUILD_TESTS  "Build the analysis core unit tests" ON)
set(KT_JUCE_PATH "" CACHE PATH "Use a local JUCE checkout instead of downloading one")
set(KT_AAX_SDK_PATH "" CACHE PATH "Path to the Avid AAX SDK (enables the AAX format)")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Sources contain UTF-8 (♯, ♭); make MSVC read them as such.
if(MSVC)
    add_compile_options(/utf-8)
endif()
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

# ----------------------------------------------------------------------------
# Analysis core — plain C++, no JUCE, so it can be unit-tested anywhere.
# ----------------------------------------------------------------------------
add_library(kt_core STATIC
    src/core/Fft.cpp
    src/core/TempoDetector.cpp
    src/core/GrooveModel.cpp
    src/core/KeyDetector.cpp
    src/core/MusicTheory.cpp
    src/core/AnalysisEngine.cpp)
target_include_directories(kt_core PUBLIC src/core)
if(MSVC)
    target_compile_options(kt_core PRIVATE /W4)
else()
    target_compile_options(kt_core PRIVATE -Wall -Wextra -O2)
endif()

if(KT_BUILD_TESTS)
    enable_testing()
    add_executable(kt_core_tests tests/CoreTests.cpp)
    target_link_libraries(kt_core_tests PRIVATE kt_core)
    add_test(NAME kt_core_tests COMMAND kt_core_tests)

    # Tempo benchmark over synthesised genre tracks (not part of ctest; run it by hand).
    add_executable(kt_tempo_bench tests/TempoBench.cpp)
    target_link_libraries(kt_tempo_bench PRIVATE kt_core)
    find_package(Threads REQUIRED)
    target_link_libraries(kt_tempo_bench PRIVATE Threads::Threads)
endif()

if(NOT KT_BUILD_PLUGIN)
    return()
endif()

# ----------------------------------------------------------------------------
# JUCE + CLAP
# ----------------------------------------------------------------------------
include(FetchContent)

if(KT_JUCE_PATH)
    add_subdirectory(${KT_JUCE_PATH} ${CMAKE_BINARY_DIR}/JUCE EXCLUDE_FROM_ALL)
else()
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        9.0.3
        GIT_SHALLOW    ON)
    FetchContent_MakeAvailable(JUCE)
endif()

FetchContent_Declare(clap_juce_extensions
    GIT_REPOSITORY https://github.com/free-audio/clap-juce-extensions.git
    # Pinned past 0.26.0: JUCE 9 support landed after the last tagged release.
    GIT_TAG        7adee3a1bd4684d4caa5601100e364abccff4b4d
    GIT_SUBMODULES_RECURSE ON)
FetchContent_MakeAvailable(clap_juce_extensions)

set(KT_FORMATS VST3 LV2 Standalone)
if(APPLE)
    list(APPEND KT_FORMATS AU)
endif()
if(KT_AAX_SDK_PATH)
    juce_set_aax_sdk_path(${KT_AAX_SDK_PATH})
    list(APPEND KT_FORMATS AAX)
endif()

juce_add_plugin(KeyTempo
    COMPANY_NAME             "${KT_COMPANY_NAME}"
    PRODUCT_NAME             "KeyTempo"
    PLUGIN_MANUFACTURER_CODE ${KT_MANUFACTURER_CODE}
    PLUGIN_CODE              ${KT_PLUGIN_CODE}
    BUNDLE_ID                ${KT_BUNDLE_ID}
    LV2URI                   ${KT_LV2_URI}
    FORMATS                  ${KT_FORMATS}
    IS_SYNTH                 FALSE
    NEEDS_MIDI_INPUT         FALSE
    NEEDS_MIDI_OUTPUT        FALSE
    IS_MIDI_EFFECT           FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD  FALSE
    VST3_CATEGORIES          Analyzer Fx
    AU_MAIN_TYPE             kAudioUnitType_Effect
    AAX_CATEGORY             AAX_ePlugInCategory_None
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "KeyTempo listens to your input to detect tempo and key.")

clap_juce_extensions_plugin(TARGET KeyTempo
    CLAP_ID       "${KT_BUNDLE_ID}"
    CLAP_FEATURES analyzer utility audio-effect stereo mono)

target_sources(KeyTempo PRIVATE
    src/plugin/PluginProcessor.cpp
    src/plugin/PluginEditor.cpp
    src/plugin/LookAndFeel.cpp)

target_compile_definitions(KeyTempo PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(KeyTempo
    PRIVATE
        kt_core
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ----------------------------------------------------------------------------
# Developer tool: offline analysis of audio files + editor screenshots.
# ----------------------------------------------------------------------------
option(KT_BUILD_TOOLS "Build kt_tool (offline analyzer / UI snapshot)" OFF)
if(KT_BUILD_TOOLS)
    juce_add_console_app(kt_tool PRODUCT_NAME "kt_tool")
    target_sources(kt_tool PRIVATE tools/KtTool.cpp)
    # Reuse the plug-in's compiled shared code (processor, editor and JUCE modules).
    target_link_libraries(kt_tool PRIVATE KeyTempo kt_core)
    target_include_directories(kt_tool PRIVATE $<TARGET_PROPERTY:KeyTempo,INCLUDE_DIRECTORIES>)
    target_compile_definitions(kt_tool PRIVATE $<TARGET_PROPERTY:KeyTempo,COMPILE_DEFINITIONS>)
endif()
