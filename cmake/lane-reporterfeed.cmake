# ---------------------------------------------------------------------------
# Lane "reporterfeed" (docs/reporter-feed.md): the getstarnews app's reporter goes live into PlayPlay. The Supervisor gets
# playout/supervisor/reporterfeed.cpp (REST routes under /api/v1/reporter and the OBS-era /api/field contract, an ffmpeg.exe
# RTMP listener per session, the go-live countdown) through laneext; nothing is added to PlayPlay or the engine.
#
# Default ON only in the lane build trees (build-seg, build-lanes); every other tree, the live production build included,
# keeps it OFF (-DPLAYPLAY_BUILD_REPORTERFEED=ON|OFF overrides). PLAYPLAY_HAVE_REPORTERFEED guards the two lines in
# supervisorcore_integrations.cpp that add `reporter` to GET /api/v1/inputs.
# ---------------------------------------------------------------------------
get_filename_component(_pp_rf_bindir "${CMAKE_BINARY_DIR}" NAME)
if(_pp_rf_bindir MATCHES "^build-(seg|lanes)$")
    set(_pp_rf_default ON)
else()
    set(_pp_rf_default OFF)
endif()
option(PLAYPLAY_BUILD_REPORTERFEED "Reporter feed lane (RTMP ingest from the getstarnews app, go-live countdown)" ${_pp_rf_default})

if(PLAYPLAY_BUILD_REPORTERFEED)
    set(_pp_rf_src "${CMAKE_SOURCE_DIR}/playout/supervisor/reporterfeed.cpp" "${CMAKE_SOURCE_DIR}/playout/supervisor/reporterfeed.h")
    if(TARGET PlayPlaySupervisor)
        target_sources(PlayPlaySupervisor PRIVATE ${_pp_rf_src})
        target_compile_definitions(PlayPlaySupervisor PRIVATE PLAYPLAY_HAVE_REPORTERFEED=1)
    endif()

    # Unit test: the service with stub hooks (no process, no network), plus the REST server and registry it compiles against.
    if(PLAYPLAY_BUILD_TESTS AND COMMAND playplay_add_schedule_test)
        playplay_add_schedule_test(test_reporterfeed "${CMAKE_SOURCE_DIR}/tests/test_reporterfeed.cpp" ${_pp_rf_src}
            ${CMAKE_SOURCE_DIR}/playout/supervisor/laneext.cpp       ${CMAKE_SOURCE_DIR}/playout/supervisor/laneext.h
            ${CMAKE_SOURCE_DIR}/playout/supervisor/restapiserver.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/restapiserver.h)
        target_link_libraries(test_reporterfeed PRIVATE Qt6::Network)
        target_include_directories(test_reporterfeed PRIVATE "${CMAKE_SOURCE_DIR}")
    endif()
endif()
