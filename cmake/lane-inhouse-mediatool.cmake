# ---------------------------------------------------------------------------
# Lane "inhouse-media", DEP-03..06 (internal plan inhouse-deps.md §6.2, §9): PlayPlayMediaTool.exe, the in-house replacement
# for the ffprobe / ffmpeg spawns of the media path, on the FFmpeg 9 libraries the engines link.
#   PlayPlayMediaToolKit  static library: every tools/mediatool/*.cpp except the entry point and the Supervisor-side client
#                         (globbed with CONFIGURE_DEPENDS, so later slices only add files), linked by the exe and the tests.
#                         DEP-03: probecli (ffprobe's two forms byte for byte, through PlayPlayAvKit), thumbnailer (the
#                         ingest's ffmpeg thumbnail arguments), serve (the --serve JSON-lines ops on ppcli::ServeLoop).
#   PlayPlayMediaTool     the exe (probe, thumb, --serve, version, and the ffmpeg-style thumbnail form).
#   test_inhouse_{mediatool,graphrunner}  registered for whichever test source exists, linked against the kit, built
#                         after the exe (PP_MEDIATOOL_EXE = its path), with the reference FFmpeg bin dir and fixture root.
# tools/mediatool/mediatoolclient.{h,cpp} (DEP-04) are kept out of the kit: they belong to PlayPlaySupervisor, which DEP-04
# adds below with target_sources. Until then nothing is added to PlayPlay / PlayPlaySupervisor / PlayPlayChannelEngine,
# and every target here is EXCLUDE_FROM_ALL: a full build of any tree (the live one included) compiles nothing from it.
# ---------------------------------------------------------------------------
include("${CMAKE_CURRENT_LIST_DIR}/lane-inhouse-common.cmake")   # PlayPlayInhouseCommon, playplay_add_inhouse_test
include("${CMAKE_CURRENT_LIST_DIR}/lane-inhouse-avkit.cmake")    # PlayPlayAvKit (include_guard'ed there)
include_guard(GLOBAL)

file(GLOB _pp_mediatool_sources CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/tools/mediatool/*.cpp" "${CMAKE_SOURCE_DIR}/tools/mediatool/*.h")
list(FILTER _pp_mediatool_sources EXCLUDE REGEX "/tools/mediatool/(mediatool_main\\.cpp|mediatoolclient\\.(cpp|h))$")
add_library(PlayPlayMediaToolKit STATIC EXCLUDE_FROM_ALL ${_pp_mediatool_sources})
target_include_directories(PlayPlayMediaToolKit PUBLIC "${CMAKE_SOURCE_DIR}/tools/mediatool")
target_link_libraries(PlayPlayMediaToolKit PUBLIC Qt6::Core FFmpeg::FFmpeg PlayPlayAvKit PlayPlayInhouseCommon)
set_target_properties(PlayPlayMediaToolKit PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

add_executable(PlayPlayMediaTool EXCLUDE_FROM_ALL "${CMAKE_SOURCE_DIR}/tools/mediatool/mediatool_main.cpp")
target_link_libraries(PlayPlayMediaTool PRIVATE PlayPlayMediaToolKit)
set_target_properties(PlayPlayMediaTool PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

file(GLOB _pp_mediatool_tests CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tests/inhouse/test_inhouse_*.cpp")
list(FILTER _pp_mediatool_tests INCLUDE REGEX "/test_inhouse_(mediatool|graphrunner)\\.cpp$")
foreach(_src IN LISTS _pp_mediatool_tests)
    get_filename_component(_name "${_src}" NAME_WE)
    playplay_add_inhouse_test(${_name} SOURCES "${_src}" LIBS PlayPlayMediaToolKit)
    if(TARGET ${_name})
        add_dependencies(${_name} PlayPlayMediaTool)
        target_compile_definitions(${_name} PRIVATE
            PP_MEDIATOOL_EXE="$<TARGET_FILE:PlayPlayMediaTool>"
            PP_INHOUSE_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/inhouse/data"
            PP_FFMPEG_BIN="${PLAYPLAY_FFMPEG_ROOT}/bin")
    endif()
endforeach()
# DEP-05a: the graph-runner test enumerates the Supervisor's own ffmpeg argument builders (suites::presetArgs,
# headline::storiesFilter / sceneTimings), so it compiles those two sources the way tests/CMakeLists.txt does for
# test_suites / test_headlinenews (read-only use; nothing of the Supervisor target changes). suitelogic.cpp needs QtNetwork.
if(TARGET test_inhouse_graphrunner)
    target_sources(test_inhouse_graphrunner PRIVATE
        "${CMAKE_SOURCE_DIR}/playout/supervisor/suitelogic.cpp" "${CMAKE_SOURCE_DIR}/playout/supervisor/headlinenews.cpp")
    target_include_directories(test_inhouse_graphrunner PRIVATE "${CMAKE_SOURCE_DIR}/playout/supervisor")
    target_link_libraries(test_inhouse_graphrunner PRIVATE Qt6::Network)
endif()
