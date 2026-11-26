# Golden-render regression: renders text scripts through the real processor and compares
# perceptual features with the references in golden/ (see RenderHarness.h).
#
#   -DUPDATE_GOLDENS=ON   makes the render tests rewrite golden/*.vsgolden instead of comparing.
#                         Only for an intended sound change; review the diff afterwards.
option(UPDATE_GOLDENS "Rewrite render-regression references instead of comparing" OFF)

# RenderHarness.cpp is instrument-independent (it renders through a processor factory);
# VintageInstance.cpp says which instrument this executable renders by default.
add_executable(render_regression
    RenderHarness.cpp
    VintageInstance.cpp
    RenderRegressionTests.cpp
    BitDump.cpp
    StateDump.cpp
    SpecParityTests.cpp)

target_include_directories(render_regression PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(render_regression PRIVATE
    VS_RR_SCRIPT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/scripts"
    VS_RR_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")
# bs_juce_free (tests/bside): SpecParityTests checks that B-Side's JUCE-free spec features measure like
# this harness.
target_link_libraries(render_regression PRIVATE
    vs_test_core
    bs_juce_free
    Catch2::Catch2WithMain)
set_target_properties(render_regression PROPERTIES COMPILE_WARNING_AS_ERROR ON)

# vs_discover_tests (tests/CMakeLists.txt) adds the tiers: label render stays, [slow] adds the label slow.
set(_rr_props)
if(UPDATE_GOLDENS)
    list(APPEND _rr_props ENVIRONMENT VS_UPDATE_GOLDENS=1)
endif()
vs_discover_tests(render_regression LABEL render PROPERTIES ${_rr_props})
