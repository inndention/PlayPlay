# ---------------------------------------------------------------------------
# Lane "nowplaying" (docs/nowplaying.md): the web "Now Playing" dashboard's Supervisor routes
# (GET /api/v1/nowplaying, GET /api/v1/nowplaying/{id}: one JSON bundle per second) and their Qt Test.
# Included by the lane glob in CMakeLists.txt; it adds nothing to PlayPlay or PlayPlayChannelEngine.
#
# playout/supervisor/nowplayingroutes.cpp holds a laneext::Registrar, so it is compiled into PlayPlaySupervisor itself
# (target_sources) and never into a static library (the linker would drop the unreferenced registrar).
# Default ON only in the lane build trees (build-seg, build-lanes); every other tree, the live production build
# included, keeps it OFF until reviewed; -DPLAYPLAY_BUILD_NOWPLAYING=ON|OFF overrides.
# ---------------------------------------------------------------------------
get_filename_component(_pp_np_bindir "${CMAKE_BINARY_DIR}" NAME)
if(_pp_np_bindir MATCHES "^build-(seg|lanes|np)$")
    set(_pp_np_default ON)
else()
    set(_pp_np_default OFF)
endif()
option(PLAYPLAY_BUILD_NOWPLAYING "Now Playing dashboard routes in PlayPlaySupervisor (+ test_nowplaying)" ${_pp_np_default})

if(PLAYPLAY_BUILD_NOWPLAYING)
    set(_pp_np_wiring "${CMAKE_SOURCE_DIR}/playout/supervisor/nowplayingroutes.cpp")
    if(TARGET PlayPlaySupervisor AND EXISTS "${_pp_np_wiring}")
        target_sources(PlayPlaySupervisor PRIVATE "${_pp_np_wiring}")
        target_compile_definitions(PlayPlaySupervisor PRIVATE PLAYPLAY_HAVE_NOWPLAYING=1)
    endif()

    # test_nowplaying drives the registrar the way SupervisorCore does (laneext::installAll on a Context) against a real
    # RestApiServer on an ephemeral loopback port that also serves stub upstream routes; it compiles the wiring, the laneext
    # registry and the REST server (the way test_mpsupervisor does) and links no Supervisor.
    set(_pp_np_test "${CMAKE_SOURCE_DIR}/tests/test_nowplaying.cpp")
    if(PLAYPLAY_BUILD_TESTS AND COMMAND playplay_add_schedule_test AND EXISTS "${_pp_np_test}")
        playplay_add_schedule_test(test_nowplaying "${_pp_np_test}" "${_pp_np_wiring}"
            ${CMAKE_SOURCE_DIR}/playout/supervisor/laneext.cpp       ${CMAKE_SOURCE_DIR}/playout/supervisor/laneext.h
            ${CMAKE_SOURCE_DIR}/playout/supervisor/restapiserver.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/restapiserver.h)
        target_link_libraries(test_nowplaying PRIVATE Qt6::Network)
    endif()
endif()
