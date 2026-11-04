# ---------------------------------------------------------------------------
# Lane "masterplay" (internal plan masterplay.md): MasterPlay .IN parser / serializer, instruction catalog, library
# resolver, importer / exporter, as-run import, REST service, CLI and tests. Included by the lane glob in
# CMakeLists.txt; it adds nothing to PlayPlay or PlayPlayChannelEngine. PlayPlaySupervisor gets the lane's routes only
# when the option is ON (MP-12: playout/masterplay/mpsupervisor.cpp through playout/supervisor/laneext.h).
#
# Default ON only in the lane build trees (build-seg, build-lanes, build-mp). Every other tree, the live
# production build included, keeps it OFF until the lane is reviewed; -DPLAYPLAY_BUILD_MASTERPLAY=ON|OFF overrides.
# ---------------------------------------------------------------------------
get_filename_component(_pp_mp_bindir "${CMAKE_BINARY_DIR}" NAME)
if(_pp_mp_bindir MATCHES "^build-(seg|lanes|mp)$")
    set(_pp_mp_default ON)
else()
    set(_pp_mp_default OFF)
endif()
option(PLAYPLAY_BUILD_MASTERPLAY "MasterPlay .IN import/export lane (PlayPlayMasterplay library, pp-masterplay, tests)" ${_pp_mp_default})

if(PLAYPLAY_BUILD_MASTERPLAY)
    # The parser lives at playout/schedule/masterplay*.{h,cpp} (QtCore-only: AN-FS compiles it read-only later) but is
    # built here, not into PlayPlaySchedule, so CMakeLists.txt stays untouched. Everything else is playout/masterplay/.
    # Globbed so later slices only add files. A laneext::Registrar must not live in this static library (the linker
    # drops unreferenced objects): MP-12 adds its install .cpp to PlayPlaySupervisor with target_sources.
    set(_pp_mp_wiring "${CMAKE_SOURCE_DIR}/playout/masterplay/mpsupervisor.cpp")
    file(GLOB _pp_mp_sources CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/playout/schedule/masterplay*.cpp" "${CMAKE_SOURCE_DIR}/playout/schedule/masterplay*.h"
        "${CMAKE_SOURCE_DIR}/playout/masterplay/*.cpp"        "${CMAKE_SOURCE_DIR}/playout/masterplay/*.h")
    list(REMOVE_ITEM _pp_mp_sources "${_pp_mp_wiring}")
    add_library(PlayPlayMasterplay STATIC ${_pp_mp_sources})
    target_include_directories(PlayPlayMasterplay PUBLIC "${CMAKE_SOURCE_DIR}/playout/masterplay")
    target_link_libraries(PlayPlayMasterplay PUBLIC PlayPlaySchedule)

    # MP-12: the Supervisor wiring. mpsupervisor.cpp holds the laneext::Registrar that mounts /api/v1/masterplay on the
    # REST server (laneext::installAll in supervisorcore.cpp runs it); it is compiled into PlayPlaySupervisor itself, not
    # the static library, so the linker keeps the static registrar. Only when the option is ON: the live tree (OFF)
    # builds exactly today's Supervisor. PLAYPLAY_HAVE_MASTERPLAY lets other Supervisor code notice the lane.
    if(TARGET PlayPlaySupervisor AND EXISTS "${_pp_mp_wiring}")
        target_sources(PlayPlaySupervisor PRIVATE "${_pp_mp_wiring}")
        target_link_libraries(PlayPlaySupervisor PRIVATE PlayPlayMasterplay)
        target_compile_definitions(PlayPlaySupervisor PRIVATE PLAYPLAY_HAVE_MASTERPLAY=1)
    endif()

    # One Qt Test executable per tests/masterplay/test_*.cpp (named after the file), linked to the lane library;
    # MP_FIXTURES is the fixture folder (sample copies listed in fixtures/manifest.tsv plus synthetic grammar files).
    # test_mpsupervisor (MP-12) also compiles the wiring, the laneext registry, the REST server and the IngestManager it
    # hands to the service (the way test_blockingworker compiles the manager), so it links no Supervisor.
    if(PLAYPLAY_BUILD_TESTS AND COMMAND playplay_add_schedule_test)
        file(GLOB _pp_mp_tests CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tests/masterplay/test_*.cpp")
        foreach(_pp_mp_src IN LISTS _pp_mp_tests)
            get_filename_component(_pp_mp_test "${_pp_mp_src}" NAME_WE)
            playplay_add_schedule_test(${_pp_mp_test} "${_pp_mp_src}")
            target_link_libraries(${_pp_mp_test} PRIVATE PlayPlayMasterplay)
            target_compile_definitions(${_pp_mp_test} PRIVATE MP_FIXTURES="${CMAKE_SOURCE_DIR}/tests/masterplay/fixtures")
            if(_pp_mp_test STREQUAL "test_mpsupervisor")
                target_sources(${_pp_mp_test} PRIVATE "${_pp_mp_wiring}"
                    ${CMAKE_SOURCE_DIR}/playout/supervisor/laneext.cpp       ${CMAKE_SOURCE_DIR}/playout/supervisor/laneext.h
                    ${CMAKE_SOURCE_DIR}/playout/supervisor/restapiserver.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/restapiserver.h
                    ${CMAKE_SOURCE_DIR}/playout/supervisor/ingestmanager.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/ingestmanager.h
                    ${CMAKE_SOURCE_DIR}/playout/supervisor/blockingworker.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/blockingworker.h)
                target_link_libraries(${_pp_mp_test} PRIVATE Qt6::Network)
            endif()
        endforeach()
    endif()
endif()
