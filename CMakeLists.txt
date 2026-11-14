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

# GCC on 64-bit ARM flags a loop in the audio codec sources bundled with JUCE (compiled into the
# test targets through juce_audio_formats). It is third-party C code: keep the warning, don't fail on it.
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    add_compile_options($<$<COMPILE_LANGUAGE:C>:-Wno-error=aggressive-loop-optimizations>)
endif()

juce_add_plugin(VintageSynth
    PRODUCT_NAME                "Exitium Vintage Synth"
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
#   VS_COMMON_SOURCES   shared by every instrument in this repository (source/common/)
#   VS_VINTAGE_SOURCES  this instrument only (source/vintage/)
# A second instrument adds its own juce_add_plugin target and its own VS_<NAME>_SOURCES list and
# links VS_COMMON_SOURCES the same way as below. Everything is included as "common/..." or
# "vintage/..." with source/ as the include root.
set(VS_COMMON_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/params/ParamFactory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/params/FxArpParams.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/params/GlobalParams.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/presets/PresetModel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/presets/StrictJson.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/presets/PresetFormat.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/presets/PresetFiles.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/presets/FactoryBank.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/presets/PresetManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/presets/PresetBar.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/shell/InstrumentEditor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/shell/ShellSupport.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/ui/Theme.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/ui/HelpTexts.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/ui/ParamMirror.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/ui/controls/BoundComponent.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/ui/controls/Slider.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/ui/controls/SegmentedSwitch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/ui/graphs/GraphView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/ui/frame/FrameParts.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/ui/frame/EditorFrame.cpp)
set(VS_VINTAGE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/source/vintage/PluginProcessor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/vintage/presets/VintagePresets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/vintage/params/ParameterLayout.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/vintage/engine/Modulation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/vintage/engine/Voice.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/vintage/engine/Part.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/vintage/ui/VintageEditor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/vintage/ui/SoundPage.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/vintage/ui/PlaceholderPage.cpp)
set(VS_PLUGIN_SOURCES ${VS_COMMON_SOURCES} ${VS_VINTAGE_SOURCES})

target_sources(VintageSynth PRIVATE ${VS_PLUGIN_SOURCES})

# Factory presets: presets/factory/*.json are embedded in the binary (JUCE binary data), so the
# plugin reads them from memory at run time and needs no file at an install path. One generated
# C++ array source per target keeps all five platforms and the non-LTO test library identical (the
# same static library links into the plugin and into vs_test_core); a CMake-generated source of
# our own would have to be kept in step with five toolchains. A file's name starts with its slot
# (A1 ... H8). The list is globbed with CONFIGURE_DEPENDS so a new file is picked up.
file(GLOB VS_FACTORY_PRESETS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/presets/factory/*.json)
juce_add_binary_data(VintageSynthFactoryData
    HEADER_NAME FactoryData.h
    NAMESPACE   vs_factory
    SOURCES     ${VS_FACTORY_PRESETS})
set_target_properties(VintageSynthFactoryData PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(VintageSynth PRIVATE VintageSynthFactoryData)

# Editor data (docs/architecture.md 9.1.1): the help texts (docs/ui/help-texts.md, the single
# source) and the embedded Inter font (resources/fonts/inter, SIL OFL 1.1: see SOURCE.txt there).
juce_add_binary_data(VintageUiData
    HEADER_NAME UiData.h
    NAMESPACE   vs_ui
    SOURCES     ${CMAKE_CURRENT_SOURCE_DIR}/docs/ui/help-texts.md
                ${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts/inter/Inter-Regular.ttf
                ${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts/inter/Inter-SemiBold.ttf)
set_target_properties(VintageUiData PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(VintageSynth PRIVATE VintageUiData)

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
