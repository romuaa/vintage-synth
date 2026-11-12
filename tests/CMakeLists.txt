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
        juce::juce_audio_utils
        juce::juce_dsp
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
    dsp_unit/RenderDemo.cpp)

target_link_libraries(VintageSynthTests PRIVATE
    vs_test_core
    Catch2::Catch2WithMain)

include(${PROJECT_SOURCE_DIR}/external/Catch2/extras/Catch.cmake)
catch_discover_tests(VintageSynthTests)

# CPU benchmark (not a test; run manually or from the validate-plugin skill).
add_executable(voice_bench benchmarks/VoiceBench.cpp)
target_link_libraries(voice_bench PRIVATE vs_test_core)

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
         COMMAND host_smoke "$<GENEX_EVAL:$<TARGET_PROPERTY:VintageSynth_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>")

# Golden-render regression harness (ctest label "render").
add_subdirectory(render_regression)
