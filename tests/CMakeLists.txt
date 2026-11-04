# Unit tests (Qt Test). Each test is its own executable so a crash in one
# does not hide the others, and ctest can run them in parallel.

function(playplay_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE PlayPlayEngine PlayPlaySchedule Qt6::Test)
    add_test(NAME ${name} COMMAND ${name})
    # Qt on Windows sends logging to the debugger when stdout is redirected;
    # force stderr so ctest --output-on-failure actually shows the test log.
    set_tests_properties(${name} PROPERTIES TIMEOUT 60
        ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1")
endfunction()

# Schedule-only tests link just the schedule library (no engine), so they build and run in seconds.
function(playplay_add_schedule_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE PlayPlaySchedule Qt6::Test)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 60 ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1")
endfunction()

playplay_add_test(test_frameclock   test_frameclock.cpp)
playplay_add_test(test_ipcprotocol  test_ipcprotocol.cpp)
playplay_add_test(test_framerate    test_framerate.cpp)
playplay_add_test(test_pipeline     test_pipeline.cpp)
playplay_add_test(bench_pipeline    bench_pipeline.cpp)          # numbers only; runs with PLAYPLAY_BENCH=1
playplay_add_test(test_schedule     test_schedule.cpp)
playplay_add_schedule_test(test_programming  test_programming.cpp)
playplay_add_schedule_test(test_suites  test_suites.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/suitelogic.cpp)
target_link_libraries(test_suites PRIVATE Qt6::Network)
playplay_add_test(test_compositor   test_compositor.cpp)
playplay_add_test(test_scenerecorder test_scenerecorder.cpp)
playplay_add_test(test_cg_captions  test_cg_captions.cpp)
playplay_add_test(test_browserhost  test_browserhost.cpp)
playplay_add_test(test_mediapreview test_mediapreview.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/mediapreview.cpp)
playplay_add_schedule_test(test_cgrules test_cgrules.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/overlaymanager.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/overlaymanager.h
    ${CMAKE_SOURCE_DIR}/playout/supervisor/pillarpanels.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/pillarpanels.h ${CMAKE_SOURCE_DIR}/playout/supervisor/scte35.cpp)
playplay_add_schedule_test(test_cgdesigner test_cgdesigner.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/cgdesign.cpp)
playplay_add_schedule_test(test_transcripttools test_transcripttools.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/transcripttools.cpp)
playplay_add_schedule_test(test_headlinenews test_headlinenews.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/headlinenews.cpp)
playplay_add_schedule_test(test_localspots test_localspots.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/localspots.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/headlinenews.cpp)
playplay_add_schedule_test(test_marketspots test_marketspots.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/marketspots.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/localspots.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/headlinenews.cpp)
playplay_add_schedule_test(test_weatherspots test_weatherspots.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/weatherspots.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/marketspots.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/localspots.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/headlinenews.cpp)
playplay_add_schedule_test(test_areamarket test_areamarket.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/areamarket.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/localspots.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/headlinenews.cpp)
playplay_add_schedule_test(test_flightplan test_flightplan.cpp)
