# ---------------------------------------------------------------------------
# Lane "inhouse-media", DEP-21 (internal plan inhouse-deps.md §4.4 / §9): legacy app integrations behind a build option.
#   PLAYPLAY_WITH_LEGACY_CLIENTS  ON (default, today's behaviour): main.cpp constructs the ffplayout HTTP client
#                                 (integration/ffplayoutapi.*) and the OBS scheduler / command executor
#                                 (integration/obsscheduler.*, obscommandexecutor.*) at start-up and exposes them to QML
#                                 as `ffplayoutApi`, `obsScheduler` and `obsCommandExecutor`.
#                                 OFF: none of the three is constructed (no QNetworkAccessManager, no OBS schedule file
#                                 read, no context properties) and main.cpp logs that. No QML file references them,
#                                 so every page works either way. The sources still compile into PlayPlay
#                                 (CMakeLists.txt globs integration/); only their construction is gated.
# The definition is unconditional (always 0 or 1) and added to PlayPlay only; nothing changes for PlayPlaySupervisor or
# PlayPlayChannelEngine. main.cpp treats an undefined macro as 1, so a tree built without this fragment keeps the clients.
# gen_qrc.py (the Python qrc generator with hard-coded absolute paths) is gone: the hand-edited qrcs of the
# PlayPlay target (resources.qrc plus every .qrc a lane fragment adds with target_sources(PlayPlay ...), today
# Play_Play_Native/ui-buttons.qrc) are authoritative; test_inhouse_common's qrcListsEveryQmlFile keeps their union
# complete and free of double listings instead.
# ---------------------------------------------------------------------------
include_guard(GLOBAL)

option(PLAYPLAY_WITH_LEGACY_CLIENTS "Construct the legacy ffplayout HTTP / OBS WebSocket app clients at start-up (DEP-21)" ON)

if(TARGET PlayPlay)
    if(PLAYPLAY_WITH_LEGACY_CLIENTS)
        set(_pp_legacy_clients 1)
    else()
        set(_pp_legacy_clients 0)
    endif()
    set_property(TARGET PlayPlay APPEND PROPERTY COMPILE_DEFINITIONS "PLAYPLAY_WITH_LEGACY_CLIENTS=${_pp_legacy_clients}")
    message(STATUS "PlayPlay legacy ffplayout/OBS app clients: ${PLAYPLAY_WITH_LEGACY_CLIENTS} (PLAYPLAY_WITH_LEGACY_CLIENTS)")
    unset(_pp_legacy_clients)
endif()
