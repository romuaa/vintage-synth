add_executable(VintageSynthTests
    dsp_unit/ProcessorTests.cpp
    dsp_unit/OscillatorTests.cpp
    dsp_unit/FilterTests.cpp
    dsp_unit/EnvelopeLfoTests.cpp
    dsp_unit/PartTests.cpp
    dsp_unit/RenderDemo.cpp)

# Compile with exactly the same include paths and JUCE config as the plugin's
# shared code, so JUCE modules are not compiled a second time.
target_include_directories(VintageSynthTests PRIVATE
    $<TARGET_PROPERTY:VintageSynth,INCLUDE_DIRECTORIES>)
target_compile_definitions(VintageSynthTests PRIVATE
    $<TARGET_PROPERTY:VintageSynth,COMPILE_DEFINITIONS>)

# Link against the plugin's shared-code target (contains JUCE modules + our sources).
target_link_libraries(VintageSynthTests PRIVATE
    VintageSynth
    Catch2::Catch2WithMain)

include(${PROJECT_SOURCE_DIR}/external/Catch2/extras/Catch.cmake)
catch_discover_tests(VintageSynthTests)

# CPU benchmark (not a test; run manually or from the validate-plugin skill).
add_executable(voice_bench benchmarks/VoiceBench.cpp)
target_include_directories(voice_bench PRIVATE $<TARGET_PROPERTY:VintageSynth,INCLUDE_DIRECTORIES>)
target_compile_definitions(voice_bench PRIVATE $<TARGET_PROPERTY:VintageSynth,COMPILE_DEFINITIONS>)
target_link_libraries(voice_bench PRIVATE VintageSynth)

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
