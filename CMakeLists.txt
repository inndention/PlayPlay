cmake_minimum_required(VERSION 3.21)
project(PlayPlay VERSION 3.0.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(MSVC)
    # Qt and DTAPI are built against the DLL runtime.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_compile_options(/W3 /MP /utf-8 /Zc:__cplusplus /permissive-)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    # Development trees (build-seg, build-lanes ...) set this ON: their app, Supervisor and engine then refuse the live station's
    # identity (IPC "PlayPlaySupervisor", REST 7700, WS 7701, the AppData data dir). The live production build tree keeps it OFF.
    option(PLAYPLAY_DEV_TREE "Binaries refuse the live Supervisor identity (set ON in every non-live build tree)" OFF)
    if(PLAYPLAY_DEV_TREE)
        add_compile_definitions(PLAYPLAY_DEV_TREE=1)
    endif()
endif()

include(cmake/Deps.cmake)

# ---------------------------------------------------------------------------
# PlayPlayCore: shared static library (domain types, services, time, IPC)
# ---------------------------------------------------------------------------
add_library(PlayPlayCore STATIC
    core/block.cpp              core/block.h
    core/cgelement.cpp          core/cgelement.h
    core/mediaitem.cpp          core/mediaitem.h
    core/playlistitem.cpp       core/playlistitem.h
    core/schedule.cpp           core/schedule.h
    core/types.cpp              core/types.h
    services/logger.cpp         services/logger.h
    services/settingsmanager.cpp services/settingsmanager.h
    services/errorbus.cpp       services/errorbus.h
    services/errorhandler.cpp   services/errorhandler.h
    services/persistentlogger.cpp services/persistentlogger.h
    services/precisiontimer.cpp services/precisiontimer.h
    services/metadataservice.cpp services/metadataservice.h
    services/mediadetection.cpp services/mediadetection.h
    services/folderscanner.cpp  services/folderscanner.h
    services/drivewatcher.cpp   services/drivewatcher.h
    utils/assethasher.cpp       utils/assethasher.h
    utils/fileresolver.cpp      utils/fileresolver.h
    utils/mediautils.cpp        utils/mediautils.h
    utils/platform.cpp          utils/platform.h
    utils/storage.cpp           utils/storage.h
    playout/timeauthority.cpp   playout/timeauthority.h
    playout/core/framerate.h
    playout/core/frameclock.cpp playout/core/frameclock.h
    playout/core/logsink.cpp    playout/core/logsink.h
    playout/core/browserlocator.cpp playout/core/browserlocator.h
    playout/core/audiodevices.cpp playout/core/audiodevices.h
    playout/core/ntpclient.cpp  playout/core/ntpclient.h
    playout/ui_ipc/ipcprotocol.cpp  playout/ui_ipc/ipcprotocol.h
    playout/ui_ipc/ipctransport.cpp playout/ui_ipc/ipctransport.h
)
target_include_directories(PlayPlayCore PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/core
    ${CMAKE_SOURCE_DIR}/services
    ${CMAKE_SOURCE_DIR}/utils
    ${CMAKE_SOURCE_DIR}/playout
)
target_link_libraries(PlayPlayCore PUBLIC Qt6::Core Qt6::Network Qt6::Sql Qt6::Concurrent)
if(WIN32)
    target_link_libraries(PlayPlayCore PUBLIC winmm ole32)
endif()

# ---------------------------------------------------------------------------
# PlayPlay: native operator application (Qt Quick + Widgets fallback)
# ---------------------------------------------------------------------------
file(GLOB APP_ROOT_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/*.cpp" "${CMAKE_SOURCE_DIR}/*.h")
file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/database/*.cpp"           "${CMAKE_SOURCE_DIR}/database/*.h"
    "${CMAKE_SOURCE_DIR}/integration/*.cpp"        "${CMAKE_SOURCE_DIR}/integration/*.h"
    "${CMAKE_SOURCE_DIR}/Play_Play_Native/*.cpp"   "${CMAKE_SOURCE_DIR}/Play_Play_Native/*.h"
)
# Control-plane sources that live in the app process (not engine / supervisor / core).
set(APP_PLAYOUT_SOURCES
    playout/supervisorclient.cpp playout/supervisorclient.h
    playout/uiscale.cpp playout/uiscale.h
    playout/nativetools.h
    playout/supervisorapi.cpp    playout/supervisorapi.h
    playout/controlservice.cpp  playout/controlservice.h
    playout/cuedispatcher.cpp   playout/cuedispatcher.h
    playout/engineadapter.cpp   playout/engineadapter.h
    playout/stateengine.cpp     playout/stateengine.h
)
# Services not in PlayPlayCore (app-only).
set(APP_SERVICE_SOURCES
    services/automationengine.cpp   services/automationengine.h
    services/autopublishservice.cpp services/autopublishservice.h
    services/blockscheduler.cpp     services/blockscheduler.h
    services/copilotservice.cpp     services/copilotservice.h
    services/fileindexer.cpp        services/fileindexer.h
    services/filetransferservice.cpp services/filetransferservice.h
    services/guestqueueservice.cpp  services/guestqueueservice.h
    services/scheduleengine.cpp     services/scheduleengine.h
    services/servicestorage.cpp     services/servicestorage.h
    services/trafficengine.cpp      services/trafficengine.h
    services/webrtcsessionmanager.cpp services/webrtcsessionmanager.h
)
# The MPV engine files are standalone experiments; keep them out of the app.
list(FILTER APP_SOURCES EXCLUDE REGEX "/integration/mpvbroadcastengine\\.(cpp|h)$")
list(FILTER APP_SOURCES EXCLUDE REGEX "/integration/mpvclient\\.(cpp|h)$")

add_executable(PlayPlay
    ${APP_ROOT_SOURCES}
    ${APP_SOURCES}
    ${APP_PLAYOUT_SOURCES}
    ${APP_SERVICE_SOURCES}
    resources.qrc
)
target_include_directories(PlayPlay PRIVATE
    ${CMAKE_SOURCE_DIR}/database
    ${CMAKE_SOURCE_DIR}/integration
    ${CMAKE_SOURCE_DIR}/Play_Play_Native
)
target_link_libraries(PlayPlay PRIVATE
    PlayPlayCore
    FFmpeg::FFmpeg
    Qt6::Gui Qt6::Widgets Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::WebSockets
)
if(PLAYPLAY_HAVE_NDI)
    target_link_libraries(PlayPlay PRIVATE NDI::NDI)
endif()
if(PLAYPLAY_HAVE_DEKTEC)
    target_link_libraries(PlayPlay PRIVATE DekTec::DTAPI)
endif()
if(WIN32)
    target_link_libraries(PlayPlay PRIVATE
        ws2_32 iphlpapi user32 gdi32 winmm secur32 bcrypt crypt32 strmiids ole32 oleaut32 shell32)
endif()
set_target_properties(PlayPlay PROPERTIES WIN32_EXECUTABLE FALSE)
if(WIN32)
    target_sources(PlayPlay PRIVATE branding/playplay.rc)          # the executable's icon in Explorer, the taskbar and Alt-Tab
endif()
if(MSVC)
    # Symbols for crash stacks (cdb/WinDbg) without changing optimisation.
    target_compile_options(PlayPlay PRIVATE /Zi)
    target_link_options(PlayPlay PRIVATE /DEBUG)
endif()

# ---------------------------------------------------------------------------
# PlayPlaySchedule: schedule model, SQLite store, runlist builder, executor
# ---------------------------------------------------------------------------
add_library(PlayPlaySchedule STATIC
    playout/schedule/schedulemodel.cpp       playout/schedule/schedulemodel.h
    playout/schedule/schedulestore.cpp       playout/schedule/schedulestore.h
    playout/schedule/fillpolicy.cpp          playout/schedule/fillpolicy.h
    playout/schedule/runlistbuilder.cpp      playout/schedule/runlistbuilder.h
    playout/schedule/schedulevalidator.cpp   playout/schedule/schedulevalidator.h
    playout/schedule/scheduleexecutor.cpp    playout/schedule/scheduleexecutor.h
)
target_sources(PlayPlaySchedule PRIVATE playout/schedule/formatclock.cpp playout/schedule/formatclock.h playout/schedule/queryblocks.cpp playout/schedule/queryblocks.h
    playout/schedule/flightplan.cpp playout/schedule/flightplan.h)
target_include_directories(PlayPlaySchedule PUBLIC ${CMAKE_SOURCE_DIR}/playout/schedule)
target_link_libraries(PlayPlaySchedule PUBLIC PlayPlayCore Qt6::Sql)

# ---------------------------------------------------------------------------
# PlayPlaySupervisor: master clock, process manager, schedule executor, REST/WS API
# ---------------------------------------------------------------------------
add_executable(PlayPlaySupervisor
    playout/supervisor/supervisor_main.cpp
    playout/supervisor/supervisorcore.cpp      playout/supervisor/supervisorcore.h
    playout/supervisor/laneext.cpp             playout/supervisor/laneext.h
    playout/supervisor/channelmanager.cpp      playout/supervisor/channelmanager.h
    playout/supervisor/restapiserver.cpp       playout/supervisor/restapiserver.h
    playout/supervisor/websocketbroker.cpp     playout/supervisor/websocketbroker.h
    playout/supervisor/failovercontroller.cpp  playout/supervisor/failovercontroller.h
    playout/supervisor/onvifmanager.cpp        playout/supervisor/onvifmanager.h
    playout/supervisor/meshclient.cpp          playout/supervisor/meshclient.h
    playout/supervisor/supervisorcore_integrations.cpp
    playout/supervisor/supervisorcore_library.cpp
    playout/supervisor/ingestmanager.cpp       playout/supervisor/ingestmanager.h
    playout/supervisor/starclient.cpp          playout/supervisor/starclient.h
    playout/supervisor/muxmanager.cpp          playout/supervisor/muxmanager.h
    playout/supervisor/scte35.cpp              playout/supervisor/scte35.h
    playout/supervisor/transcriptionmanager.cpp playout/supervisor/transcriptionmanager.h
    playout/supervisor/transcripttools.cpp     playout/supervisor/transcripttools.h
    playout/supervisor/supervisorcore_programming.cpp
    playout/supervisor/supervisorcore_spots.cpp
    playout/supervisor/spotgenerator.cpp       playout/supervisor/spotgenerator.h
    playout/supervisor/headlinenews.cpp        playout/supervisor/headlinenews.h
    playout/supervisor/localspots.cpp          playout/supervisor/localspots.h
    playout/supervisor/areamarket.cpp          playout/supervisor/areamarket.h
    playout/supervisor/marketspots.cpp         playout/supervisor/marketspots.h
    playout/supervisor/weatherspots.cpp        playout/supervisor/weatherspots.h
    playout/supervisor/supervisorcore_reports.cpp
    playout/supervisor/reports.cpp             playout/supervisor/reports.h
    playout/supervisor/supervisorcore_redundancy.cpp
    playout/supervisor/supervisorcore_automation.cpp
    playout/supervisor/recordingroutes.cpp
    playout/supervisor/rundowninsert.cpp
    playout/supervisor/sessionsmanager.cpp     playout/supervisor/sessionsmanager.h
    playout/supervisor/failoverpair.cpp        playout/supervisor/failoverpair.h
    playout/supervisor/supervisorcore_overlays.cpp
    playout/supervisor/overlaymanager.cpp      playout/supervisor/overlaymanager.h
    playout/supervisor/pillarpanels.cpp        playout/supervisor/pillarpanels.h
    playout/supervisor/cgdesign.cpp            playout/supervisor/cgdesign.h
    playout/supervisor/cgdesigner.cpp          playout/supervisor/cgdesigner.h
    playout/supervisor/mediapreview.cpp        playout/supervisor/mediapreview.h
    playout/supervisor/supervisorcore_suites.cpp
    playout/supervisor/supervisorcore_flightplan.cpp
    playout/supervisor/suitelogic.cpp          playout/supervisor/suitelogic.h
    playout/supervisor/suitehttp.cpp           playout/supervisor/suitehttp.h
    playout/supervisor/renderfarm.cpp          playout/supervisor/renderfarm.h
    playout/supervisor/starnodes.cpp           playout/supervisor/starnodes.h
    playout/supervisor/publisher.cpp           playout/supervisor/publisher.h
    playout/supervisor/spotstudio.cpp          playout/supervisor/spotstudio.h
    playout/supervisor/agentsuite.cpp          playout/supervisor/agentsuite.h
    playout/supervisor/aimodels.cpp            playout/supervisor/aimodels.h
    playout/supervisor/systemstats.cpp         playout/supervisor/systemstats.h
)
target_link_libraries(PlayPlaySupervisor PRIVATE PlayPlayCore PlayPlaySchedule Qt6::WebSockets FFmpeg::FFmpeg)
if(WIN32)
    target_link_libraries(PlayPlaySupervisor PRIVATE ws2_32 iphlpapi psapi)
    target_sources(PlayPlaySupervisor PRIVATE branding/playplay.rc)
endif()

# ---------------------------------------------------------------------------
# PlayPlayChannelEngine: per-channel decode / mix / encode / output process
# ---------------------------------------------------------------------------
# Engine pipeline as a library so tests can link it without engine_main.
add_library(PlayPlayEngine STATIC
    playout/engine/mediaframe.h
    playout/engine/engine.cpp             playout/engine/engine.h
    playout/engine/decoderpipeline.cpp    playout/engine/decoderpipeline.h
    playout/engine/mixer.cpp              playout/engine/mixer.h
    playout/engine/encoderstage.cpp       playout/engine/encoderstage.h
    playout/engine/scenerecorder.cpp      playout/engine/scenerecorder.h
    playout/engine/previewtap.cpp         playout/engine/previewtap.h
    playout/engine/sinks/sinks.h
    playout/engine/sinks/nullsink.h
    playout/engine/sinks/ffmpegmuxsink.cpp playout/engine/sinks/ffmpegmuxsink.h
    playout/engine/sinks/tsmuxer.cpp      playout/engine/sinks/tsmuxer.h
    playout/engine/sinks/dektecasisink.cpp playout/engine/sinks/dektecasisink.h
    playout/engine/sinks/dektecsdisink.cpp playout/engine/sinks/dektecsdisink.h
    playout/engine/sinks/ndisink.cpp      playout/engine/sinks/ndisink.h
    playout/engine/sinks/decklinksink.cpp playout/engine/sinks/decklinksink.h
    playout/engine/compositor/source.h
    playout/engine/compositor/basicsources.cpp  playout/engine/compositor/basicsources.h
    playout/engine/compositor/livesources.cpp   playout/engine/compositor/livesources.h
    playout/engine/compositor/audioinsource.cpp playout/engine/compositor/audioinsource.h
    playout/engine/compositor/browserhost.cpp   playout/engine/compositor/browserhost.h
    playout/engine/compositor/sourcefactory.cpp
    playout/engine/compositor/loudness.cpp      playout/engine/compositor/loudness.h
    playout/engine/compositor/compositor.cpp    playout/engine/compositor/compositor.h
    playout/engine/compositor/gpucompositor.cpp playout/engine/compositor/gpucompositor.h
)
target_include_directories(PlayPlayEngine PUBLIC ${CMAKE_SOURCE_DIR}/playout/engine)
# HTML CG layers: HtmlSource drives a headless Chromium-family browser over the DevTools protocol
# (compositor/browserhost.*): no Qt WebEngine. One QWebSocket to the browser endpoint, so WebSockets is needed here.
target_link_libraries(PlayPlayEngine PUBLIC Qt6::WebSockets Qt6::Network)
if(WIN32)
    target_link_libraries(PlayPlayEngine PUBLIC d3d11 dxgi d3dcompiler)
endif()
target_link_libraries(PlayPlayEngine PUBLIC PlayPlayCore FFmpeg::FFmpeg Qt6::Gui)
if(PLAYPLAY_HAVE_NDI)
    target_link_libraries(PlayPlayEngine PUBLIC NDI::NDI)
endif()
if(PLAYPLAY_HAVE_DEKTEC)
    target_link_libraries(PlayPlayEngine PUBLIC DekTec::DTAPI)
endif()
if(PLAYPLAY_HAVE_DECKLINK)
    target_link_libraries(PlayPlayEngine PUBLIC DeckLink::SDK)
endif()

add_executable(PlayPlayChannelEngine playout/engine/engine_main.cpp)
target_link_libraries(PlayPlayChannelEngine PRIVATE PlayPlayEngine)
add_executable(PlayPlayRecompose tools/recompose/main.cpp)
target_link_libraries(PlayPlayRecompose PRIVATE PlayPlayEngine)
if(WIN32)
    target_link_libraries(PlayPlayChannelEngine PRIVATE ws2_32 winmm)
endif()

# ---------------------------------------------------------------------------
# Runtime DLL staging (FFmpeg, NDI) next to the executables
# ---------------------------------------------------------------------------
set(_runtime_dlls ${PLAYPLAY_FFMPEG_RUNTIME_DLLS} ${PLAYPLAY_FFMPEG_TOOLS})
if(PLAYPLAY_HAVE_NDI AND EXISTS "${PLAYPLAY_NDI_RUNTIME_DLL}")
    list(APPEND _runtime_dlls "${PLAYPLAY_NDI_RUNTIME_DLL}")
endif()
# Qt platform plugins: Qt searches <exe dir>/platforms, so staging qwindows and
# qoffscreen here lets the app, the headless engine and the tests run from the
# build tree without QT_PLUGIN_PATH.
set(_qt_platforms "${PLAYPLAY_QT_ROOT}/lib/qt6/plugins/platforms")  # NB: Library/plugins is Qt 5
add_custom_target(stage_runtime ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_runtime_dlls} "${CMAKE_BINARY_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/platforms"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_qt_platforms}/qwindows.dll" "${_qt_platforms}/qoffscreen.dll" "${_qt_platforms}/qminimal.dll"
        "${CMAKE_BINARY_DIR}/platforms"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/sqldrivers"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PLAYPLAY_QT_ROOT}/lib/qt6/plugins/sqldrivers/qsqlite.dll" "${CMAKE_BINARY_DIR}/sqldrivers"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/imageformats"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PLAYPLAY_QT_ROOT}/lib/qt6/plugins/imageformats/qjpeg.dll" "${CMAKE_BINARY_DIR}/imageformats"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/web" "${CMAKE_BINARY_DIR}/web"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/filters" "${CMAKE_BINARY_DIR}/filters"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/docs/scene-recording-and-slaves.md" "${CMAKE_BINARY_DIR}/SCENE-RECORDING-AND-SLAVES.md"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/scripts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/scripts/benchmark-vapoursynth.py" "${CMAKE_BINARY_DIR}/scripts/benchmark-vapoursynth.py"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/scripts/transcribe.py" "${CMAKE_BINARY_DIR}/scripts/transcribe.py"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/scripts/spot_tts.py" "${CMAKE_BINARY_DIR}/scripts/spot_tts.py"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/scripts/spot_card.py" "${CMAKE_BINARY_DIR}/scripts/spot_card.py"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/scripts/weather_cards.py" "${CMAKE_BINARY_DIR}/scripts/weather_cards.py"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/scripts/kokoro_sherpa.js" "${CMAKE_BINARY_DIR}/scripts/kokoro_sherpa.js"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/scripts/spot_music.py" "${CMAKE_BINARY_DIR}/scripts/spot_music.py"
    COMMENT "Staging FFmpeg/NDI runtime DLLs, Qt platform plugins and the web dashboard"
    VERBATIM
)
add_dependencies(PlayPlay stage_runtime)

# ---------------------------------------------------------------------------
# qt.conf: lets every executable in the build tree find Qt's plugins and QML
# modules when launched directly (this Qt keeps them under lib/qt6/...).
# A deployed install uses windeployqt instead (scripts/build.ps1 -Deploy).
# ---------------------------------------------------------------------------
file(WRITE "${CMAKE_BINARY_DIR}/qt.conf"
"[Paths]
Prefix=${PLAYPLAY_QT_ROOT}
Plugins=lib/qt6/plugins
QmlImports=lib/qt6/qml
")

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(PLAYPLAY_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Lane fragments: self-contained tools and modules (cmake/lane-<name>.cmake), each owned by one workstream so
# parallel work never edits this file. A fragment may use playplay_add_test / playplay_add_schedule_test.
# ---------------------------------------------------------------------------
file(GLOB PLAYPLAY_LANE_FRAGMENTS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/cmake/lane-*.cmake")
list(SORT PLAYPLAY_LANE_FRAGMENTS)
foreach(_frag IN LISTS PLAYPLAY_LANE_FRAGMENTS)
    include("${_frag}")
endforeach()

message(STATUS "PlayPlay ${PROJECT_VERSION} configured: NDI=${PLAYPLAY_HAVE_NDI} DekTec=${PLAYPLAY_HAVE_DEKTEC} DeckLink=${PLAYPLAY_HAVE_DECKLINK} Tests=${PLAYPLAY_BUILD_TESTS}")
