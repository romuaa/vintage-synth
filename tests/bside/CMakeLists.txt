# B-Side JUCE-free code and its fast test executable (docs/architecture.md 8.1.2).
#
# bs_juce_free compiles the JUCE-free sources of source/bside/ (no JUCE module is built or linked;
# only the shared warning flags are taken from JUCE's flag targets, so GCC/Clang/MSVC see the same
# strictness as the plugin code). bside_unit links it with Catch2 only: its edit-build-test loop is
# a few seconds. The plugin's own source lists (BS_SOURCES, in the top-level CMakeLists.txt) are separate: bs_juce_free does not replace them.
set(BS_JUCEFREE_SOURCES
    ${PROJECT_SOURCE_DIR}/source/bside/text/TextScanner.cpp
    ${PROJECT_SOURCE_DIR}/source/bside/program/Program.cpp
    ${PROJECT_SOURCE_DIR}/source/bside/program/TableReader.cpp
    ${PROJECT_SOURCE_DIR}/source/bside/program/TableWriter.cpp
    ${PROJECT_SOURCE_DIR}/source/bside/trace/Sha256.cpp
    ${PROJECT_SOURCE_DIR}/source/bside/trace/TraceFormat.cpp
    ${PROJECT_SOURCE_DIR}/source/bside/trace/ReferenceFormat.cpp
    ${PROJECT_SOURCE_DIR}/source/bside/trace/Compare.cpp
    ${PROJECT_SOURCE_DIR}/source/bside/trace/SpecAnalysis.cpp
    ${PROJECT_SOURCE_DIR}/source/bside/trace/AnalogMeasure.cpp
    ${PROJECT_SOURCE_DIR}/source/bside/chip/Chip.cpp
    ${PROJECT_SOURCE_DIR}/source/bside/chip/ChipRender.cpp
    ${PROJECT_SOURCE_DIR}/source/bside/chip/RenderTables.cpp
    ${PROJECT_SOURCE_DIR}/source/bside/chip/CombinedWaveforms.cpp
    ${PROJECT_SOURCE_DIR}/source/bside/chip/CombinedWaveformParams.cpp
    ${PROJECT_SOURCE_DIR}/source/bside/chip/Filter.cpp
    ${PROJECT_SOURCE_DIR}/source/bside/chip/OutputStage.cpp
    ${PROJECT_SOURCE_DIR}/source/bside/chip/HalfBand.cpp
    ${PROJECT_SOURCE_DIR}/source/bside/chip/ChipVoice.cpp
    ${PROJECT_SOURCE_DIR}/source/bside/engine/ProgramEngine.cpp)

add_library(bs_juce_free STATIC ${BS_JUCEFREE_SOURCES})
target_include_directories(bs_juce_free PUBLIC ${PROJECT_SOURCE_DIR}/source)
target_link_libraries(bs_juce_free PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
set_target_properties(bs_juce_free PROPERTIES COMPILE_WARNING_AS_ERROR ON)
bs_chip_fp_rules(bs_juce_free) # the chip code's rounding rules (top-level CMakeLists.txt, the one place)

add_executable(bside_unit
    unit/TableSpecTests.cpp
    unit/TableBoundsTests.cpp
    unit/TableRoundTripTests.cpp
    unit/TextScannerTests.cpp
    unit/TraceFormatTests.cpp
    unit/ReferenceFormatTests.cpp
    unit/ReplayCompareTests.cpp
    unit/FormatLimitTests.cpp
    unit/HandoverTests.cpp
    unit/HandoverModelTests.cpp
    unit/FuzzTests.cpp
    unit/SpecAnalysisTests.cpp
    unit/AnalogMeasureTests.cpp
    unit/ChipRowTests.cpp
    unit/ChipEquivalenceTests.cpp
    unit/ChipReplayTests.cpp
    unit/ChipStepperTests.cpp
    unit/CombinedWaveformTests.cpp
    unit/ChipRestartTests.cpp
    unit/ProgramEngineTests.cpp
    unit/ProgramEngineGoldenTests.cpp
    unit/ProgramEngineChipTests.cpp
    unit/ProgramEnginePropertyTests.cpp
    unit/ChipRenderTests.cpp
    unit/ChipRenderKaiserTests.cpp
    unit/ChipRenderStreamTests.cpp
    unit/ChipRenderCostTests.cpp
    unit/ChipRenderDefectTests.cpp
    unit/ChipRenderExactTests.cpp
    unit/ChipBusTests.cpp
    unit/ChipVoiceTests.cpp
    unit/ChipVoiceAliasTests.cpp
    unit/FilterOutputTests.cpp unit/S3aCutoffLaw.cpp)
target_link_libraries(bside_unit PRIVATE bs_juce_free Catch2::Catch2WithMain)
target_compile_definitions(bside_unit PRIVATE BS_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")
set_target_properties(bside_unit PROPERTIES COMPILE_WARNING_AS_ERROR ON)

include(${PROJECT_SOURCE_DIR}/external/Catch2/extras/Catch.cmake)
catch_discover_tests(bside_unit PROPERTIES LABELS bside)

# Two static libraries below define the same symbols (the JUCE-free sources) with different defaults of
# chip::Config::shapes (BS_CHIP_DEFAULT_SHAPES): NEVER link bs_juce_free and bs_juce_free_placeholder into
# one executable. A static linker would silently take each object from whichever library it meets first,
# and the default shape set would then differ between translation units (an ODR violation).
#
# bside_independent: the independent per-cycle model (unit/IndependentModel.h, written from 8.15 alone and
# never edited by whoever edits source/bside/chip/) against the chip. The model has no interface for our
# fitted combined-waveform tables and still uses the S2a placeholder shapes, so this executable compiles
# the JUCE-free sources once more with the chip's default shape set switched to that placeholder
# (BS_CHIP_DEFAULT_SHAPES, ChipTypes.h; every source of the executable sees the same definition). The
# comparison then covers every rule of 8.15 except the table values themselves, which bside_trace tests
# against the reference. It goes away once the model takes a table (docs/decisions.md).
add_library(bs_juce_free_placeholder STATIC ${BS_JUCEFREE_SOURCES})
target_include_directories(bs_juce_free_placeholder PUBLIC ${PROJECT_SOURCE_DIR}/source)
target_compile_definitions(bs_juce_free_placeholder PUBLIC BS_CHIP_DEFAULT_SHAPES=placeholder)
target_link_libraries(bs_juce_free_placeholder PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
set_target_properties(bs_juce_free_placeholder PROPERTIES COMPILE_WARNING_AS_ERROR ON)
bs_chip_fp_rules(bs_juce_free_placeholder)
add_executable(bside_independent unit/IndependentModelTests.cpp)
target_link_libraries(bside_independent PRIVATE bs_juce_free_placeholder Catch2::Catch2WithMain)
set_target_properties(bside_independent PROPERTIES COMPILE_WARNING_AS_ERROR ON)
catch_discover_tests(bside_independent PROPERTIES LABELS bside)

# Golden write traces of the program engine: tests/bside/golden/*.bstrace (compared by ProgramEngineGoldenTests).
# tests/bside/golden/tools/model.py is the independent model that derives them and tools/gen.py regenerates them
# (python3 tests/bside/golden/tools/gen.py tests/bside/golden); the four hand-derived files are only checked, never
# overwritten. Each file's first line says which kind it is.
#
# bside_trace (ctest label trace): the committed reference data of the trace replay
# (tests/bside/reference/, generated by the reference-emulator adapter, docs/bside-oracle-adapter.md).
# Reads the committed files only and never needs the adapter. Tests that need a chip which does not
# exist yet report themselves skipped (Catch2 SKIP, ctest "Skipped"), never passed. One label only:
# catch_discover_tests passes PROPERTIES on unquoted, so a label list would be split.
add_executable(bside_trace trace/ReferenceDataTests.cpp trace/ShapeDataTests.cpp trace/RenderReferenceTests.cpp trace/AnalogReferenceTests.cpp
    trace/FilterDiagnosticTests.cpp)
target_link_libraries(bside_trace PRIVATE bs_juce_free Catch2::Catch2WithMain)
target_compile_definitions(bside_trace PRIVATE BS_REFERENCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/reference"
    BS_SHAPES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/shapes"
    BS_EXEMPTION_BUDGET="${CMAKE_CURRENT_SOURCE_DIR}/trace/exemption-budget.txt")
set_target_properties(bside_trace PROPERTIES COMPILE_WARNING_AS_ERROR ON)
catch_discover_tests(bside_trace PROPERTIES LABELS trace)

# chip_bench: CPU cost of Chip::render (docs/architecture.md 8.1.2; a benchmark, not a test, so no ctest
# entry and no sanitizer run): `build/tests/bside/chip_bench`.
add_executable(chip_bench benchmarks/ChipBench.cpp)
target_link_libraries(chip_bench PRIVATE bs_juce_free)
set_target_properties(chip_bench PROPERTIES COMPILE_WARNING_AS_ERROR ON)

# bside_shapefit: our combined-waveform fit (a developer tool, not a test: tests/bside/tools/ShapeFit.cpp;
# data and procedure in tests/bside/shapes/MANIFEST). Built with the tests so it never rots.
add_executable(bside_shapefit tools/ShapeFit.cpp)
target_link_libraries(bside_shapefit PRIVATE bs_juce_free)
set_target_properties(bside_shapefit PROPERTIES COMPILE_WARNING_AS_ERROR ON)

# ---- The JUCE-bound part: the plugin shell (only with VS_BUILD_BSIDE; the JUCE-free part above is always built) ----
if(VS_BUILD_BSIDE)
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
            vs_juce_modules # the compiled JUCE modules, shared with vs_test_core (tests/CMakeLists.txt)
            BSideFactoryData
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
    set_target_properties(bs_test_core PROPERTIES COMPILE_WARNING_AS_ERROR ON)
    bs_chip_fp_rules(bs_test_core)

    # BSideTests: the processor shell (parameters, state, presets, bypass, guard, arpeggiator through the
    # chip part, real-time safety) and the part itself (integration/PartTests.cpp, 8.21.1). The JUCE-free test executables (bside_unit: chip, program, trace) are
    # added by S1c and S2 and link Catch2 only, not bs_test_core.
    add_executable(BSideTests
        integration/ShellContractTests.cpp
        integration/ShellTests.cpp
        integration/PartTests.cpp
        integration/PartAliasTests.cpp
        integration/TablePresetTests.cpp
        integration/ParamTests.cpp)
    target_link_libraries(BSideTests PRIVATE
        bs_test_core
        Catch2::Catch2WithMain)
    set_target_properties(BSideTests PROPERTIES COMPILE_WARNING_AS_ERROR ON)
    # The prefix keeps ctest names unique next to VintageSynthTests' (both are discovered into one list).
    catch_discover_tests(BSideTests TEST_PREFIX "bside: ")

    # part_bench: the CPU gates of S4 (docs/architecture.md 8.21) through the part and the processor (a benchmark, not
    # a test: no ctest entry and no sanitizer run): `build/tests/bside/part_bench`.
    add_executable(part_bench benchmarks/PartBench.cpp)
    target_link_libraries(part_bench PRIVATE bs_test_core)
    set_target_properties(part_bench PROPERTIES COMPILE_WARNING_AS_ERROR ON)
endif()
