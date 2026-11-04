# ---------------------------------------------------------------------------
# Lane "inhouse-media", DEP-07..09 (internal plan inhouse-deps.md §6.3, §9): PlayPlaySpotRender.exe, the in-house replacement
# for the spot generator's Python helpers (spot_music.py now; spot_card.py / weather_cards.py with DEP-08/09).
#   PlayPlaySpotRenderKit  static library: every tools/spotrender/*.cpp except the entry point (globbed with
#                          CONFIGURE_DEPENDS, so later slices only add files), linked by the exe and by the tests.
#                          DEP-07: pyrandom (bit-exact CPython random.Random), musicsynth (composer, MIDI writer, synth).
#                          Qt6::Gui / FFmpeg::FFmpeg are linked for the card renderer and its libav image fallback (DEP-08);
#                          the MSVC linker imports a DLL only once a symbol from it is used, so `music` needs neither.
#   PlayPlaySpotRender     the exe (sub-commands `music`, `version`).
#   test_inhouse_{music,card,weather}  registered here for whichever of the three test sources exist, each linked
#                          against the kit, built after the exe (PP_SPOTRENDER_EXE = its path) and given the fixture root.
# EXCLUDE_FROM_ALL; nothing is added to PlayPlay / PlayPlaySupervisor / PlayPlayChannelEngine (DEP-13 wires the switch).
# ---------------------------------------------------------------------------
include("${CMAKE_CURRENT_LIST_DIR}/lane-inhouse-common.cmake")   # PlayPlayInhouseCommon, playplay_add_inhouse_test
include_guard(GLOBAL)

file(GLOB _pp_spotrender_sources CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/tools/spotrender/*.cpp" "${CMAKE_SOURCE_DIR}/tools/spotrender/*.h")
list(FILTER _pp_spotrender_sources EXCLUDE REGEX "/tools/spotrender/spotrender_main\\.cpp$")
add_library(PlayPlaySpotRenderKit STATIC EXCLUDE_FROM_ALL ${_pp_spotrender_sources})
target_include_directories(PlayPlaySpotRenderKit PUBLIC "${CMAKE_SOURCE_DIR}/tools/spotrender")
target_link_libraries(PlayPlaySpotRenderKit PUBLIC Qt6::Core Qt6::Gui FFmpeg::FFmpeg PlayPlayInhouseCommon)
set_target_properties(PlayPlaySpotRenderKit PROPERTIES AUTOUIC OFF AUTORCC OFF)

add_executable(PlayPlaySpotRender EXCLUDE_FROM_ALL "${CMAKE_SOURCE_DIR}/tools/spotrender/spotrender_main.cpp")
target_link_libraries(PlayPlaySpotRender PRIVATE PlayPlaySpotRenderKit)
set_target_properties(PlayPlaySpotRender PROPERTIES AUTOUIC OFF AUTORCC OFF)

file(GLOB _pp_spotrender_tests CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tests/inhouse/test_inhouse_*.cpp")
list(FILTER _pp_spotrender_tests INCLUDE REGEX "/test_inhouse_(music|card|weather)\\.cpp$")
# test_inhouse_card and test_inhouse_music compare against fixtures and goldens under tests/inhouse/data. That folder is not
# part of the public tree (generated fixtures), so those two tests are not registered without it.
if(NOT IS_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/inhouse/data")
    message(STATUS "tests/inhouse/data is missing: test_inhouse_card and test_inhouse_music are not registered")
    list(FILTER _pp_spotrender_tests EXCLUDE REGEX "/test_inhouse_(music|card)\\.cpp$")
endif()
foreach(_src IN LISTS _pp_spotrender_tests)
    get_filename_component(_name "${_src}" NAME_WE)
    playplay_add_inhouse_test(${_name} SOURCES "${_src}" LIBS PlayPlaySpotRenderKit)
    if(TARGET ${_name})
        add_dependencies(${_name} PlayPlaySpotRender)
        target_compile_definitions(${_name} PRIVATE
            PP_SPOTRENDER_EXE="$<TARGET_FILE:PlayPlaySpotRender>"
            PP_INHOUSE_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/inhouse/data"
            PP_FFMPEG_BIN="${PLAYPLAY_FFMPEG_ROOT}/bin")
    endif()
endforeach()
