# ---------------------------------------------------------------------------
# Lane "studio-engine": PlayPlayStudio.exe (tools/studio), the ported StarMedia publishing-harness engine, and its Qt
# tests (tests/studio). A separate process from the on-air Supervisor (internal plan starmedia-studio.md).
#
# Later slices and the studio-film lane add code without editing this file (all globs are CONFIGURE_DEPENDS, so a new
# file re-runs CMake on the next build):
#   - every tools/studio/src/**/*.cpp|*.h goes into PlayPlayStudioLib, except app/main.cpp (the exe entry point);
#   - every tests/studio/test_studio_<name>.cpp becomes the test executable and ctest test test_studio_<name>
#     (studio_add_test below: PlayPlayStudioLib + Qt6::Test, never the engine);
#   - every other tests/studio/*.cpp|*.h (mock OpenRouter, shared fixtures) goes into StudioTestSupport, which every
#     studio test links (created once at least one such .cpp exists).
# Tests get STUDIO_EXE_PATH (PlayPlayStudio.exe, built first), STUDIO_SOURCE_DIR (tools/studio, for data/*.json) and
# STUDIO_TEST_DIR (tests/studio, for fixtures/).
#
# Safety: nothing here touches PlayPlay / PlayPlaySupervisor / PlayPlayChannelEngine. Every target is EXCLUDE_FROM_ALL
# (built only by name). PLAYPLAY_BUILD_STUDIO defaults ON only in the lane trees (build-seg, build-lanes, build-studio;
# same rule as lane-masterplay.cmake) and OFF everywhere else, the live <source>/build included: the tests below are
# registered with add_test but never built by "all", so in any other tree with PLAYPLAY_BUILD_TESTS a plain ctest would
# report them "Not Run" and fail (scripts/build.ps1 -Config Debug|RelWithDebInfo runs exactly that). Pass
# -DPLAYPLAY_BUILD_STUDIO=ON|OFF to override anywhere.
# ---------------------------------------------------------------------------
get_filename_component(_studio_bindir "${CMAKE_BINARY_DIR}" NAME)
string(TOLOWER "${_studio_bindir}" _studio_bindir)
if(_studio_bindir MATCHES "^build-(seg|lanes|studio)$")
    set(_studio_default ON)
else()
    set(_studio_default OFF)
endif()
option(PLAYPLAY_BUILD_STUDIO "Configure PlayPlayStudio.exe (tools/studio) and its tests (targets build only by name)" ${_studio_default})
if(NOT PLAYPLAY_BUILD_STUDIO)
    return()
endif()

set(PLAYPLAY_STUDIO_DIR "${CMAKE_SOURCE_DIR}/tools/studio")

# The Studio embeds tools/studio/data/*.json (resources.qrc). Those files are generated from the StarMedia sources by
# tools/studio/scripts/export-starmedia-data.mjs and are not part of the public tree, so without them the lane is skipped
# (configure still succeeds; PlayPlayStudio and test_studio_* simply do not exist).
foreach(_studio_data schema model-limits model-prompts realpeople styles)
    if(NOT EXISTS "${PLAYPLAY_STUDIO_DIR}/data/${_studio_data}.json")
        message(STATUS "PlayPlayStudio lane skipped: tools/studio/data/${_studio_data}.json is missing (not part of this tree)")
        return()
    endif()
endforeach()
set(PLAYPLAY_STUDIO_TEST_DIR "${CMAKE_SOURCE_DIR}/tests/studio")
set(PLAYPLAY_STUDIO_VERSION "${PROJECT_VERSION}")

file(GLOB_RECURSE _studio_src CONFIGURE_DEPENDS "${PLAYPLAY_STUDIO_DIR}/src/*.cpp" "${PLAYPLAY_STUDIO_DIR}/src/*.h")
list(REMOVE_ITEM _studio_src "${PLAYPLAY_STUDIO_DIR}/src/app/main.cpp")

# The engine as a library so tests link it without main(). Qt SQL (store), WebSockets (WS hub), Concurrent (worker
# pool), Gui (QImage stills) and FFmpeg (media utils) are linked here once so no later slice edits this fragment.
add_library(PlayPlayStudioLib STATIC EXCLUDE_FROM_ALL ${_studio_src} "${PLAYPLAY_STUDIO_DIR}/resources.qrc")
target_include_directories(PlayPlayStudioLib PUBLIC "${PLAYPLAY_STUDIO_DIR}/src")
target_compile_definitions(PlayPlayStudioLib PUBLIC PLAYPLAY_STUDIO_VERSION="${PLAYPLAY_STUDIO_VERSION}")
target_link_libraries(PlayPlayStudioLib PUBLIC
    Qt6::Core Qt6::Network Qt6::Sql Qt6::Concurrent Qt6::WebSockets Qt6::Gui FFmpeg::FFmpeg)

add_executable(PlayPlayStudio EXCLUDE_FROM_ALL "${PLAYPLAY_STUDIO_DIR}/src/app/main.cpp")
target_link_libraries(PlayPlayStudio PRIVATE PlayPlayStudioLib)

if(PLAYPLAY_BUILD_TESTS)
    file(GLOB _studio_tests CONFIGURE_DEPENDS "${PLAYPLAY_STUDIO_TEST_DIR}/test_studio_*.cpp")
    file(GLOB _studio_support CONFIGURE_DEPENDS "${PLAYPLAY_STUDIO_TEST_DIR}/*.cpp" "${PLAYPLAY_STUDIO_TEST_DIR}/*.h")
    if(_studio_tests)
        list(REMOVE_ITEM _studio_support ${_studio_tests})
    endif()
    # Only a .cpp makes a library: a STATIC target of headers alone has no link language and would fail configure in
    # every lane tree. Header-only helpers are reachable anyway (each test has tests/studio on its include path).
    set(_studio_support_cpp ${_studio_support})
    list(FILTER _studio_support_cpp INCLUDE REGEX "\\.cpp$")
    if(_studio_support_cpp)
        add_library(StudioTestSupport STATIC EXCLUDE_FROM_ALL ${_studio_support})
        target_include_directories(StudioTestSupport PUBLIC "${PLAYPLAY_STUDIO_TEST_DIR}")
        target_link_libraries(StudioTestSupport PUBLIC PlayPlayStudioLib Qt6::Test)
        # the helpers find tests/studio/fixtures (the mock OpenRouter's catalog) and tools/studio/data on their own
        target_compile_definitions(StudioTestSupport PRIVATE
            STUDIO_SOURCE_DIR="${PLAYPLAY_STUDIO_DIR}"
            STUDIO_TEST_DIR="${PLAYPLAY_STUDIO_TEST_DIR}")
    endif()

    function(studio_add_test name)
        add_executable(${name} EXCLUDE_FROM_ALL ${ARGN})
        target_link_libraries(${name} PRIVATE PlayPlayStudioLib Qt6::Test)
        if(TARGET StudioTestSupport)
            target_link_libraries(${name} PRIVATE StudioTestSupport)
        endif()
        target_include_directories(${name} PRIVATE "${PLAYPLAY_STUDIO_TEST_DIR}")
        target_compile_definitions(${name} PRIVATE
            STUDIO_EXE_PATH="$<TARGET_FILE:PlayPlayStudio>"
            STUDIO_SOURCE_DIR="${PLAYPLAY_STUDIO_DIR}"
            STUDIO_TEST_DIR="${PLAYPLAY_STUDIO_TEST_DIR}")
        add_dependencies(${name} PlayPlayStudio)
        add_test(NAME ${name} COMMAND ${name})
        # Same as playplay_add_test: Qt on Windows logs to the debugger when stdout is redirected.
        set_tests_properties(${name} PROPERTIES TIMEOUT 120 LABELS studio ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1")
    endfunction()

    foreach(_t IN LISTS _studio_tests)
        get_filename_component(_n "${_t}" NAME_WE)
        studio_add_test(${_n} "${_t}")
    endforeach()
endif()
