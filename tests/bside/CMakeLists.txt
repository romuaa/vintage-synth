# Exitium B-Side tests (docs/architecture.md 8.1.2). Included from tests/CMakeLists.txt when
# VS_BUILD_BSIDE is ON.
#
# bs_test_core is B-Side's plugin code (BS_PLUGIN_SOURCES: the common sources + source/bside/ + JUCE
# modules) compiled again without LTO, with the include paths and JucePlugin_* definitions of the BSide
# plugin target: the vs_test_core pattern. It defines createPluginFilter() for B-Side exactly like
# vs_test_core does for Vintage, so THE TWO TEST CORES MUST NEVER BE LINKED INTO ONE EXECUTABLE (the
# linker rejects it: duplicate symbols, and the JucePlugin_* definitions differ). Every B-Side test
# executable links bs_test_core and nothing of Vintage; every Vintage one links vs_test_core.
add_library(bs_test_core STATIC ${BS_PLUGIN_SOURCES})
target_include_directories(bs_test_core PUBLIC
    $<TARGET_PROPERTY:BSide,INCLUDE_DIRECTORIES>)
target_compile_definitions(bs_test_core PUBLIC
    $<TARGET_PROPERTY:BSide,COMPILE_DEFINITIONS>)
target_link_libraries(bs_test_core
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        BSideFactoryData
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
set_target_properties(bs_test_core PROPERTIES COMPILE_WARNING_AS_ERROR ON)

# BSideTests: the processor shell (parameters, state, presets, bypass, guard, arpeggiator through the
# test tone, real-time safety). The JUCE-free test executables (bside_unit: chip, program, trace) are
# added by S1c and S2 and link Catch2 only, not bs_test_core.
add_executable(BSideTests
    integration/ShellContractTests.cpp
    integration/ShellTests.cpp
    integration/TestTonePartTests.cpp)
target_link_libraries(BSideTests PRIVATE
    bs_test_core
    Catch2::Catch2WithMain)
set_target_properties(BSideTests PROPERTIES COMPILE_WARNING_AS_ERROR ON)
# The prefix keeps ctest names unique next to VintageSynthTests' (both are discovered into one list).
catch_discover_tests(BSideTests TEST_PREFIX "bside: ")
