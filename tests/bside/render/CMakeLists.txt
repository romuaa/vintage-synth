# B-Side render regression (docs/architecture.md 8.21.2): six factory programs rendered through the real processor and the
# shared, instrument-independent harness (tests/render_regression/RenderHarness.cpp) with B-Side's instance file; their
# perceptual features are compared with the references in golden/.
#
#   -DUPDATE_GOLDENS=ON   the option Vintage's render_regression uses (declared in tests/render_regression/CMakeLists.txt):
#                         rewrites golden/*.vsgolden instead of comparing.
#
# Links bs_test_core and nothing of Vintage (the two test cores must never meet in one executable).
if(NOT DEFINED UPDATE_GOLDENS)
    option(UPDATE_GOLDENS "Rewrite render-regression references instead of comparing" OFF)
endif()

add_executable(bside_render
    ${PROJECT_SOURCE_DIR}/tests/render_regression/RenderHarness.cpp
    BSideInstance.cpp
    BSideRenderTests.cpp)
target_include_directories(bside_render PRIVATE ${PROJECT_SOURCE_DIR}/tests/render_regression)
target_compile_definitions(bside_render PRIVATE
    BS_RR_SCRIPT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/scripts"
    BS_RR_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")
target_link_libraries(bside_render PRIVATE bs_test_core Catch2::Catch2WithMain)
set_target_properties(bside_render PROPERTIES COMPILE_WARNING_AS_ERROR ON)

# vs_discover_tests (tests/CMakeLists.txt) adds the tiers: label render stays, [slow] adds the label slow.
set(_bsrr_props)
if(UPDATE_GOLDENS)
    list(APPEND _bsrr_props ENVIRONMENT VS_UPDATE_GOLDENS=1)
endif()
vs_discover_tests(bside_render LABEL render TEST_PREFIX "bside_render: " PROPERTIES ${_bsrr_props})
