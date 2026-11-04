# ---------------------------------------------------------------------------
# Lane "anfs-foundation" (internal plan anfs-cpp.md): the standalone AN-FS C++ project tools/anfs (anfs.exe, anfs_sim,
# tests/anfs). Included by the lane glob in CMakeLists.txt. It adds nothing to PlayPlay, PlayPlaySupervisor or
# PlayPlayChannelEngine; every AN-FS target is EXCLUDE_FROM_ALL (built only by name) and lands in <build>/anfs, never
# next to the live executables. Development builds use the standalone tree: cmake -S tools/anfs -B build-anfs.
#
# PLAYPLAY_WITH_ANFS defaults OFF in the live build tree (<source>/build) so the live build never even configures it,
# ON in every other tree (lane trees, scratch trees). PLAYPLAY_ANFS_TESTS (default OFF) registers the AN-FS Qt Tests
# in this tree too; they are EXCLUDE_FROM_ALL, so turn it on only where they are built before ctest runs.
# Later slices (P1: test_meshclient) add their entries below.
# ---------------------------------------------------------------------------
get_filename_component(_anfs_bin "${CMAKE_BINARY_DIR}" REALPATH)
get_filename_component(_anfs_live "${CMAKE_SOURCE_DIR}/build" REALPATH)
string(TOLOWER "${_anfs_bin}" _anfs_bin)
string(TOLOWER "${_anfs_live}" _anfs_live)
if(_anfs_bin STREQUAL _anfs_live)
    set(_anfs_default OFF)
else()
    set(_anfs_default ON)
endif()
option(PLAYPLAY_WITH_ANFS "Configure the AN-FS C++ port (tools/anfs; targets build only by name)" ${_anfs_default})
option(PLAYPLAY_ANFS_TESTS "Register the AN-FS Qt Tests (tests/anfs) in the PlayPlay tree" OFF)

if(PLAYPLAY_WITH_ANFS AND EXISTS "${CMAKE_SOURCE_DIR}/tools/anfs/CMakeLists.txt")
    add_subdirectory("${CMAKE_SOURCE_DIR}/tools/anfs" "${CMAKE_BINARY_DIR}/anfs" EXCLUDE_FROM_ALL)
endif()
