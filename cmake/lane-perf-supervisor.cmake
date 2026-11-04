# ---------------------------------------------------------------------------
# Lane "perf-supervisor" (internal plan perf-supervisor-app.md W5-W8, W12-W16; slices PS-1..PS-10 in
# internal plan lane-plan.json): keep the Supervisor's event loop from blocking. Included by the lane glob in CMakeLists.txt.
#
# Production changes land in files PlayPlaySupervisor / PlayPlayChannelEngine already compile (websocketbroker.cpp,
# ipctransport.cpp, ...), so the live build needs nothing from here unless a slice adds a new .cpp to a target; such
# additions use target_sources and stay unconditional. Tests are registered per slice below; each block checks its
# source exists so slices can land in any order.
# ---------------------------------------------------------------------------

# PS-6 (W12): the UI state publisher. UNCONDITIONAL on purpose: supervisorcore.cpp uses it, so every tree (the live
# production build included) must compile it into PlayPlaySupervisor or the next build fails to link.
target_sources(PlayPlaySupervisor PRIVATE
    ${CMAKE_SOURCE_DIR}/playout/supervisor/statepublisher.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/statepublisher.h)

# PS-3 (W5): the engine's Supervisor link. UNCONDITIONAL on purpose: engine_main.cpp uses both, so every tree (the live
# production build included) must compile them or PlayPlayChannelEngine fails to link. enginelock (engine.lock and the
# one-engine-per-channel mutex) goes into PlayPlayCore because the Supervisor's reaper (PS-4a) reads the same lock;
# supervisorlink only into the engine executable (perf-engine's library glob skips it on purpose).
target_sources(PlayPlayCore PRIVATE
    ${CMAKE_SOURCE_DIR}/playout/core/enginelock.cpp ${CMAKE_SOURCE_DIR}/playout/core/enginelock.h)
target_sources(PlayPlayChannelEngine PRIVATE
    ${CMAKE_SOURCE_DIR}/playout/engine/supervisorlink.cpp ${CMAKE_SOURCE_DIR}/playout/engine/supervisorlink.h)

# PS-4a (W6): the engine reaper core (pure decide(), lock-file + process-list scan scoped to (ipcName, dataDir)) and the
# Supervisor single-instance mutex. UNCONDITIONAL so PS-4b's supervisorcore / supervisor_main wiring links in every tree
# (the live build included); until then nothing calls them, so the live Supervisor's behaviour is unchanged.
target_sources(PlayPlaySupervisor PRIVATE
    ${CMAKE_SOURCE_DIR}/playout/supervisor/enginereaper.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/enginereaper.h
    ${CMAKE_SOURCE_DIR}/playout/supervisor/singleinstance.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/singleinstance.h)

# PS-8 (W14 ingest part): the BlockingWorker (serial worker thread with results on the owner's thread). UNCONDITIONAL on
# purpose: ingestmanager.cpp (in PlayPlaySupervisor's own source list) uses it, so every tree (the live build included)
# must compile it or PlayPlaySupervisor fails to link.
target_sources(PlayPlaySupervisor PRIVATE
    ${CMAKE_SOURCE_DIR}/playout/supervisor/blockingworker.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/blockingworker.h)

# PS-9 (W15): read-only reader connections for the heavy REST GETs. UNCONDITIONAL on purpose: supervisorcore.cpp
# (/asrun) uses it, so every tree (the live build included) must compile it into PlayPlaySchedule or the Supervisor
# fails to link. Without a readable database the Supervisor falls back to the synchronous store.
target_sources(PlayPlaySchedule PRIVATE
    ${CMAKE_SOURCE_DIR}/playout/schedule/storereader.cpp ${CMAKE_SOURCE_DIR}/playout/schedule/storereader.h)

if(PLAYPLAY_BUILD_TESTS AND COMMAND playplay_add_schedule_test)
    # PS-9 (W15) test_restapiserver: the route trie against the old per-request matcher for every key of a route dump
    # (the Supervisor's ~412 routes, embedded; PP_ROUTE_DUMP=<file> overrides), 2,000 idle keep-alive sockets closed by
    # the sweep, a stable handle count over 10,000 requests, 431 on an oversize header, the slow-handler log and the
    # watchdog tag, and the StoreReader (/asrun off the loop against the store's own answer; a 2 s read while the loop
    # answers). Compiles the server itself on an ephemeral port; temp databases only.
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_restapiserver.cpp")
        playplay_add_schedule_test(test_restapiserver
            ${CMAKE_SOURCE_DIR}/tests/test_restapiserver.cpp
            ${CMAKE_SOURCE_DIR}/playout/supervisor/restapiserver.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/restapiserver.h)
        target_include_directories(test_restapiserver PRIVATE ${CMAKE_SOURCE_DIR})
        target_link_libraries(test_restapiserver PRIVATE Qt6::Network Qt6::Sql)
        if(WIN32)
            target_link_libraries(test_restapiserver PRIVATE psapi)
        endif()
        set_tests_properties(test_restapiserver PROPERTIES TIMEOUT 150)
    endif()
    # PS-10 (W16) test_wsbroker: WebSocketBroker fan-out, low-rate coalescing and the per-client pending cap with 20 local
    # clients (one parked). Compiles the broker itself, so it needs only the schedule library and Qt WebSockets.
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_wsbroker.cpp")
        playplay_add_schedule_test(test_wsbroker
            ${CMAKE_SOURCE_DIR}/tests/test_wsbroker.cpp
            ${CMAKE_SOURCE_DIR}/playout/supervisor/websocketbroker.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/websocketbroker.h)
        target_link_libraries(test_wsbroker PRIVATE Qt6::WebSockets Qt6::Network)
    endif()
    # PS-1 (W7, PE-20) test_ipcbackpressure: IpcTransport caps (droppable / disconnect), the 64 MB frame limit and the
    # async client connect, against raw Win32 pipe ends that never read. IpcTransport lives in PlayPlayCore (linked via
    # the schedule library). test_ipcprotocol (tests/CMakeLists.txt) covers framing through the transport.
    # Windows only (raw named-pipe ends).
    if(WIN32 AND EXISTS "${CMAKE_SOURCE_DIR}/tests/test_ipcbackpressure.cpp")
        playplay_add_schedule_test(test_ipcbackpressure ${CMAKE_SOURCE_DIR}/tests/test_ipcbackpressure.cpp)
        target_link_libraries(test_ipcbackpressure PRIVATE Qt6::Network psapi)
    endif()
    # PS-5 (W8) test_ipcbinary: binary preview frames (codec, negotiation, a mixed old/new relay with a raw pre-PS-5
    # reader, receiver safety), preview subscriptions, and the relay CPU bench (100 frames/s, JSON/base64 vs binary).
    # Windows only (raw named-pipe ends, QueryThreadCycleTime).
    if(WIN32 AND EXISTS "${CMAKE_SOURCE_DIR}/tests/test_ipcbinary.cpp")
        playplay_add_schedule_test(test_ipcbinary ${CMAKE_SOURCE_DIR}/tests/test_ipcbinary.cpp)
        target_link_libraries(test_ipcbinary PRIVATE Qt6::Network)
        set_tests_properties(test_ipcbinary PROPERTIES TIMEOUT 120)
    endif()
    # Engine stand-in for ChannelManager tests (PS-2, PS-4b): tests/helpers/fake_engine.cpp takes the engine's command
    # line, connects to the given IPC name, sends READY and heartbeats, and obeys fake_* transport commands; with
    # --fake-link it runs the real engine's SupervisorLink + duplicate guard (PS-4b), with --fake-stay it plays a
    # pre-PS-3 engine. Out of "all"; tests that spawn it depend on it and get its path as PP_FAKE_ENGINE.
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/helpers/fake_engine.cpp" AND NOT TARGET fake_engine)
        add_executable(fake_engine EXCLUDE_FROM_ALL ${CMAKE_SOURCE_DIR}/tests/helpers/fake_engine.cpp
            ${CMAKE_SOURCE_DIR}/playout/engine/supervisorlink.cpp ${CMAKE_SOURCE_DIR}/playout/engine/supervisorlink.h)
        target_include_directories(fake_engine PRIVATE ${CMAKE_SOURCE_DIR})
        target_link_libraries(fake_engine PRIVATE PlayPlayCore)
    endif()
    # PS-2 (C6, W14 channelmanager part) test_channelmanager: one respawn per engine death with the Supervisor's restart
    # sources wired as in supervisorcore.cpp (incl. FailoverController), and kill / restart that never wait on the loop.
    # Compiles the manager and FailoverController themselves; engines are fake_engine on a private IPC name. The manager
    # uses the reaper core since PS-4b.
    if(TARGET fake_engine AND EXISTS "${CMAKE_SOURCE_DIR}/tests/test_channelmanager.cpp")
        playplay_add_schedule_test(test_channelmanager
            ${CMAKE_SOURCE_DIR}/tests/test_channelmanager.cpp
            ${CMAKE_SOURCE_DIR}/playout/supervisor/channelmanager.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/channelmanager.h
            ${CMAKE_SOURCE_DIR}/playout/supervisor/enginereaper.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/enginereaper.h
            ${CMAKE_SOURCE_DIR}/playout/supervisor/failovercontroller.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/failovercontroller.h)
        target_compile_definitions(test_channelmanager PRIVATE PP_FAKE_ENGINE="$<TARGET_FILE:fake_engine>")
        add_dependencies(test_channelmanager fake_engine)
        set_tests_properties(test_channelmanager PROPERTIES TIMEOUT 120)
    endif()
    # PS-4a (W6) + PS-4b test_enginereaper: decide() / decideAll() tables, the scan over a temp data dir with fake
    # engine.lock files and over child processes with engine-like command lines (scope), SingleInstance and the
    # supervisor_main guard flow (/healthz, --replace-hung: dump + terminate), and the wiring end to end: this binary in
    # --sup-child mode is a Supervisor (IpcTransport + ChannelManager + reaper) that spawns fake_engine --fake-link; it is
    # killed and restarted to show adoption (same pid), reaping on a build change, a lockless legacy engine matched by
    # its command line, the binding guard, the re-scan, and that another IPC name's engine is never touched.
    if(WIN32 AND TARGET fake_engine AND EXISTS "${CMAKE_SOURCE_DIR}/tests/test_enginereaper.cpp")
        playplay_add_schedule_test(test_enginereaper
            ${CMAKE_SOURCE_DIR}/tests/test_enginereaper.cpp
            ${CMAKE_SOURCE_DIR}/playout/supervisor/enginereaper.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/enginereaper.h
            ${CMAKE_SOURCE_DIR}/playout/supervisor/singleinstance.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/singleinstance.h
            ${CMAKE_SOURCE_DIR}/playout/supervisor/channelmanager.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/channelmanager.h)
        target_link_libraries(test_enginereaper PRIVATE Qt6::Network)
        target_compile_definitions(test_enginereaper PRIVATE PP_FAKE_ENGINE="$<TARGET_FILE:fake_engine>")
        add_dependencies(test_enginereaper fake_engine)
        set_tests_properties(test_enginereaper PROPERTIES TIMEOUT 150)
    endif()
    # PS-6 (W12) test_statepublisher: the snapshot's stable hash, publish-on-change with keepalive, query replies and
    # runlist bookkeeping, the ScheduleStore channel cache against the old per-call SQL, the settings/schedule revision
    # split, and IPC bytes to a fake UI over 10 simulated minutes (old vs new wiring through a real IpcTransport).
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_statepublisher.cpp")
        playplay_add_schedule_test(test_statepublisher
            ${CMAKE_SOURCE_DIR}/tests/test_statepublisher.cpp
            ${CMAKE_SOURCE_DIR}/playout/supervisor/statepublisher.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/statepublisher.h)
        target_link_libraries(test_statepublisher PRIVATE Qt6::Network Qt6::Sql)
        set_tests_properties(test_statepublisher PROPERTIES TIMEOUT 120)
    endif()
    # PS-7 (W13) test_executor_flap: the executor's planned-list hash cached at rebuild, rebuilds only on a schedule
    # revision or a write of the settings the build reads (not on the setSetting storm), the emergency loop pushed once
    # (no startup emergency/join flap), the fixed-source hash across hours and reload, and the per-tick cost bench.
    # Schedule-only (fake EngineLink, temp store).
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_executor_flap.cpp")
        playplay_add_schedule_test(test_executor_flap ${CMAKE_SOURCE_DIR}/tests/test_executor_flap.cpp)
        set_tests_properties(test_executor_flap PROPERTIES TIMEOUT 120)
    endif()
    # PS-8 (W14 ingest part) test_blockingworker: BlockingWorker (owner-thread delivery, FIFO, coalescing, cancel, a
    # hung job at destruction) and the IngestManager work that moved onto it: the async sweep against the synchronous
    # one (same records), a 10,000-file watch folder walked off the loop with the old rules' outcome, an unreachable UNC
    # watch path that holds the worker and not the loop, the drive poll priming. Compiles the manager itself; temp
    # stores and dirs only. Windows only (the UNC path and QStorageInfo behaviour it measures).
    if(WIN32 AND EXISTS "${CMAKE_SOURCE_DIR}/tests/test_blockingworker.cpp")
        playplay_add_schedule_test(test_blockingworker
            ${CMAKE_SOURCE_DIR}/tests/test_blockingworker.cpp
            ${CMAKE_SOURCE_DIR}/playout/supervisor/ingestmanager.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/ingestmanager.h
            ${CMAKE_SOURCE_DIR}/playout/supervisor/blockingworker.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/blockingworker.h)
        set_tests_properties(test_blockingworker PROPERTIES TIMEOUT 150)
    endif()
    # Supervisor stand-in for SupervisorLink tests (PS-3; PS-4b may reuse it): tests/helpers/fake_supervisor.cpp listens on
    # a private IPC name, prints every message, optionally adopts engines (MSG_SUP_ALIVE for their channel) and can die
    # abruptly to play the engine's parent. Out of "all"; tests get its path as PP_FAKE_SUPERVISOR.
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/helpers/fake_supervisor.cpp" AND NOT TARGET fake_supervisor)
        add_executable(fake_supervisor EXCLUDE_FROM_ALL ${CMAKE_SOURCE_DIR}/tests/helpers/fake_supervisor.cpp)
        target_link_libraries(fake_supervisor PRIVATE PlayPlayCore)
    endif()
    # PS-3 (W5) test_supervisorlink: HELLO before READY on every (re)connect, the 1 s async reconnect, engine.lock, the
    # parent watch / orphan grace / adoption, and the duplicate guard (exit code 4). Compiles the link itself; engine-like
    # processes are this test binary in --engine-child mode (no video, no outputs). Private IPC names only.
    if(TARGET fake_supervisor AND EXISTS "${CMAKE_SOURCE_DIR}/tests/test_supervisorlink.cpp")
        playplay_add_schedule_test(test_supervisorlink
            ${CMAKE_SOURCE_DIR}/tests/test_supervisorlink.cpp
            ${CMAKE_SOURCE_DIR}/playout/engine/supervisorlink.cpp ${CMAKE_SOURCE_DIR}/playout/engine/supervisorlink.h)
        target_compile_definitions(test_supervisorlink PRIVATE PP_FAKE_SUPERVISOR="$<TARGET_FILE:fake_supervisor>")
        add_dependencies(test_supervisorlink fake_supervisor)
        set_tests_properties(test_supervisorlink PROPERTIES TIMEOUT 150)
    endif()
endif()
