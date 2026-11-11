add_executable(VintageSynthTests
    dsp_unit/ProcessorTests.cpp)

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
