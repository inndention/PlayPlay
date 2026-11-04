# ---------------------------------------------------------------------------
# Lane "stability", STB-6 (internal plan perf-supervisor-app.md §5.3, W19): PlayPlayWatchdog.exe, the external report-only
# watchdog. It polls /healthz (STB-4) of the Supervisor and the engines and, on HANG, writes an incident bundle and a dump
# of that one process; restarting a hung Supervisor is opt-in. See tools/ppwatch/README.md.
#   PlayPlayWatchdogKit  static library: every tools/ppwatch/*.cpp except main.cpp (probe + tracker, system snapshots,
#                        incident bundle), linked by the exe and the test.
#   PlayPlayWatchdog     the exe (console program that never writes to the console while watching).
#   test_ppwatch         unit checks plus end-to-end runs of the exe against tests/helpers/hang_helper (lane-stability
#                        defines that target; fragments are included in name order, so it is referenced by name only,
#                        which CMake resolves at generate time).
# Nothing here is added to PlayPlay / PlayPlaySupervisor / PlayPlayChannelEngine, and every target is EXCLUDE_FROM_ALL:
# a full build of any tree (the live one included) compiles nothing from this fragment.
# ---------------------------------------------------------------------------
include_guard(GLOBAL)

file(GLOB _pp_ppwatch_sources CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tools/ppwatch/*.cpp" "${CMAKE_SOURCE_DIR}/tools/ppwatch/*.h")
list(FILTER _pp_ppwatch_sources EXCLUDE REGEX "/tools/ppwatch/main\\.cpp$")
add_library(PlayPlayWatchdogKit STATIC EXCLUDE_FROM_ALL ${_pp_ppwatch_sources})
target_include_directories(PlayPlayWatchdogKit PUBLIC "${CMAKE_SOURCE_DIR}")
target_link_libraries(PlayPlayWatchdogKit PUBLIC Qt6::Core Qt6::Network)
if(WIN32)
    # Winsock (probe), iphlpapi (GetExtendedTcpTable), psapi (module list), shell32 (CommandLineToArgvW). dbghelp is
    # loaded at run time from System32 (incident.cpp), so nothing links it.
    target_link_libraries(PlayPlayWatchdogKit PUBLIC ws2_32 iphlpapi psapi shell32)
endif()
set_target_properties(PlayPlayWatchdogKit PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

add_executable(PlayPlayWatchdog EXCLUDE_FROM_ALL "${CMAKE_SOURCE_DIR}/tools/ppwatch/main.cpp")
target_link_libraries(PlayPlayWatchdog PRIVATE PlayPlayWatchdogKit)
set_target_properties(PlayPlayWatchdog PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
if(MSVC)
    # A PDB next to the tool, like the Supervisor and engine (STB-3); /OPT:REF,ICF keep the Release image as without /DEBUG.
    target_compile_options(PlayPlayWatchdogKit PRIVATE /Zi)
    target_compile_options(PlayPlayWatchdog PRIVATE /Zi)
    target_link_options(PlayPlayWatchdog PRIVATE /DEBUG /OPT:REF /OPT:ICF)
endif()

if(PLAYPLAY_BUILD_TESTS AND COMMAND playplay_add_schedule_test AND EXISTS "${CMAKE_SOURCE_DIR}/tests/test_ppwatch.cpp"
   AND EXISTS "${CMAKE_SOURCE_DIR}/tests/helpers/hang_helper.cpp")
    playplay_add_schedule_test(test_ppwatch "${CMAKE_SOURCE_DIR}/tests/test_ppwatch.cpp")
    target_link_libraries(test_ppwatch PRIVATE PlayPlayWatchdogKit)
    add_dependencies(test_ppwatch PlayPlayWatchdog hang_helper)
    target_compile_definitions(test_ppwatch PRIVATE
        PP_PPWATCH_EXE="$<TARGET_FILE:PlayPlayWatchdog>"
        PP_HANG_HELPER="$<TARGET_FILE:hang_helper>")
    # Three end-to-end runs (hang, unresponsive, mismatch in one; restart; --once) plus units: about 45 s.
    set_tests_properties(test_ppwatch PROPERTIES TIMEOUT 180)
endif()
