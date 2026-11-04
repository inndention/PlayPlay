# ---------------------------------------------------------------------------
# Lane "stability" (internal plan perf-supervisor-app.md W0-W4, W17, W19; internal plan perf-engine.md PE-01, PE-02):
# no playout process may block on console output, and every process reports its own stalls.
#
# UNCONDITIONAL on purpose: playout/core/logsink.cpp (listed in CMakeLists.txt) calls asynclog, so every tree, the
# live production build included, must compile these sources into PlayPlayCore or the Supervisor and engine fail
# to link. The lane's core files are globbed by name, so later slices (processlaunch, loopwatchdog, healthendpoint,
# minidump) only add files.
# ---------------------------------------------------------------------------
file(GLOB _pp_stb_core CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/playout/core/*.cpp" "${CMAKE_SOURCE_DIR}/playout/core/*.h")
list(FILTER _pp_stb_core INCLUDE REGEX "/(asynclog|processlaunch|loopwatchdog|healthendpoint|minidump)\\.(cpp|h)$")
target_sources(PlayPlayCore PRIVATE ${_pp_stb_core})
# STB-4: the watchdog's /healthz listener (Winsock) and its process counters (psapi). dbghelp is loaded at run time
# from System32 (minidump.cpp), so nothing links it.
if(WIN32)
    target_link_libraries(PlayPlayCore PUBLIC ws2_32 psapi)
endif()

# STB-3 (W3): the operator app is a GUI-subsystem program. As a CONSOLE program (CMakeLists.txt:135) every start from
# Explorer or a start script gave it a Windows Terminal tab (defterm), and before STB-2 the Supervisor and the engines
# shared that console: the 10-24 / 10-25 freezes. Fragments are included after CMakeLists.txt:135, so this wins. Qt
# supplies WinMain: Qt6::Core links Qt6::EntryPointPrivate (lib/Qt6EntryPoint.lib) for WIN32_EXECUTABLE targets, and it
# calls our main(), so main.cpp is unchanged. Without a console Qt's default handler writes to OutputDebugString instead
# of stderr; the app's own Logger file (playplay_*.log) is unaffected. Supervisor and engines stay console programs:
# they run with a hidden console of their own (processlaunch, STB-2) and the Supervisor's service mode needs none.
set_target_properties(PlayPlay PROPERTIES WIN32_EXECUTABLE TRUE)
# PDBs for the Supervisor and the engine, as the app has had since CMakeLists.txt:139-143: crash dumps and the watchdog's
# stack samples (STB-4) need symbols. /Zi also goes on the three static libraries they are made of (the engine is
# almost all PlayPlayEngine), or their code would only have public names and no lines. /Zi does not change code
# generation; /DEBUG would switch the linker to /OPT:NOREF,NOICF, so /OPT:REF,ICF keeps today's Release binaries.
# Appended (never set_target_properties, which would replace /W3 /MP /utf-8 ... from add_compile_options).
if(MSVC)
    foreach(_pp_stb_t PlayPlaySupervisor PlayPlayChannelEngine PlayPlayCore PlayPlaySchedule PlayPlayEngine)
        set_property(TARGET ${_pp_stb_t} APPEND PROPERTY COMPILE_OPTIONS /Zi)
    endforeach()
    foreach(_pp_stb_t PlayPlaySupervisor PlayPlayChannelEngine)
        set_property(TARGET ${_pp_stb_t} APPEND PROPERTY LINK_OPTIONS /DEBUG /OPT:REF /OPT:ICF)
    endforeach()
endif()

# Tests: one Qt Test executable per lane test file that exists. Helpers (tests/helpers/<name>_helper.cpp) are small
# executables the tests spawn; they stay out of "all", every lane test depends on them and gets its path as
# PP_<NAME>_HELPER (e.g. PP_LOGFLOOD_HELPER).
if(PLAYPLAY_BUILD_TESTS AND COMMAND playplay_add_schedule_test)
    # Wildcard globs filtered to the lane's names (other lanes keep their own files in tests/ and tests/helpers/).
    file(GLOB _pp_stb_helpers CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tests/helpers/*_helper.cpp")
    list(FILTER _pp_stb_helpers INCLUDE REGEX "/(logflood|console_probe|hang)_helper\\.cpp$")
    set(_pp_stb_helper_targets "")
    foreach(_pp_stb_src IN LISTS _pp_stb_helpers)
        get_filename_component(_pp_stb_name "${_pp_stb_src}" NAME_WE)
        add_executable(${_pp_stb_name} EXCLUDE_FROM_ALL "${_pp_stb_src}")
        target_link_libraries(${_pp_stb_name} PRIVATE PlayPlayCore)
        list(APPEND _pp_stb_helper_targets ${_pp_stb_name})
    endforeach()
    # STB-4: test_loopwatchdog checks that the hang stack sample names hang_helper's blocking function and line.
    if(TARGET hang_helper AND MSVC)
        target_compile_options(hang_helper PRIVATE /Zi)
        target_link_options(hang_helper PRIVATE /DEBUG /OPT:REF /OPT:ICF)
    endif()

    file(GLOB _pp_stb_tests CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tests/test_*.cpp")
    list(FILTER _pp_stb_tests INCLUDE REGEX "/test_(asynclog|processlaunch|loopwatchdog|timeauthority)\\.cpp$")
    foreach(_pp_stb_src IN LISTS _pp_stb_tests)
        get_filename_component(_pp_stb_test "${_pp_stb_src}" NAME_WE)
        playplay_add_schedule_test(${_pp_stb_test} "${_pp_stb_src}")
        foreach(_pp_stb_h IN LISTS _pp_stb_helper_targets)
            string(TOUPPER "${_pp_stb_h}" _pp_stb_H)
            string(REGEX REPLACE "_HELPER$" "" _pp_stb_H "${_pp_stb_H}")
            target_compile_definitions(${_pp_stb_test} PRIVATE PP_${_pp_stb_H}_HELPER="$<TARGET_FILE:${_pp_stb_h}>")
            add_dependencies(${_pp_stb_test} ${_pp_stb_h})
        endforeach()
    endforeach()
    # STB-4: a 12 s hang, two rate-limit runs, a crash and an abort run one after another (about 50 s).
    if(TARGET test_loopwatchdog)
        set_tests_properties(test_loopwatchdog PROPERTIES TIMEOUT 180)
    endif()
    # test_processlaunch also checks the two spawn sites in the source tree (STB-2).
    if(TARGET test_processlaunch)
        target_compile_definitions(test_processlaunch PRIVATE PP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
        # STB-3: it reads the PE headers and PDBs of the three programs. Paths only, no build dependency: building the
        # test must not drag in the whole app; a missing exe is a skip, a stale one fails (rebuild it).
        target_compile_definitions(test_processlaunch PRIVATE
            PP_APP_EXE="$<TARGET_FILE:PlayPlay>"
            PP_SUPERVISOR_EXE="$<TARGET_FILE:PlayPlaySupervisor>"
            PP_ENGINE_EXE="$<TARGET_FILE:PlayPlayChannelEngine>")
        if(WIN32)
            target_link_libraries(test_processlaunch PRIVATE dbghelp)
        endif()
    endif()
endif()
