# ---------------------------------------------------------------------------
# AN-FS C++ port: anfs.exe (modes off | peer | server), its foundation, modules and Qt Tests (docs/plans/anfs-cpp.md).
#
# Two ways in:
#   standalone   cmake -S tools/anfs -B build-anfs -G Ninja      (development; everything builds, tests ON)
#   PlayPlay     cmake/lane-anfs.cmake add_subdirectory(... EXCLUDE_FROM_ALL) when PLAYPLAY_WITH_ANFS; targets build
#                only by name, output in <build>/anfs, tests only with PLAYPLAY_ANFS_TESTS (default OFF)
#
# Later slices and lanes add code without editing this file (every glob is CONFIGURE_DEPENDS):
#   - src/core/*.cpp and src/mod/<module>/*.cpp  -> OBJECT library anfs_obj (an object library keeps every static
#     ANFS_MODULE registrar; a static library would let the linker drop them)
#   - src/main.cpp                               -> anfs.exe
#   - test/sim/*.cpp                             -> anfs_sim.exe (when present)
#   - tests/anfs/test_anfs_<id>.cpp              -> one Qt Test executable + ctest test each (label "anfs")
#   - tests/anfs/anfs_testkit.{h,cpp}            -> static helper library every AN-FS test links (when present)
#   - third_party/sqlite/sqlite3.c               -> anfs_sqlite (vendored amalgamation, when present)
#   - *.qrc, ui*/*.qrc, and ui*/ folders no qrc names -> embedded in anfs_obj by AUTORCC with --no-compress
#
# Secrets: the Node agent token is never in the repository. ANFS_COMPAT_TOKEN_FILE may name an untracked file
# OUTSIDE the repo; CMake then writes <build>/anfs_generated/compat_token.h (build dir only). test_anfs_secrets
# guards tools/anfs and tests/anfs.
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.21)

set(ANFS_VERSION 0.1.0)   # keep in step with src/core/version.h
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(anfs VERSION ${ANFS_VERSION} LANGUAGES CXX)
    set(ANFS_STANDALONE ON)
else()
    set(ANFS_STANDALONE OFF)
endif()

get_filename_component(ANFS_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(ANFS_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(ANFS_TEST_DIR "${ANFS_REPO_ROOT}/tests/anfs")
set(ANFS_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/anfs_generated")
# Executables of this directory land next to each other: build-anfs\ standalone, <build>\anfs\ in the PlayPlay tree
# (never next to the live PlayPlay executables). The variable is directory-scoped, so the parent is unaffected.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

if(ANFS_STANDALONE)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
    endif()
    if(MSVC)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        add_compile_options(/W3 /utf-8 /Zc:__cplusplus /permissive-)
        add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    endif()
    if(DEFINED ENV{CONDA_PREFIX})
        file(TO_CMAKE_PATH "$ENV{CONDA_PREFIX}/Library" _anfs_default_qt_root)
    else()
        file(TO_CMAKE_PATH "$ENV{USERPROFILE}/Miniconda3/Library" _anfs_default_qt_root)
    endif()
    set(PLAYPLAY_QT_ROOT "${_anfs_default_qt_root}" CACHE PATH "Qt 6 prefix (the conda Library folder)")
    list(APPEND CMAKE_PREFIX_PATH "${PLAYPLAY_QT_ROOT}/lib/cmake" "${PLAYPLAY_QT_ROOT}")
    option(ANFS_BUILD_TESTS "Build the AN-FS Qt Tests (tests/anfs)" ON)
    # Lets anfs.exe and the tests find Qt plugins (TLS backends for QSslServer) when run from the build folder.
    file(WRITE "${CMAKE_BINARY_DIR}/qt.conf" "[Paths]\nPrefix=${PLAYPLAY_QT_ROOT}\nPlugins=lib/qt6/plugins\n")
else()
    set(ANFS_BUILD_TESTS ${PLAYPLAY_ANFS_TESTS})
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Network)

# ---- third-party: SQLite (vendored amalgamation, ANFS-F3) and zlib (conda static, for the ZIP writer) -------------
set(ANFS_SQLITE "vendored" CACHE STRING "SQLite for anfs: vendored (third_party/sqlite) or conda (PLAYPLAY_QT_ROOT)")
set_property(CACHE ANFS_SQLITE PROPERTY STRINGS vendored conda)
set(_anfs_sqlite_c "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite/sqlite3.c")
set(ANFS_HAVE_SQLITE OFF)
if(ANFS_SQLITE STREQUAL "vendored" AND EXISTS "${_anfs_sqlite_c}")
    enable_language(C)
    add_library(anfs_sqlite STATIC "${_anfs_sqlite_c}")
    target_include_directories(anfs_sqlite PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite")
    # Plain C: no moc/uic/rcc (the PlayPlay root turns AUTOMOC on for every target, which added a C++ moc unit).
    set_target_properties(anfs_sqlite PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    # The C# port's native build (anfs\native\build-native.cmd) uses THREADSAFE=1, ENABLE_FTS5, ENABLE_JSON1 and
    # DQS=0. Two extras from SQLite's recommended options: DEFAULT_WAL_SYNCHRONOUS=1 (NORMAL in WAL mode, which
    # core/db sets anyway) and OMIT_DEPRECATED (drops APIs nobody here calls). Neither changes the file format, so
    # the C# and C++ binaries still share one anfs.db.
    target_compile_definitions(anfs_sqlite PRIVATE SQLITE_THREADSAFE=1 SQLITE_ENABLE_FTS5 SQLITE_ENABLE_JSON1
        SQLITE_DQS=0 SQLITE_DEFAULT_WAL_SYNCHRONOUS=1 SQLITE_OMIT_DEPRECATED)
    if(MSVC)
        # No warnings for the amalgamation. add_compile_options (here and in the PlayPlay root) already gave the
        # target /W3, and cl answers /W3 plus /W0 with D9025, so the inherited warning level is taken out first.
        get_target_property(_anfs_sqlite_opts anfs_sqlite COMPILE_OPTIONS)
        if(_anfs_sqlite_opts)
            list(FILTER _anfs_sqlite_opts EXCLUDE REGEX "^[/-](W[0-4]|Wall)$")
            set_property(TARGET anfs_sqlite PROPERTY COMPILE_OPTIONS "${_anfs_sqlite_opts}")
        endif()
        target_compile_options(anfs_sqlite PRIVATE /W0)
    endif()
    set(ANFS_HAVE_SQLITE ON)
elseif(ANFS_SQLITE STREQUAL "conda" AND EXISTS "${PLAYPLAY_QT_ROOT}/lib/sqlite3.lib")
    # Fallback: conda sqlite3 3.50.4 (FTS5 verified); needs sqlite3.dll at run time. Only the header is exposed.
    configure_file("${PLAYPLAY_QT_ROOT}/include/sqlite3.h" "${ANFS_GEN_DIR}/sqlite/sqlite3.h" COPYONLY)
    add_library(anfs_sqlite INTERFACE)
    target_include_directories(anfs_sqlite INTERFACE "${ANFS_GEN_DIR}/sqlite")
    target_link_libraries(anfs_sqlite INTERFACE "${PLAYPLAY_QT_ROOT}/lib/sqlite3.lib")
    set(ANFS_HAVE_SQLITE ON)
endif()

set(ANFS_HAVE_ZLIB OFF)
if(EXISTS "${PLAYPLAY_QT_ROOT}/lib/zlibstatic.lib" AND EXISTS "${PLAYPLAY_QT_ROOT}/include/zlib.h")
    # Copy just zlib.h/zconf.h so the whole conda include folder never lands on the include path.
    configure_file("${PLAYPLAY_QT_ROOT}/include/zlib.h" "${ANFS_GEN_DIR}/zlib/zlib.h" COPYONLY)
    configure_file("${PLAYPLAY_QT_ROOT}/include/zconf.h" "${ANFS_GEN_DIR}/zlib/zconf.h" COPYONLY)
    add_library(anfs_zlib INTERFACE)
    target_include_directories(anfs_zlib INTERFACE "${ANFS_GEN_DIR}/zlib")
    target_link_libraries(anfs_zlib INTERFACE "${PLAYPLAY_QT_ROOT}/lib/zlibstatic.lib")
    set(ANFS_HAVE_ZLIB ON)
endif()

# ---- compat token (build dir only, never printed) -------------------------------------------------------------------
set(ANFS_COMPAT_TOKEN_FILE "" CACHE FILEPATH "Untracked file OUTSIDE the repository holding the Node agent token")
set(_anfs_token "")
if(ANFS_COMPAT_TOKEN_FILE)
    get_filename_component(_anfs_tok_abs "${ANFS_COMPAT_TOKEN_FILE}" ABSOLUTE)
    string(TOLOWER "${_anfs_tok_abs}" _anfs_tok_lower)
    string(TOLOWER "${ANFS_REPO_ROOT}/" _anfs_repo_lower)
    string(FIND "${_anfs_tok_lower}" "${_anfs_repo_lower}" _anfs_tok_in_repo)
    if(_anfs_tok_in_repo EQUAL 0)
        message(WARNING "AN-FS: ANFS_COMPAT_TOKEN_FILE is inside the repository; ignored (keep it outside the repo)")
    elseif(NOT EXISTS "${_anfs_tok_abs}")
        message(WARNING "AN-FS: ANFS_COMPAT_TOKEN_FILE does not exist; ignored")
    else()
        file(READ "${_anfs_tok_abs}" _anfs_token)
        string(STRIP "${_anfs_token}" _anfs_token)
        if(NOT _anfs_token MATCHES "^[A-Za-z0-9._~-]+$")
            message(WARNING "AN-FS: the compat token file does not hold a single plain token; ignored")
            set(_anfs_token "")
        else()
            message(STATUS "AN-FS: compat token taken from ANFS_COMPAT_TOKEN_FILE")
        endif()
    endif()
endif()
if(_anfs_token)
    set(ANFS_HAVE_COMPAT_TOKEN 1)
else()
    set(ANFS_HAVE_COMPAT_TOKEN 0)
endif()
file(CONFIGURE OUTPUT "${ANFS_GEN_DIR}/compat_token.h" CONTENT
"// Generated by tools/anfs/CMakeLists.txt from ANFS_COMPAT_TOKEN_FILE. Build folder only: never copy it into the repo.
#pragma once
#define ANFS_HAVE_COMPAT_TOKEN @ANFS_HAVE_COMPAT_TOKEN@
namespace anfs { inline constexpr char kCompatToken[] = \"@_anfs_token@\"; }
" @ONLY)
unset(_anfs_token)

# ---- embedded UI (anfs-cpp.md 2.2): rcc --no-compress, so served bytes equal the source bytes --------------------------
# Compiled into anfs_obj by AUTORCC, with no CMake edit by the UI lanes:
#   - every tools/anfs/*.qrc (anfs_ui.qrc, srv-ui) and tools/anfs/ui*/*.qrc (a folder's own qrc);
#   - a tools/anfs/ui*/ folder that no qrc names (no qrc of its own, and no root qrc entry "<folder>/...") gets a
#     generated one in the build dir: every file in it as :/<folder>/<relative path>, the path a root qrc entry
#     <file><folder>/...</file> under prefix "/" gives. So the peer pages (ui-peer/, pfs-setup) are embedded as soon
#     as they exist, and stop being generated once a qrc names them.
# test_anfs_secrets checks that every embedded file is stored uncompressed and equals its source.
file(GLOB _anfs_qrc CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.qrc" "${CMAKE_CURRENT_SOURCE_DIR}/ui*/*.qrc")
set(_anfs_qrc_text "")
foreach(_anfs_q IN LISTS _anfs_qrc)
    file(READ "${_anfs_q}" _anfs_t)
    string(APPEND _anfs_qrc_text "${_anfs_t}")
    # An edited qrc may start or stop naming a folder: re-configure so the generated qrc follows.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_anfs_q}")
endforeach()
file(GLOB _anfs_ui_dirs LIST_DIRECTORIES true CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ui*")
foreach(_anfs_d IN LISTS _anfs_ui_dirs)
    get_filename_component(_anfs_n "${_anfs_d}" NAME)
    if(NOT IS_DIRECTORY "${_anfs_d}" OR NOT _anfs_n MATCHES "^ui[A-Za-z0-9_-]*$")
        continue()
    endif()
    file(GLOB _anfs_own "${_anfs_d}/*.qrc")
    if(_anfs_own OR _anfs_qrc_text MATCHES "[>/]${_anfs_n}/")
        continue()
    endif()
    file(GLOB_RECURSE _anfs_files CONFIGURE_DEPENDS RELATIVE "${_anfs_d}" "${_anfs_d}/*")
    list(SORT _anfs_files)
    set(_anfs_xml "<!DOCTYPE RCC>\n<!-- Generated by tools/anfs/CMakeLists.txt from ${_anfs_n}/ (build dir only). -->\n<RCC version=\"1.0\">\n<qresource prefix=\"/${_anfs_n}\">\n")
    set(_anfs_count 0)
    foreach(_anfs_f IN LISTS _anfs_files)
        if(_anfs_f MATCHES "(^|/)\\.")
            continue()                                  # .gitkeep and other dot files are not UI
        endif()
        if(_anfs_f MATCHES "[<>&\"]")
            message(WARNING "AN-FS: ${_anfs_n}/${_anfs_f} is not embedded (name has XML special characters)")
            continue()
        endif()
        string(APPEND _anfs_xml "<file alias=\"${_anfs_f}\">${_anfs_d}/${_anfs_f}</file>\n")
        math(EXPR _anfs_count "${_anfs_count} + 1")
    endforeach()
    string(APPEND _anfs_xml "</qresource>\n</RCC>\n")
    if(_anfs_count GREATER 0)
        string(MAKE_C_IDENTIFIER "anfs_gen_${_anfs_n}" _anfs_id)
        # configure_file rewrites only on a change, so an unchanged folder does not re-run rcc.
        file(WRITE "${ANFS_GEN_DIR}/qrc/${_anfs_id}.qrc.in" "${_anfs_xml}")
        configure_file("${ANFS_GEN_DIR}/qrc/${_anfs_id}.qrc.in" "${ANFS_GEN_DIR}/qrc/${_anfs_id}.qrc" COPYONLY)
        list(APPEND _anfs_qrc "${ANFS_GEN_DIR}/qrc/${_anfs_id}.qrc")
        message(STATUS "AN-FS: ${_anfs_n}/ embedded through a generated qrc (${_anfs_count} files)")
    endif()
endforeach()

# ---- anfs_obj: foundation + modules ---------------------------------------------------------------------------------
file(GLOB _anfs_core_sources CONFIGURE_DEPENDS "${ANFS_SRC_DIR}/core/*.cpp" "${ANFS_SRC_DIR}/core/*.h")
file(GLOB _anfs_iface_headers CONFIGURE_DEPENDS "${ANFS_SRC_DIR}/iface/*.h")
file(GLOB _anfs_mod_sources CONFIGURE_DEPENDS "${ANFS_SRC_DIR}/mod/*/*.cpp" "${ANFS_SRC_DIR}/mod/*/*.h")
add_library(anfs_obj OBJECT ${_anfs_core_sources} ${_anfs_iface_headers} ${_anfs_mod_sources} ${_anfs_qrc})
target_include_directories(anfs_obj PUBLIC "${ANFS_SRC_DIR}" "${ANFS_GEN_DIR}")
target_compile_definitions(anfs_obj PUBLIC
    ANFS_HAVE_SQLITE=$<BOOL:${ANFS_HAVE_SQLITE}> ANFS_HAVE_ZLIB=$<BOOL:${ANFS_HAVE_ZLIB}>
    _WIN32_WINNT=0x0A00 WINVER=0x0A00 UNICODE _UNICODE)
target_link_libraries(anfs_obj PUBLIC Qt6::Core Qt6::Network
    ws2_32 mswsock winhttp bcrypt crypt32 iphlpapi advapi32 shell32 ole32)
if(TARGET anfs_sqlite)
    target_link_libraries(anfs_obj PUBLIC anfs_sqlite)
endif()
if(TARGET anfs_zlib)
    target_link_libraries(anfs_obj PUBLIC anfs_zlib)
endif()
set_target_properties(anfs_obj PROPERTIES AUTOMOC OFF AUTOUIC OFF)
if(_anfs_qrc)
    # The rcc output is linked into every consumer (object library) and registers itself from a static initializer.
    set_target_properties(anfs_obj PROPERTIES AUTORCC ON AUTORCC_OPTIONS "--no-compress")
else()
    set_target_properties(anfs_obj PROPERTIES AUTORCC OFF)
endif()

add_executable(anfs "${ANFS_SRC_DIR}/main.cpp")
target_link_libraries(anfs PRIVATE anfs_obj)
set_target_properties(anfs PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)

file(GLOB _anfs_sim_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test/sim/*.cpp")
if(_anfs_sim_sources)
    add_executable(anfs_sim ${_anfs_sim_sources})
    target_link_libraries(anfs_sim PRIVATE anfs_obj)
    set_target_properties(anfs_sim PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
endif()

# ---- tests ----------------------------------------------------------------------------------------------------------
if(ANFS_BUILD_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    enable_testing()

    file(GLOB _anfs_testkit_sources CONFIGURE_DEPENDS "${ANFS_TEST_DIR}/anfs_testkit.cpp" "${ANFS_TEST_DIR}/anfs_testkit.h")
    if(_anfs_testkit_sources)
        add_library(anfs_testkit STATIC ${_anfs_testkit_sources})
        target_include_directories(anfs_testkit PUBLIC "${ANFS_TEST_DIR}")
        target_link_libraries(anfs_testkit PUBLIC anfs_obj Qt6::Test Qt6::Network)
        set_target_properties(anfs_testkit PROPERTIES AUTOMOC ON)
    endif()

    # Extra command-line arguments per test (test_anfs_secrets scans the repo, optionally with the token file).
    set(ANFS_TEST_ARGS_test_anfs_secrets --root "${ANFS_REPO_ROOT}")
    if(ANFS_COMPAT_TOKEN_FILE AND ANFS_HAVE_COMPAT_TOKEN)
        list(APPEND ANFS_TEST_ARGS_test_anfs_secrets --token-file "${ANFS_COMPAT_TOKEN_FILE}")
    endif()

    file(GLOB _anfs_tests CONFIGURE_DEPENDS "${ANFS_TEST_DIR}/test_anfs_*.cpp")
    foreach(_anfs_test_src IN LISTS _anfs_tests)
        get_filename_component(_anfs_test "${_anfs_test_src}" NAME_WE)
        add_executable(${_anfs_test} "${_anfs_test_src}")
        target_link_libraries(${_anfs_test} PRIVATE anfs_obj Qt6::Test Qt6::Network)
        if(TARGET anfs_testkit)
            target_link_libraries(${_anfs_test} PRIVATE anfs_testkit)
        endif()
        set_target_properties(${_anfs_test} PROPERTIES AUTOMOC ON)
        add_dependencies(${_anfs_test} anfs)
        target_compile_definitions(${_anfs_test} PRIVATE
            ANFS_REPO_ROOT="${ANFS_REPO_ROOT}"
            ANFS_TEST_DIR="${ANFS_TEST_DIR}"
            ANFS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
            ANFS_EXE_PATH="$<TARGET_FILE:anfs>")
        add_test(NAME ${_anfs_test} COMMAND ${_anfs_test} ${ANFS_TEST_ARGS_${_anfs_test}})
        set_tests_properties(${_anfs_test} PROPERTIES LABELS anfs TIMEOUT 120
            ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1;QT_PLUGIN_PATH=${PLAYPLAY_QT_ROOT}/lib/qt6/plugins"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${PLAYPLAY_QT_ROOT}/bin")
    endforeach()
endif()

message(STATUS "AN-FS ${ANFS_VERSION}: standalone=${ANFS_STANDALONE} tests=${ANFS_BUILD_TESTS} sqlite=${ANFS_HAVE_SQLITE} zlib=${ANFS_HAVE_ZLIB} compatToken=${ANFS_HAVE_COMPAT_TOKEN}")
