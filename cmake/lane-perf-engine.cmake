# Lane perf-engine (internal plan perf-engine.md, internal plan lane-plan.json): channel-engine performance work.
# Included by the top-level CMakeLists.txt after tests/ (so playplay_add_test exists). Rules: add to existing targets only
# through target_sources / target_link_libraries / set_target_properties; everything here must also configure and
# compile in the LIVE build tree (it includes every lane fragment), so new executables stay EXCLUDE_FROM_ALL and
# engine changes keep today's behaviour by default.

# --- New engine sources from later perf-engine slices --------------------------------------------------------------
# PlayPlayEngine lists its sources explicitly (CMakeLists.txt). A .cpp that a perf-engine slice adds under
# playout/engine/ joins the library through this glob, so later slices never edit this file. engine_main.cpp and
# supervisorlink.* belong to the perf-supervisor lane (and to the PlayPlayChannelEngine executable), never to the library.
get_target_property(_pe_engine_listed PlayPlayEngine SOURCES)
set(_pe_engine_known "")
foreach(_s IN LISTS _pe_engine_listed)
    get_filename_component(_abs "${_s}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
    list(APPEND _pe_engine_known "${_abs}")
endforeach()
file(GLOB_RECURSE _pe_engine_found CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/playout/engine/*.cpp")
foreach(_s IN LISTS _pe_engine_found)
    get_filename_component(_name "${_s}" NAME)
    if(_name STREQUAL "engine_main.cpp" OR _name MATCHES "^supervisorlink\\.")
        continue()
    endif()
    # libobssource.cpp includes libobs's obs.h: only cmake/lane-libobs.cmake (PLAYPLAY_WITH_LIBOBS=ON) may add it.
    if(_name MATCHES "^libobssource\\.")
        continue()
    endif()
    list(FIND _pe_engine_known "${_s}" _idx)
    if(_idx EQUAL -1)
        message(STATUS "lane perf-engine: adding ${_s} to PlayPlayEngine")
        target_sources(PlayPlayEngine PRIVATE "${_s}")
    endif()
endforeach()

if(PLAYPLAY_BUILD_TESTS)
    # --- bench_engine (PE-23): encode / template / composite_cg / livesource_lock / ndi_send rows ---------------------
    # Numbers with PLAYPLAY_BENCH=1, a short smoke test otherwise. EXCLUDE_FROM_ALL: built by name only
    # (wrapper: -Targets bench_engine), so a live-tree build never compiles or runs it.
    if(NOT TARGET bench_engine)
        playplay_add_test(bench_engine "${CMAKE_SOURCE_DIR}/tests/bench_engine.cpp")
        set_target_properties(bench_engine PROPERTIES EXCLUDE_FROM_ALL TRUE)
        set_tests_properties(bench_engine PROPERTIES TIMEOUT 300)
        if(WIN32)
            target_link_libraries(bench_engine PRIVATE psapi winmm)
        endif()
    endif()

    # --- Unit tests named by later perf-engine slices: registered as soon as the file exists ------------------------
    # EXCLUDE_FROM_ALL like bench_engine: a half-written test from a slice in progress must never break a full
    # (stage_runtime) build of the live tree; the slices build them by name through the wrapper.
    foreach(_t IN ITEMS test_livesource_nonblocking test_tickprofiler test_template_cache)
        file(GLOB _pe_src CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tests/${_t}.cpp")
        if(_pe_src AND NOT TARGET ${_t})
            playplay_add_test(${_t} ${_pe_src})
            set_target_properties(${_t} PROPERTIES EXCLUDE_FROM_ALL TRUE)
        endif()
    endforeach()
    # PE-07: the template cache test also runs on the windows QPA, whose DirectWrite text is what the live engine draws
    # (the default offscreen QPA renders with FreeType and, on this box, without any font files).
    if(WIN32 AND TARGET test_template_cache AND NOT TEST test_template_cache_directwrite)
        add_test(NAME test_template_cache_directwrite COMMAND test_template_cache)
        set_tests_properties(test_template_cache_directwrite PROPERTIES TIMEOUT 120
            ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1;QT_QPA_PLATFORM=windows")
    endif()

    # --- Isolated-engine harness guard self-test (PE-23): the script must refuse live names / non-standalone runs ---
    find_program(PLAYPLAY_PWSH NAMES pwsh pwsh.exe)
    if(PLAYPLAY_PWSH AND EXISTS "${CMAKE_SOURCE_DIR}/scripts/perf/run-isolated-engine.ps1")
        add_test(NAME perf_isolated_engine_guard
                 COMMAND "${PLAYPLAY_PWSH}" -NoProfile -NonInteractive -File "${CMAKE_SOURCE_DIR}/scripts/perf/run-isolated-engine.ps1" -SelfTest)
        set_tests_properties(perf_isolated_engine_guard PROPERTIES TIMEOUT 60)
    endif()
endif()
