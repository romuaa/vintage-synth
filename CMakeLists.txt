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

target_sources(VintageSynth PRIVATE
    source/PluginProcessor.cpp
    source/params/ParameterLayout.cpp)

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
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

set_target_properties(VintageSynth PROPERTIES COMPILE_WARNING_AS_ERROR ON)

if(VS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(external/Catch2)
    add_subdirectory(tests)
endif()
