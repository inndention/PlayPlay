# ---------------------------------------------------------------------------
# Lane "perf-app" (internal plan perf-supervisor-app.md W9-W11, W12 client side, W18; slices PA-1..PA-5 in
# internal plan lane-plan.json): keep the operator app's GUI thread from blocking on IPC, JPEG decode or per-frame model
# conversion, and show a banner when the Supervisor is hung. Included by the lane glob in CMakeLists.txt.
#
# UNCONDITIONAL on purpose: playout/supervisorclient.cpp (compiled into PlayPlay by CMakeLists.txt) uses
# SupervisorClientWorker (PA-1), so every tree, the live production build included, must compile the lane's app
# sources into PlayPlay or the app fails to link. They are globbed by name, so later slices (channellistmodel PA-3,
# meterticker PA-5) only add files.
# ---------------------------------------------------------------------------
file(GLOB _pp_pa_app CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/playout/*.cpp" "${CMAKE_SOURCE_DIR}/playout/*.h")
list(FILTER _pp_pa_app INCLUDE REGEX "/playout/(supervisorclientworker|channellistmodel|meterticker)\\.(cpp|h)$")
target_sources(PlayPlay PRIVATE ${_pp_pa_app})

# Tests: the app is an executable, so each test compiles the app-side sources it exercises and links PlayPlayCore
# (through the schedule library) plus the Qt modules those sources need. Each block checks its source exists.
if(PLAYPLAY_BUILD_TESTS AND COMMAND playplay_add_schedule_test)
    set(_pp_pa_client_srcs
        ${CMAKE_SOURCE_DIR}/playout/supervisorclient.cpp ${CMAKE_SOURCE_DIR}/playout/supervisorclient.h
        ${CMAKE_SOURCE_DIR}/playout/supervisorapi.cpp    ${CMAKE_SOURCE_DIR}/playout/supervisorapi.h
        ${_pp_pa_app})
    # PA-1 (W9) test_supervisorclient: worker-thread IPC, async connect, stall after 3 s of silence, GUI loop lag at
    # 100 msg/s, coalescing while the GUI is busy, busy-pipe stall, paused api.* GETs. Fake QLocalServer / HTTP only.
    # PA-4 (W12 client side) adds: no periodic status query, runlists by broadcast, the stale-snapshot query, the
    # rollback knob, and 10 simulated minutes of an idle schedule (2 x 15 s) measured by the client's byte counter.
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_supervisorclient.cpp")
        playplay_add_schedule_test(test_supervisorclient ${CMAKE_SOURCE_DIR}/tests/test_supervisorclient.cpp ${_pp_pa_client_srcs})
        target_link_libraries(test_supervisorclient PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Network)
        set_tests_properties(test_supervisorclient PROPERTIES TIMEOUT 150)
        if(WIN32)
            target_link_libraries(test_supervisorclient PRIVATE winmm)
        endif()
    endif()
    # PA-3 (W10) test_channellistmodel: rows updated in place (identical snapshot = no signal, one changed field = one
    # dataChanged), QML delegates kept across snapshots, metersChanged <= 15 Hz, active* signals only for the active channel.
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_channellistmodel.cpp")
        playplay_add_schedule_test(test_channellistmodel ${CMAKE_SOURCE_DIR}/tests/test_channellistmodel.cpp ${_pp_pa_client_srcs})
        target_link_libraries(test_channellistmodel PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Network)
        if(WIN32)
            target_link_libraries(test_channellistmodel PRIVATE winmm)
        endif()
    endif()
    # PA-2 (W8 client side + W11) test_previewdecode: a 1080p JPEG decoded to a 320x180 tile with QImageReader::setScaledSize
    # is >= 4x faster than the old full decode + smooth scale and has the right size; binary (PS-5) and JSON/base64
    # preview frames both reach the client through a real IpcTransport, byte counter exact for both; the rollback knob.
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_previewdecode.cpp")
        playplay_add_schedule_test(test_previewdecode ${CMAKE_SOURCE_DIR}/tests/test_previewdecode.cpp ${_pp_pa_client_srcs})
        target_link_libraries(test_previewdecode PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Network)
        if(WIN32)
            target_link_libraries(test_previewdecode PRIVATE winmm)
        endif()
    endif()
    # PA-5 (W18) test_meterticker: one shared peak-hold timer for every meter, stopped when idle, the old decay value for
    # value; loads the real ui/VuMeter.qml and ui/AudioStrip.qml from the source tree (PP_SOURCE_DIR), no window.
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_meterticker.cpp")
        playplay_add_schedule_test(test_meterticker ${CMAKE_SOURCE_DIR}/tests/test_meterticker.cpp
            ${CMAKE_SOURCE_DIR}/playout/meterticker.cpp ${CMAKE_SOURCE_DIR}/playout/meterticker.h)
        target_link_libraries(test_meterticker PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick)
        target_compile_definitions(test_meterticker PRIVATE PP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    endif()
endif()
