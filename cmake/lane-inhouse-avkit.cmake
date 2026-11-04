# ---------------------------------------------------------------------------
# Lane "inhouse-media", DEP-01 (internal plan inhouse-deps.md §6.1, §9): PlayPlayAvKit, in-process libav capabilities and
# probe for the Supervisor, the app and the in-house tools.
#   PlayPlayAvKit   static library (Qt6::Core, FFmpeg::FFmpeg): avcaps (`ffmpeg -encoders` / `-version` without the
#                   spawn: encoder list, configuration, versions, MuxManager's capabilities "ffmpeg" object) and avprobe
#                   (ffprobe -show_format -show_streams JSON, same keys/order/layout, plus a header-only duration probe).
#   test_inhouse_avkit  compares both with ffmpeg.exe / ffprobe.exe of the same FFmpeg build.
# EXCLUDE_FROM_ALL; consumers link it with target_link_libraries(<target> PRIVATE PlayPlayAvKit) (a target in ALL that
# links an EXCLUDE_FROM_ALL library still builds it). DEP-02a (below) links it into the app's MetadataService; the
# inhouse-mux lane's DEP-02b links it into PlayPlaySupervisor for MuxManager::capabilities from its own fragment.
# Later slices add files under tools/avkit/ without editing this fragment (CONFIGURE_DEPENDS glob).
# ---------------------------------------------------------------------------
include("${CMAKE_CURRENT_LIST_DIR}/lane-inhouse-common.cmake")   # playplay_add_inhouse_test (include_guard'ed there)
include_guard(GLOBAL)

file(GLOB _pp_avkit_sources CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/tools/avkit/*.cpp" "${CMAKE_SOURCE_DIR}/tools/avkit/*.h")
add_library(PlayPlayAvKit STATIC EXCLUDE_FROM_ALL ${_pp_avkit_sources})
target_include_directories(PlayPlayAvKit PUBLIC "${CMAKE_SOURCE_DIR}/tools/avkit")
target_link_libraries(PlayPlayAvKit PUBLIC Qt6::Core FFmpeg::FFmpeg)
set_target_properties(PlayPlayAvKit PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

playplay_add_inhouse_test(test_inhouse_avkit
    SOURCES "${CMAKE_SOURCE_DIR}/tests/inhouse/test_inhouse_avkit.cpp"
    LIBS PlayPlayAvKit)
if(TARGET test_inhouse_avkit)
    # reference ffmpeg.exe / ffprobe.exe of the build being linked (also staged next to the test); never the PATH ones
    target_compile_definitions(test_inhouse_avkit PRIVATE PP_FFMPEG_BIN="${PLAYPLAY_FFMPEG_ROOT}/bin")
endif()

# ---------------------------------------------------------------------------
# DEP-02a: the app's MetadataService (services/metadataservice.cpp, compiled into the PlayPlayCore static library) probes
# through AvKit instead of spawning whatever `ffprobe` Windows finds (PLAYPLAY_PROBE_ENGINE=ffprobe keeps that fallback).
# Link-only on purpose: PlayPlayCore's compile flags stay as they are (the source includes "tools/avkit/avprobe.h"
# through the source-root include path), and every target that pulls metadataservice.obj out of the library (today the
# app: main.cpp and the bulk importer) gets PlayPlayAvKit plus its libav import libraries. Targets that never reference
# MetadataService (Supervisor, engines, tests) take nothing from it.
# ---------------------------------------------------------------------------
target_link_libraries(PlayPlayCore INTERFACE "$<LINK_ONLY:PlayPlayAvKit>")

# test_inhouse_avkit_app: the same test source with PP_AVKIT_APP, built against the real PlayPlayCore (so through the
# link above) plus the app's bulk importer widget, run offscreen with every ffprobe removed from PATH.
playplay_add_inhouse_test(test_inhouse_avkit_app
    SOURCES "${CMAKE_SOURCE_DIR}/tests/inhouse/test_inhouse_avkit.cpp"
            "${CMAKE_SOURCE_DIR}/Play_Play_Native/widgets/bulkmediaimporter.cpp"
            "${CMAKE_SOURCE_DIR}/Play_Play_Native/widgets/bulkmediaimporter.h"
            "${CMAKE_SOURCE_DIR}/database/mediadb.cpp" "${CMAKE_SOURCE_DIR}/database/mediadb.h"
    LIBS PlayPlayCore Qt6::Widgets)
if(TARGET test_inhouse_avkit_app)
    target_compile_definitions(test_inhouse_avkit_app PRIVATE PP_AVKIT_APP PP_FFMPEG_BIN="${PLAYPLAY_FFMPEG_ROOT}/bin")
endif()
