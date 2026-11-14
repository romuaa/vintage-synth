# vs_juce_modules: the JUCE modules the test cores use (audio_utils, dsp and what they pull in),
# compiled ONCE and shared by vs_test_core and bs_test_core (tests/bside). They link the modules
# PRIVATE: JUCE's module targets carry their .cpp files as interface sources, so a PUBLIC link would
# compile them again into every consumer (and the two copies would collide at link time). What the
# consumers need to compile against the modules (include directories, JUCE_* definitions) they take
# from their own plugin target, as before; the library itself is built with Vintage's. The JucePlugin_*
# definitions differ per plugin, but no JUCE module code the cores use depends on them (the plugin
# client module is not part of it). Nothing here touches the shipped plugin targets.
add_library(vs_juce_modules STATIC)
target_include_directories(vs_juce_modules PRIVATE
    $<TARGET_PROPERTY:VintageSynth,INCLUDE_DIRECTORIES>)
target_compile_definitions(vs_juce_modules PRIVATE
    $<TARGET_PROPERTY:VintageSynth,COMPILE_DEFINITIONS>)
target_link_libraries(vs_juce_modules
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
set_target_properties(vs_juce_modules PROPERTIES COMPILE_WARNING_AS_ERROR ON)

# vs_test_core: the plugin's code (our sources + JUCE modules) compiled again *without* LTO.
# Linking tests against the LTO-built VintageSynth shared code made every test executable pay
# a full link-time code generation (about 5 min per Windows CI job). Same sources, same
# include paths and JucePlugin_* definitions as the plugin; only the optimisation mode differs.
add_library(vs_test_core STATIC ${VS_PLUGIN_SOURCES})
target_include_directories(vs_test_core PUBLIC
    $<TARGET_PROPERTY:VintageSynth,INCLUDE_DIRECTORIES>)
target_compile_definitions(vs_test_core PUBLIC
    $<TARGET_PROPERTY:VintageSynth,COMPILE_DEFINITIONS>)
target_link_libraries(vs_test_core
    PRIVATE
        vs_juce_modules
        VintageSynthFactoryData
        VintageUiData
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
set_target_properties(vs_test_core PROPERTIES COMPILE_WARNING_AS_ERROR ON)

add_executable(VintageSynthTests
    dsp_unit/ProcessorTests.cpp
    dsp_unit/OscillatorTests.cpp
    dsp_unit/FilterTests.cpp
    dsp_unit/EnvelopeLfoTests.cpp
    dsp_unit/PartTests.cpp
    dsp_unit/EventTests.cpp
    dsp_unit/FastMathTests.cpp
    dsp_unit/VoiceAllocatorTests.cpp
    dsp_unit/PolyphonySpecTests.cpp
    dsp_unit/KeyRouterTests.cpp
    dsp_unit/BitimbralTests.cpp
    dsp_unit/OversamplingTests.cpp
    dsp_unit/SyncCrossModTests.cpp
    dsp_unit/RenderDemo.cpp
    dsp_unit/RenderDemoM3.cpp)

# M5 effect blocks (header-only, source/common/fx/).
target_sources(VintageSynthTests PRIVATE
    fx_unit/ChorusTests.cpp
    fx_unit/DelayTests.cpp
    fx_unit/ReverbTests.cpp
    fx_unit/FxRenderDemo.cpp)

# M4 arpeggiator and step sequencer (header-only, source/common/seq/).
target_sources(VintageSynthTests PRIVATE
    seq_unit/ClockTests.cpp
    seq_unit/TimingTests.cpp
    seq_unit/ArpTests.cpp
    seq_unit/SeqTests.cpp
    seq_unit/ModeTests.cpp
    seq_unit/InvariantTests.cpp
    seq_unit/PedalTests.cpp
    seq_unit/OverflowTests.cpp
    seq_unit/DriftTests.cpp
    seq_unit/ArpPatternTests.cpp
    seq_unit/MusicalTimeTests.cpp
    seq_unit/FuzzTests.cpp)

# Wave 3 integration: effects and arpeggiator/sequencer in the processor (docs/architecture.md 6a).
target_sources(VintageSynthTests PRIVATE
    integration/FxIntegrationTests.cpp
    integration/ArpIntegrationTests.cpp
    integration/RobustnessTests.cpp
    integration/OutputStageTests.cpp
    integration/RenderDemoM5.cpp)

# M6a presets and banks (docs/architecture.md 6b): files, manager, factory bank, state, MIDI, host list.
target_sources(VintageSynthTests PRIVATE
    preset_unit/PresetFormatTests.cpp
    preset_unit/PresetManagerTests.cpp
    preset_unit/StrictJsonTests.cpp
    preset_unit/FactoryBankTests.cpp
    preset_unit/FactoryRender.cpp
    preset_unit/PresetDump.cpp
    preset_unit/PresetTableFormatTests.cpp
    preset_unit/PresetTableManagerTests.cpp)
target_compile_definitions(VintageSynthTests PRIVATE VS_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

# M7a custom editor (docs/architecture.md 9.6): models, help texts, UI feed, UI state, filter curve,
# registry, automation follow, gestures, layout snapshot, open/close; hidden [.uishots] and [.uibench].
target_sources(VintageSynthTests PRIVATE
    ui/ModelTests.cpp
    ui/HelpTextTests.cpp
    ui/UiFeedTests.cpp
    ui/UiStateTests.cpp
    ui/PictureTests.cpp
    ui/EditorTests.cpp
    ui/EditorBehaviourTests.cpp
    ui/PlayEffectsTests.cpp
    ui/ParamTextTests.cpp
    ui/ArpSeqPageTests.cpp
    ui/UiShots.cpp)
target_compile_definitions(VintageSynthTests PRIVATE
    VS_UI_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/ui/golden"
    VS_HELP_TEXTS_FILE="${PROJECT_SOURCE_DIR}/docs/ui/help-texts.md")

target_link_libraries(VintageSynthTests PRIVATE
    vs_test_core
    VintageUiData
    Catch2::Catch2WithMain)

include(${PROJECT_SOURCE_DIR}/external/Catch2/extras/Catch.cmake)
catch_discover_tests(VintageSynthTests)

# CPU benchmark (not a test; run manually or from the validate-plugin skill).
add_executable(voice_bench benchmarks/VoiceBench.cpp)
target_link_libraries(voice_bench PRIVATE vs_test_core)

# Effect CPU benchmark: chorus, delay, reverb and all three in series (stereo, 48 kHz).
add_executable(fx_bench benchmarks/FxBench.cpp)
target_link_libraries(fx_bench PRIVATE vs_test_core)

# Host smoke test: loads the real VST3 bundle through JUCE's VST3 hosting.
juce_add_console_app(host_smoke PRODUCT_NAME "host_smoke")
target_sources(host_smoke PRIVATE host_smoke/HostSmoke.cpp)
target_compile_definitions(host_smoke PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=0
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)
target_link_libraries(host_smoke PRIVATE
    juce::juce_audio_processors_headless
    juce::juce_events
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
add_dependencies(host_smoke VintageSynth_VST3)
add_test(NAME host_smoke
         COMMAND host_smoke --profile vintage "$<GENEX_EVAL:$<TARGET_PROPERTY:VintageSynth_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>")
if(VS_BUILD_BSIDE)
    add_dependencies(host_smoke BSide_VST3)
    add_test(NAME host_smoke_bside
             COMMAND host_smoke --profile bside "$<GENEX_EVAL:$<TARGET_PROPERTY:BSide_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>")
endif()

# Golden-render regression harness (ctest label "render").
add_subdirectory(render_regression)

# Exitium B-Side (tests/bside/CMakeLists.txt): the JUCE-free unit tests (bside_unit: table reader/writer,
# trace formats, comparer; always built) and, with VS_BUILD_BSIDE, the plugin shell's test core and BSideTests.
add_subdirectory(bside)

# Steinberg VST3 validator (optional). Pass the path of a built `validator` binary, e.g. from CI:
#   cmake ... -DVS_VST3_VALIDATOR=/path/to/validator
# Unset (the default): no test is added and nothing else changes.
set(VS_VST3_VALIDATOR "" CACHE FILEPATH "Path to the VST3 SDK validator executable (optional)")
if(VS_VST3_VALIDATOR)
    if(NOT EXISTS "${VS_VST3_VALIDATOR}")
        message(FATAL_ERROR "VS_VST3_VALIDATOR does not exist: ${VS_VST3_VALIDATOR}")
    endif()
    add_test(NAME vst3_validator
             COMMAND "${VS_VST3_VALIDATOR}"
                     "$<GENEX_EVAL:$<TARGET_PROPERTY:VintageSynth_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>")
    # Exit code alone is not enough: require the validator's own summary with at least one test
    # run and none failed, so a validator that loads nothing cannot pass silently.
    set_tests_properties(vst3_validator PROPERTIES
        LABELS vst3
        PASS_REGULAR_EXPRESSION "Result: [1-9][0-9]* tests passed, 0 tests failed")
    if(VS_BUILD_BSIDE)
        add_test(NAME vst3_validator_bside
                 COMMAND "${VS_VST3_VALIDATOR}"
                         "$<GENEX_EVAL:$<TARGET_PROPERTY:BSide_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>")
        set_tests_properties(vst3_validator_bside PROPERTIES
            LABELS vst3
            PASS_REGULAR_EXPRESSION "Result: [1-9][0-9]* tests passed, 0 tests failed")
    endif()
endif()
