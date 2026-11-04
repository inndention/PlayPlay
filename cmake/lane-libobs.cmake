# Lane libobs (docs/libobs-spike.md): libobs (OBS Studio's core) running headless as ONE live-input layer inside the
# channel engine — the hybrid spike. Everything is behind PLAYPLAY_WITH_LIBOBS (default OFF): with the option off this
# fragment defines two cache variables and nothing else, so a live-tree build is byte-for-byte unaffected.
# Lane rules: existing targets are touched only through target_sources / target_link_libraries /
# target_compile_definitions (plus a staging dependency); new executables are registered here.
option(PLAYPLAY_WITH_LIBOBS "Build the libobs live-input source (LayerSource type \"libobs\") against a libobs install" OFF)
set(PLAYPLAY_LIBOBS_ROOT "C:/SDKs/obs/install" CACHE PATH
    "libobs install prefix: bin/64bit/obs.dll (+deps), lib/obs.lib, include/obs.h, core/<module>/<module>.dll, data/libobs/*.effect")

# Supervisor side (docs/libobs.md): /api/v1/libobs/* routes (catalog, saved scenes, show / switch / hide / status per channel)
# through the lane-extension registry. UNCONDITIONAL: the Supervisor answers them with or without the engine option, so the
# UIs behave the same against either engine (an engine without the source refuses the layer; status says so).
if(TARGET PlayPlaySupervisor)
    target_sources(PlayPlaySupervisor PRIVATE "${CMAKE_SOURCE_DIR}/playout/supervisor/libobsroutes.cpp")
endif()
if(PLAYPLAY_BUILD_TESTS AND COMMAND playplay_add_schedule_test AND EXISTS "${CMAKE_SOURCE_DIR}/tests/test_libobsroutes.cpp")
    # Route validation and the scene store against a real RestApiServer on an ephemeral port and a temp store; no engine, no
    # Supervisor process, no libobs (the module compiles without obs.h).
    playplay_add_schedule_test(test_libobsroutes "${CMAKE_SOURCE_DIR}/tests/test_libobsroutes.cpp"
        "${CMAKE_SOURCE_DIR}/playout/supervisor/libobsroutes.cpp"
        "${CMAKE_SOURCE_DIR}/playout/supervisor/laneext.cpp"       "${CMAKE_SOURCE_DIR}/playout/supervisor/laneext.h"
        "${CMAKE_SOURCE_DIR}/playout/supervisor/restapiserver.cpp" "${CMAKE_SOURCE_DIR}/playout/supervisor/restapiserver.h")
    target_include_directories(test_libobsroutes PRIVATE "${CMAKE_SOURCE_DIR}")
    target_link_libraries(test_libobsroutes PRIVATE Qt6::Network Qt6::Sql)
    if(WIN32)
        target_link_libraries(test_libobsroutes PRIVATE psapi)
    endif()
    set_tests_properties(test_libobsroutes PROPERTIES TIMEOUT 120)
endif()

if(PLAYPLAY_WITH_LIBOBS)
    set(_lo_inc "${PLAYPLAY_LIBOBS_ROOT}/include")
    set(_lo_lib "${PLAYPLAY_LIBOBS_ROOT}/lib/obs.lib")
    set(_lo_bin "${PLAYPLAY_LIBOBS_ROOT}/bin/64bit")
    if(NOT EXISTS "${_lo_inc}/obs.h" OR NOT EXISTS "${_lo_lib}" OR NOT EXISTS "${_lo_bin}/obs.dll")
        message(FATAL_ERROR "PLAYPLAY_WITH_LIBOBS=ON but no libobs install under ${PLAYPLAY_LIBOBS_ROOT} "
                            "(need include/obs.h, lib/obs.lib, bin/64bit/obs.dll; build recipe: docs/libobs-spike.md)")
    endif()
    file(STRINGS "${_lo_inc}/obs-config.h" _lo_ver REGEX "#define LIBOBS_API_(MAJOR|MINOR|PATCH)_VER ")
    string(REGEX REPLACE "[^0-9;]" "" _lo_ver "${_lo_ver}")
    string(REPLACE ";" "." _lo_ver "${_lo_ver}")
    message(STATUS "lane libobs: libobs ${_lo_ver} from ${PLAYPLAY_LIBOBS_ROOT}")

    # obs.dll as an imported target. The exported libobsConfig.cmake wants the obs-deps finders (SIMDe, w32-pthreads)
    # on CMAKE_PREFIX_PATH; the public headers need neither on x64 MSVC, so a plain imported target is enough.
    add_library(LibObs::libobs SHARED IMPORTED)
    set_target_properties(LibObs::libobs PROPERTIES
        IMPORTED_LOCATION "${_lo_bin}/obs.dll"
        IMPORTED_IMPLIB "${_lo_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_lo_inc}")

    target_sources(PlayPlayEngine PRIVATE
        "${CMAKE_SOURCE_DIR}/playout/engine/compositor/libobssource.cpp"
        "${CMAKE_SOURCE_DIR}/playout/engine/compositor/libobssource.h")
    target_link_libraries(PlayPlayEngine PUBLIC LibObs::libobs)
    # PUBLIC: sourcefactory.cpp (in the library) and the tests see the define.
    target_compile_definitions(PlayPlayEngine PUBLIC PLAYPLAY_WITH_LIBOBS=1 PLAYPLAY_LIBOBS_ROOT="${PLAYPLAY_LIBOBS_ROOT}")

    # Runtime: obs.dll, its graphics backend and the obs-deps DLLs next to the executables (same idea as the
    # FFmpeg/NDI staging in stage_runtime). The modules under core/ and data/ are loaded from PLAYPLAY_LIBOBS_ROOT.
    # (Globbed at configure time like the FFmpeg DLLs in Deps.cmake: after adding a DLL to bin/64bit, re-run cmake.)
    file(GLOB _lo_dlls "${_lo_bin}/*.dll")
    add_custom_target(stage_libobs
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_lo_dlls} "${CMAKE_BINARY_DIR}"
        COMMENT "Staging libobs runtime DLLs from ${_lo_bin}"
        VERBATIM)
    add_dependencies(PlayPlayChannelEngine stage_libobs)

    if(PLAYPLAY_BUILD_TESTS)
        # Isolated source test: colour scenes, 120 frames at 29.97, frame age, cut latency, CPU / working-set delta.
        playplay_add_test(test_libobssource "${CMAKE_SOURCE_DIR}/tests/test_libobssource.cpp")
        set_tests_properties(test_libobssource PROPERTIES TIMEOUT 300)
        if(WIN32)
            target_link_libraries(test_libobssource PRIVATE psapi winmm)
        endif()
        add_dependencies(test_libobssource stage_libobs)

        # Perf driver (not a test): plays supervisor to a real PlayPlayChannelEngine.exe over IPC, attaches the libobs
        # layer, switches scenes and samples the engine process (CPU, working set, tick times, frame age).
        add_executable(libobs_spike_driver "${CMAKE_SOURCE_DIR}/tests/libobs_spike_driver.cpp")
        target_link_libraries(libobs_spike_driver PRIVATE PlayPlayCore Qt6::Core Qt6::Network)
        if(WIN32)
            target_link_libraries(libobs_spike_driver PRIVATE psapi)
        endif()
        add_dependencies(libobs_spike_driver PlayPlayChannelEngine)
    endif()
endif()
