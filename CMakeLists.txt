cmake_minimum_required(VERSION 3.28)

# Must be set before project(). Default: macOS universal binary, macOS 11+.
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures")

project(VintageSynth VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static MSVC runtime: no VC++ redistributable needed on user machines.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(VS_BUILD_TESTS "Build unit tests" ON)

add_subdirectory(external/JUCE)

juce_add_plugin(VintageSynth
    PRODUCT_NAME                "Vintage Synth"
    COMPANY_NAME                "Exitium"
    BUNDLE_ID                   "com.exitium.vintagesynth"
    PLUGIN_MANUFACTURER_CODE    Exit
    PLUGIN_CODE                 Vsy1
    FORMATS                     VST3
    IS_SYNTH                    TRUE
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES             Instrument Synth
    HARDENED_RUNTIME_ENABLED    TRUE
    COPY_PLUGIN_AFTER_BUILD     FALSE)

# Our sources. Also compiled (without LTO) into vs_test_core for the tests; see tests/CMakeLists.txt.
set(VS_PLUGIN_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/source/PluginProcessor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/params/ParameterLayout.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/engine/Modulation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/engine/Voice.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/engine/Part.cpp)

target_sources(VintageSynth PRIVATE ${VS_PLUGIN_SOURCES})

target_include_directories(VintageSynth PRIVATE source)

target_compile_definitions(VintageSynth PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(VintageSynth
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# Link-time optimisation for the shipped plugin only. PRIVATE, so it doesn't leak into anything
# that links VintageSynth; tests use vs_test_core instead and skip the slow LTCG/LTO link.
target_link_libraries(VintageSynth PRIVATE juce::juce_recommended_lto_flags)
if(TARGET VintageSynth_VST3)
    target_link_libraries(VintageSynth_VST3 PRIVATE juce::juce_recommended_lto_flags)
endif()

set_target_properties(VintageSynth PROPERTIES COMPILE_WARNING_AS_ERROR ON)

# Linux: link the C++ runtime statically into the shipped plugin, so it depends only on glibc and
# the usual system libraries (a host may load a different libstdc++ than the one we built with).
# --exclude-libs keeps the statically linked runtime's symbols out of the plugin's exports.
if(UNIX AND NOT APPLE AND TARGET VintageSynth_VST3)
    target_link_options(VintageSynth_VST3 PRIVATE
        -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL)
endif()

if(VS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(external/Catch2)
    add_subdirectory(tests)
endif()

# Optional developer convenience: JUCE's own AudioPluginHost, built from the submodule, for hands-on
# UI testing (see scripts/dev-host.sh). Off by default; CI and normal builds are unaffected.
option(VS_BUILD_DEV_HOST "Also build JUCE's AudioPluginHost (dev tool, not shipped)" OFF)

if(VS_BUILD_DEV_HOST)
    add_subdirectory(external/JUCE/extras/AudioPluginHost
                     ${CMAKE_CURRENT_BINARY_DIR}/dev-host-build)
    # The host is JUCE's code: never apply our warnings-as-errors policy to it, even when
    # -DCMAKE_COMPILE_WARNING_AS_ERROR=ON is passed globally.
    set_target_properties(AudioPluginHost PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
    if(TARGET AudioPluginHostData)
        set_target_properties(AudioPluginHostData PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
    endif()
    # Make sure the plugin bundle is built whenever the host is.
    add_dependencies(AudioPluginHost VintageSynth_VST3)
endif()
