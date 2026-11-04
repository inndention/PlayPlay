# ---------------------------------------------------------------------------
# Lane "inhouse-media", DEP-00 (internal plan inhouse-deps.md §5, §9): helpers every in-house tool links.
#   PlayPlayInhouseCommon  static library, QtCore only: ppcli (last-line JSON result, PROGRESS lines, exit codes 0/2/64,
#                          argparse-style options, the --serve JSON-lines loop with per-request timeouts) and wavio
#                          (PCM WAV I/O plus the trim / voiced-RMS normalize / 2 ms limiter of scripts/spot_tts.py).
#   playplay_add_inhouse_test(<name> SOURCES ... [LIBS ...])  Qt Test helper for tests/inhouse/test_inhouse_*.cpp.
# The other lane-inhouse-*.cmake fragments sort before and after this one, so each starts with
#   include("${CMAKE_CURRENT_LIST_DIR}/lane-inhouse-common.cmake")
# and the guard below makes the second include a no-op.
# Nothing here is added to PlayPlay / PlayPlaySupervisor / PlayPlayChannelEngine, and every target is EXCLUDE_FROM_ALL:
# a full build of any tree (the live one included) never compiles this lane; build the targets by name.
# ---------------------------------------------------------------------------
include_guard(GLOBAL)

file(GLOB _pp_inhouse_common_sources CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/tools/ppcommon/*.cpp" "${CMAKE_SOURCE_DIR}/tools/ppcommon/*.h")
add_library(PlayPlayInhouseCommon STATIC EXCLUDE_FROM_ALL ${_pp_inhouse_common_sources})
target_include_directories(PlayPlayInhouseCommon PUBLIC "${CMAKE_SOURCE_DIR}/tools/ppcommon")
target_link_libraries(PlayPlayInhouseCommon PUBLIC Qt6::Core)
set_target_properties(PlayPlayInhouseCommon PROPERTIES AUTOUIC OFF AUTORCC OFF)

# One Qt Test executable per call; PP_SOURCE_DIR lets a test find scripts/ (Python parity checks) and its fixtures.
function(playplay_add_inhouse_test name)
    if(NOT PLAYPLAY_BUILD_TESTS)
        return()
    endif()
    cmake_parse_arguments(_pp "" "" "SOURCES;LIBS" ${ARGN})
    add_executable(${name} EXCLUDE_FROM_ALL ${_pp_SOURCES})
    target_link_libraries(${name} PRIVATE ${_pp_LIBS} Qt6::Test)
    target_compile_definitions(${name} PRIVATE PP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 120 ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1")
endfunction()

playplay_add_inhouse_test(test_inhouse_common
    SOURCES "${CMAKE_SOURCE_DIR}/tests/inhouse/test_inhouse_common.cpp"
    LIBS PlayPlayInhouseCommon)
