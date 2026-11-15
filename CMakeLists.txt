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
# The second plugin, Exitium B-Side (docs/architecture.md section 8). OFF builds exactly what the
# repository built before it existed: Exitium Vintage Synth and its tests.
option(VS_BUILD_BSIDE "Also build the second plugin, Exitium B-Side" ON)

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
# The second instrument (B-Side, below) has its own juce_add_plugin target and its own source list
# (BS_SOURCES) and compiles VS_COMMON_SOURCES the same way. Everything is included as "common/...",
# "vintage/..." or "bside/..." with source/ as the include root.
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
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/presets/PresetActions.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/presets/PresetBar.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/shell/InstrumentEditor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/shell/ShellSupport.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/ui/Theme.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/ui/HelpTexts.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/ui/ParamMirror.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/ui/controls/BoundComponent.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/ui/controls/Slider.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/ui/controls/SegmentedSwitch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/ui/controls/StepTypeToggle.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/ui/StepGrid.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/ui/graphs/PianoRollView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/ui/graphs/GraphView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/ui/graphs/KeyboardStrip.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/ui/graphs/FxViews.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/ui/controls/PowerSwitch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/ui/graphs/EnvelopeView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/common/ui/graphs/LfoView.cpp
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
    ${CMAKE_CURRENT_SOURCE_DIR}/source/vintage/ui/PlayPage.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/vintage/ui/EffectsPage.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/vintage/ui/ArpSeqPage.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/vintage/ui/EnvelopesPage.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/vintage/ui/LfoPage.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/vintage/ui/BrowserPage.cpp)
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
# What every plugin target of this repository shares (one call per plugin): its factory data, the
# include root, compile definitions, link libraries, plugin-only link-time optimisation, warnings as
# errors, and the Linux static C++ runtime. JUCE modules are compiled into each plugin target and the
# JucePlugin_* definitions differ per plugin, so there is deliberately no static library of our code
# shared between the plugins.
function(vs_configure_plugin target factory_data)
    set_target_properties(${factory_data} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(${target} PRIVATE ${factory_data})

    target_include_directories(${target} PRIVATE source)

    target_compile_definitions(${target} PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)

    target_link_libraries(${target}
        PRIVATE
            juce::juce_audio_utils
            juce::juce_dsp
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)

    # Link-time optimisation for the shipped plugin only. PRIVATE, so it doesn't leak into anything
    # that links the plugin; tests use a *_test_core library instead and skip the slow LTCG/LTO link.
    target_link_libraries(${target} PRIVATE juce::juce_recommended_lto_flags)
    if(TARGET ${target}_VST3)
        target_link_libraries(${target}_VST3 PRIVATE juce::juce_recommended_lto_flags)
    endif()

    set_target_properties(${target} PROPERTIES COMPILE_WARNING_AS_ERROR ON)

    # Linux: link the C++ runtime statically into the shipped plugin, so it depends only on glibc and
    # the usual system libraries (a host may load a different libstdc++ than the one we built with).
    # --exclude-libs keeps the statically linked runtime's symbols out of the plugin's exports.
    if(UNIX AND NOT APPLE AND TARGET ${target}_VST3)
        target_link_options(${target}_VST3 PRIVATE
            -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL)
    endif()
endfunction()

vs_configure_plugin(VintageSynth VintageSynthFactoryData)

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

# ---- Exitium B-Side (docs/architecture.md 8.1) ----
# BS_SOURCES: the .cpp files of source/bside/. S1c adds source/bside/program/ and source/bside/trace/
# (JUCE-free), S2 adds source/bside/chip/; they join this list. BS_PLUGIN_SOURCES is what the plugin and
# bs_test_core (tests/bside) compile; the plugin target never sees a Vintage source and vice versa.
if(VS_BUILD_BSIDE)
    juce_add_plugin(BSide
        PRODUCT_NAME                "Exitium B-Side"
        COMPANY_NAME                "Exitium"
        BUNDLE_ID                   "com.exitium.bside"
        PLUGIN_MANUFACTURER_CODE    Exit
        PLUGIN_CODE                 Bsy1
        FORMATS                     VST3
        IS_SYNTH                    TRUE
        NEEDS_MIDI_INPUT            TRUE
        NEEDS_MIDI_OUTPUT           FALSE
        IS_MIDI_EFFECT              FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        VST3_CATEGORIES             Instrument Synth
        HARDENED_RUNTIME_ENABLED    TRUE
        COPY_PLUGIN_AFTER_BUILD     FALSE)

    set(BS_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/source/bside/PluginProcessor.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/source/bside/presets/BSidePresets.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/source/bside/presets/BSideTables.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/source/bside/text/TextScanner.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/source/bside/program/Program.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/source/bside/program/TableReader.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/source/bside/program/TableWriter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/source/bside/params/ParameterLayout.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/source/bside/engine/TestTonePart.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/source/bside/engine/ProgramEngine.cpp)
    set(BS_PLUGIN_SOURCES ${VS_COMMON_SOURCES} ${BS_SOURCES})
    target_sources(BSide PRIVATE ${BS_PLUGIN_SOURCES})

    # Factory programs: presets/bside/factory/*.json, embedded like Vintage's (see above).
    file(GLOB BS_FACTORY_PRESETS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/presets/bside/factory/*.json)
    juce_add_binary_data(BSideFactoryData
        HEADER_NAME BSideFactoryData.h
        NAMESPACE   bs_factory
        SOURCES     ${BS_FACTORY_PRESETS})

    vs_configure_plugin(BSide BSideFactoryData)
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
    # No dependency on a plugin bundle: scripts/dev-host.sh builds the plugin it opens
    # (--plugin vintage|bside), so opening one plugin never needs the other built.
endif()
