# Lane "inhouse-mux" (internal plan inhouse-deps.md: DEP-14..18, DEP-02b and the mux `-flags` fix MX-0): in-house
# PSI/PSIP/SCTE-35 sections, the PlayPlayMux TS analyzer / DTAPI tool and MuxManager tests. Included by CMakeLists.txt
# (cmake/lane-*.cmake). The only additions to a shipped executable are DEP-02b's PlayPlayAvKit + version.lib and DEP-15's
# PlayPlayTsKit in PlayPlaySupervisor (below); the libraries and the tool are EXCLUDE_FROM_ALL, so a full build of any tree
# compiles them only for a target that needs them (the Supervisor now needs PlayPlayAvKit and PlayPlayTsKit).

# PlayPlayInhouseCommon (ppcli: the last-line JSON contract of every in-house tool) for PlayPlayMux; the include guard in
# that fragment makes this a no-op when the glob already included it.
include("${CMAKE_CURRENT_LIST_DIR}/lane-inhouse-common.cmake")
# PlayPlayAvKit (lane inhouse-media, DEP-01) for DEP-02b; include_guard'ed there, so a no-op after the glob included it.
include("${CMAKE_CURRENT_LIST_DIR}/lane-inhouse-avkit.cmake")

# DEP-02b: MuxManager::capabilities() builds its "ffmpeg" object in-process with PlayPlayAvKit (avkit::ffmpegCapabilities)
# instead of spawning `ffmpeg -encoders` / `-version` with up to 25 s of waitForFinished on the Supervisor main thread, and
# reads tsp.exe's version from its version resource (GetFileVersionInfoW: version.lib) instead of running `tsp --version`.
# PlayPlaySupervisor already links FFmpeg::FFmpeg; AvKit adds no DLL it does not load today. Unconditional on purpose: the
# live tree includes this fragment on its next build and muxmanager.cpp needs both.
target_link_libraries(PlayPlaySupervisor PRIVATE PlayPlayAvKit)
if(WIN32)
    target_link_libraries(PlayPlaySupervisor PRIVATE version)
endif()

# PlayPlayTsKit (DEP-14): sections, packetizer, PSI/PSIP tables, SCTE-35, the TSDuck-XML compiler; QtCore only. Globbed so
# later slices (analyzer DEP-16) only add files. mux_main.cpp is the tool's entry point; dtdevice.cpp needs DTAPI and is
# its own library (PlayPlayDtDevice, DEP-17), so neither goes into PlayPlayTsKit.
file(GLOB _pp_tskit_sources CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tools/mux/*.cpp" "${CMAKE_SOURCE_DIR}/tools/mux/*.h")
list(FILTER _pp_tskit_sources EXCLUDE REGEX "/tools/mux/(mux_main|dtdevice)\\.(cpp|h)$")
add_library(PlayPlayTsKit STATIC EXCLUDE_FROM_ALL ${_pp_tskit_sources})
target_include_directories(PlayPlayTsKit PUBLIC "${CMAKE_SOURCE_DIR}/tools/mux")
target_link_libraries(PlayPlayTsKit PUBLIC Qt6::Core)
if(WIN32)
    target_link_libraries(PlayPlayTsKit PUBLIC ws2_32)    # UDP multicast input of the analyzer (DEP-16)
endif()
set_target_properties(PlayPlayTsKit PROPERTIES AUTOUIC OFF AUTORCC OFF)

# DEP-15: MuxManager::compileTables() validates the PSIP / PAT / SCTE-35 XML in-process with PlayPlayTsKit (mux.config.tables =
# native, the default) instead of up to 260 sequential tstabcomp spawns waited for on the Supervisor main thread. QtCore + ws2_32
# only (both already in the Supervisor). Unconditional on purpose: the live tree includes this fragment on its next build and
# muxmanager.cpp needs it.
target_link_libraries(PlayPlaySupervisor PRIVATE PlayPlayTsKit)

# PlayPlayDtDevice (DEP-17): DekTec device listing and port preparation (tools/mux/dtdevice.*) behind `PlayPlayMux dektec`.
# DTAPI only where the SDK was found (PUBLIC, so users see HAS_DEKTEC and the DTAPI lib directory); without it the backend
# reports itself unavailable and everything still builds.
add_library(PlayPlayDtDevice STATIC EXCLUDE_FROM_ALL
    "${CMAKE_SOURCE_DIR}/tools/mux/dtdevice.cpp" "${CMAKE_SOURCE_DIR}/tools/mux/dtdevice.h")
target_include_directories(PlayPlayDtDevice PUBLIC "${CMAKE_SOURCE_DIR}/tools/mux")
target_link_libraries(PlayPlayDtDevice PUBLIC Qt6::Core)
if(PLAYPLAY_HAVE_DEKTEC)
    target_link_libraries(PlayPlayDtDevice PUBLIC DekTec::DTAPI)
endif()
set_target_properties(PlayPlayDtDevice PROPERTIES AUTOUIC OFF AUTORCC OFF)

# PlayPlayMux.exe: compile (= tstabcomp --atsc), pmt-scte35, packetize, analyze (= tsp -P tables / analyze, DEP-16),
# dektec list|prep (= tsdektec -a -v / --output, DEP-17). Build it by name.
add_executable(PlayPlayMux EXCLUDE_FROM_ALL "${CMAKE_SOURCE_DIR}/tools/mux/mux_main.cpp")
target_link_libraries(PlayPlayMux PRIVATE PlayPlayTsKit PlayPlayDtDevice PlayPlayInhouseCommon)
set_target_properties(PlayPlayMux PROPERTIES AUTOUIC OFF AUTORCC OFF)

# The PSIP / TS analyzer / DekTec tests compare against recorded goldens in tests/inhouse/data/psip, which is not part of the
# public tree; without it they are not registered.
if(IS_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/inhouse/data/psip")
    set(_pp_mux_goldens ON)
else()
    set(_pp_mux_goldens OFF)
    message(STATUS "tests/inhouse/data/psip is missing: test_inhouse_psip, test_inhouse_tsanalyzer and test_inhouse_dtdevice are not registered")
endif()

if(PLAYPLAY_BUILD_TESTS AND _pp_mux_goldens AND COMMAND playplay_add_schedule_test)
    # test_inhouse_psip: MuxManager's pure argument builder (ffmpegArgs, MX-0), the section-library goldens (DEP-14), capabilities
    # (DEP-02b) and the in-process / asynchronous tools (DEP-15). Compiles muxmanager.cpp itself (with scte35.cpp: the SCTE-35 XML
    # the Supervisor really sends is compiled in the test; laneext.cpp + restapiserver.cpp for the async routes it registers).
    playplay_add_schedule_test(test_inhouse_psip
        ${CMAKE_SOURCE_DIR}/tests/inhouse/test_inhouse_psip.cpp
        ${CMAKE_SOURCE_DIR}/playout/supervisor/muxmanager.cpp    ${CMAKE_SOURCE_DIR}/playout/supervisor/muxmanager.h
        ${CMAKE_SOURCE_DIR}/playout/supervisor/scte35.cpp        ${CMAKE_SOURCE_DIR}/playout/supervisor/scte35.h
        ${CMAKE_SOURCE_DIR}/playout/supervisor/laneext.cpp       ${CMAKE_SOURCE_DIR}/playout/supervisor/laneext.h
        ${CMAKE_SOURCE_DIR}/playout/supervisor/restapiserver.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/restapiserver.h)
    target_link_libraries(test_inhouse_psip PRIVATE Qt6::Network PlayPlayTsKit PlayPlayAvKit)
    if(WIN32)
        target_link_libraries(test_inhouse_psip PRIVATE version)
    endif()
    # PP_FFMPEG_BIN: ffmpeg.exe of the FFmpeg build being linked, the oracle for DEP-02b's in-process capabilities (the
    # old `ffmpeg -encoders` / `-version` parse, compared field by field). PLAYPLAYMUX_EXE: the tool DEP-15 runs (analyze,
    # dektec list / prep on a simulated card only).
    target_compile_definitions(test_inhouse_psip PRIVATE PSIP_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/inhouse/data/psip"
        PP_FFMPEG_BIN="${PLAYPLAY_FFMPEG_ROOT}/bin" PLAYPLAYMUX_EXE="$<TARGET_FILE:PlayPlayMux>")
    add_dependencies(test_inhouse_psip PlayPlayMux)
    set_tests_properties(test_inhouse_psip PROPERTIES TIMEOUT 120)
endif()

if(PLAYPLAY_BUILD_TESTS AND _pp_mux_goldens AND COMMAND playplay_add_test)
    # test_inhouse_tsanalyzer (DEP-16): the analyzer behind `PlayPlayMux analyze` on a stream the engine's TsMuxer writes
    # (playplay_add_test links PlayPlayEngine + FFmpeg), the tsp-format goldens in tests/inhouse/data/psip/tables, UDP input
    # on loopback (127.0.0.1:50071-50074, multicast 239.255.97.250 joined on 127.0.0.1 only) and the CLI itself.
    playplay_add_test(test_inhouse_tsanalyzer ${CMAKE_SOURCE_DIR}/tests/inhouse/test_inhouse_tsanalyzer.cpp)
    target_link_libraries(test_inhouse_tsanalyzer PRIVATE PlayPlayTsKit Qt6::Network)
    target_compile_definitions(test_inhouse_tsanalyzer PRIVATE
        TSANALYZER_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/inhouse/data/psip"
        PLAYPLAYMUX_EXE="$<TARGET_FILE:PlayPlayMux>")
    add_dependencies(test_inhouse_tsanalyzer PlayPlayMux)
    set_tests_properties(test_inhouse_tsanalyzer PROPERTIES TIMEOUT 120)
endif()

if(PLAYPLAY_BUILD_TESTS AND _pp_mux_goldens AND COMMAND playplay_add_inhouse_test)
    # test_inhouse_dtdevice (DEP-17): prep decisions and list JSON / text on a simulated card (SimHardware), the CLI with
    # --sim, MuxManager's tsdektec parser on a recorded `tsdektec -a -v`; with DTAPI and a card: `list` against tsdektec
    # (read-only). Tests that attach a channel run only with PLAYPLAY_DEKTEC_HWTEST=1 (never on output ports).
    # EXCLUDE_FROM_ALL (the common helper): build it by name.
    playplay_add_inhouse_test(test_inhouse_dtdevice
        SOURCES "${CMAKE_SOURCE_DIR}/tests/inhouse/test_inhouse_dtdevice.cpp"
        LIBS PlayPlayDtDevice PlayPlayInhouseCommon)
    target_compile_definitions(test_inhouse_dtdevice PRIVATE
        DTDEVICE_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/inhouse/data/psip/dektec"
        PLAYPLAYMUX_EXE="$<TARGET_FILE:PlayPlayMux>")
    add_dependencies(test_inhouse_dtdevice PlayPlayMux)
endif()
