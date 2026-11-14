# B-Side JUCE-free code and its fast test executable (docs/architecture.md 8.1.2).
#
# bs_juce_free compiles the JUCE-free sources of source/bside/ (no JUCE module is built or linked;
# only the shared warning flags are taken from JUCE's flag targets, so GCC/Clang/MSVC see the same
# strictness as the plugin code). bside_unit links it with Catch2 only: its edit-build-test loop is
# a few seconds. The plugin's own source lists (BS_SOURCES) arrive with S1b and do not replace this.
set(BS_JUCEFREE_SOURCES
    ${PROJECT_SOURCE_DIR}/source/bside/text/TextScanner.cpp
    ${PROJECT_SOURCE_DIR}/source/bside/program/Program.cpp
    ${PROJECT_SOURCE_DIR}/source/bside/program/TableReader.cpp
    ${PROJECT_SOURCE_DIR}/source/bside/program/TableWriter.cpp
    ${PROJECT_SOURCE_DIR}/source/bside/trace/Sha256.cpp
    ${PROJECT_SOURCE_DIR}/source/bside/trace/TraceFormat.cpp
    ${PROJECT_SOURCE_DIR}/source/bside/trace/ReferenceFormat.cpp
    ${PROJECT_SOURCE_DIR}/source/bside/trace/Compare.cpp)

add_library(bs_juce_free STATIC ${BS_JUCEFREE_SOURCES})
target_include_directories(bs_juce_free PUBLIC ${PROJECT_SOURCE_DIR}/source)
target_link_libraries(bs_juce_free PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
set_target_properties(bs_juce_free PROPERTIES COMPILE_WARNING_AS_ERROR ON)

add_executable(bside_unit
    unit/TableSpecTests.cpp
    unit/TableBoundsTests.cpp
    unit/TableRoundTripTests.cpp
    unit/TextScannerTests.cpp
    unit/TraceFormatTests.cpp
    unit/ReferenceFormatTests.cpp
    unit/ReplayCompareTests.cpp
    unit/FuzzTests.cpp)
target_link_libraries(bside_unit PRIVATE bs_juce_free Catch2::Catch2WithMain)
set_target_properties(bside_unit PROPERTIES COMPILE_WARNING_AS_ERROR ON)

include(${PROJECT_SOURCE_DIR}/external/Catch2/extras/Catch.cmake)
catch_discover_tests(bside_unit PROPERTIES LABELS bside)
