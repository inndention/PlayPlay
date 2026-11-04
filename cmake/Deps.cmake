# ---------------------------------------------------------------------------
# PlayPlay third-party dependency discovery.
#
# Every SDK root is a CMake cache variable so a different machine (or a CI
# box) can override it with -D<VAR>=... without editing this file.
# Defaults are typical install locations on Windows; an SDK that is not found is skipped (NDI, DekTec, DeckLink) or
# reported (Qt, FFmpeg).
# ---------------------------------------------------------------------------

# Qt 6.9 from a conda environment (conda install qt6-main ...): the prefix is that environment's Library folder, taken from
# CONDA_PREFIX when an environment is active. Any other Qt 6 install: pass -DPLAYPLAY_QT_ROOT=<prefix>.
if(DEFINED ENV{CONDA_PREFIX})
    file(TO_CMAKE_PATH "$ENV{CONDA_PREFIX}/Library" _pp_qt_default)
else()
    set(_pp_qt_default "C:/Miniconda3/Library")
endif()
set(PLAYPLAY_QT_ROOT "${_pp_qt_default}" CACHE PATH
    "Qt6 prefix (contains lib/cmake/Qt6; the conda layout keeps plugins in lib/qt6/plugins)")
# FFmpeg 9 (BtbN win64 GPL shared build: mpeg2video/mpeg2_qsv, h264_qsv/hevc_qsv, libx264, AC-3, SRT, RIST).
# Kept apart from the conda prefix on purpose: conda's include/ still holds its own (older) FFmpeg headers.
set(PLAYPLAY_FFMPEG_ROOT "C:/SDKs/ffmpeg-9.0-win64-gpl-shared" CACHE PATH
    "FFmpeg shared-dev prefix (include/libav*, lib/av*.lib, bin/av*.dll, bin/ffmpeg.exe)")
set(PLAYPLAY_NDI_ROOT "C:/Program Files/NDI/NDI 6 SDK" CACHE PATH
    "NDI SDK root (Include/Processing.NDI.Lib.h, Lib/x64/*.lib)")
set(PLAYPLAY_DEKTEC_ROOT "C:/Program Files (x86)/DekTec/SDKs/WinSDK/DTAPI" CACHE PATH
    "DekTec DTAPI SDK root (Include/DTAPI.h, Lib/VC15/*.lib)")

option(PLAYPLAY_WITH_NDI    "Build NDI output support if the SDK is found"    ON)
option(PLAYPLAY_WITH_DEKTEC "Build DekTec ASI/SDI support if the SDK is found" ON)
set(PLAYPLAY_DECKLINK_ROOT "C:/SDKs/Blackmagic DeckLink SDK 14.4/Win/include" CACHE PATH
    "Blackmagic DeckLink SDK include dir (DeckLinkAPI.idl)")
option(PLAYPLAY_WITH_DECKLINK "Build Blackmagic DeckLink SDI output if the SDK is found" ON)
option(PLAYPLAY_BUILD_TESTS "Build unit tests"                                ON)

# ---------------------------------------------------------------------------
# Qt6
# ---------------------------------------------------------------------------
list(APPEND CMAKE_PREFIX_PATH "${PLAYPLAY_QT_ROOT}/lib/cmake")
find_package(Qt6 REQUIRED COMPONENTS
    Core Gui Widgets Network Sql Qml Quick QuickControls2 WebSockets Concurrent)
if(PLAYPLAY_BUILD_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)
endif()
message(STATUS "Qt6 ${Qt6_VERSION} from ${Qt6_DIR}")

# ---------------------------------------------------------------------------
# FFmpeg (shared dev libraries) -> imported target FFmpeg::FFmpeg
# ---------------------------------------------------------------------------
set(_ff_inc "${PLAYPLAY_FFMPEG_ROOT}/include")
set(_ff_lib "${PLAYPLAY_FFMPEG_ROOT}/lib")
set(_ff_bin "${PLAYPLAY_FFMPEG_ROOT}/bin")

if(NOT EXISTS "${_ff_inc}/libavcodec/avcodec.h")
    message(FATAL_ERROR "FFmpeg headers not found under ${_ff_inc}. Set -DPLAYPLAY_FFMPEG_ROOT=...")
endif()

add_library(FFmpeg::FFmpeg INTERFACE IMPORTED)
target_include_directories(FFmpeg::FFmpeg INTERFACE "${_ff_inc}")
foreach(_c avformat avcodec avutil swscale avfilter swresample avdevice)
    if(NOT EXISTS "${_ff_lib}/${_c}.lib")
        message(FATAL_ERROR "FFmpeg import library missing: ${_ff_lib}/${_c}.lib")
    endif()
    target_link_libraries(FFmpeg::FFmpeg INTERFACE "${_ff_lib}/${_c}.lib")
endforeach()
# Runtime DLLs to copy next to the executables (globbed so the ABI suffix does not matter).
file(GLOB PLAYPLAY_FFMPEG_RUNTIME_DLLS
    "${_ff_bin}/avcodec-*.dll" "${_ff_bin}/avformat-*.dll" "${_ff_bin}/avutil-*.dll"
    "${_ff_bin}/swscale-*.dll" "${_ff_bin}/swresample-*.dll" "${_ff_bin}/avfilter-*.dll"
    "${_ff_bin}/avdevice-*.dll" "${_ff_bin}/postproc-*.dll")
# The command-line tools of the same build (ingest probe, thumbnails, mux encoders, spot renders) are staged next
# to the executables, where the Supervisor looks first, so CLI work and the linked libraries are the same FFmpeg.
set(PLAYPLAY_FFMPEG_TOOLS)
foreach(_t ffmpeg.exe ffprobe.exe)
    if(EXISTS "${_ff_bin}/${_t}")
        list(APPEND PLAYPLAY_FFMPEG_TOOLS "${_ff_bin}/${_t}")
    endif()
endforeach()
file(STRINGS "${_ff_inc}/libavcodec/version_major.h" _ff_major REGEX "#define LIBAVCODEC_VERSION_MAJOR")
message(STATUS "FFmpeg from ${PLAYPLAY_FFMPEG_ROOT} (${_ff_major})")

# ---------------------------------------------------------------------------
# NDI SDK (optional) -> imported target NDI::NDI, define HAS_NDI=1
# ---------------------------------------------------------------------------
set(PLAYPLAY_HAVE_NDI OFF)
if(PLAYPLAY_WITH_NDI AND EXISTS "${PLAYPLAY_NDI_ROOT}/Include/Processing.NDI.Lib.h")
    set(_ndi_lib "${PLAYPLAY_NDI_ROOT}/Lib/x64/Processing.NDI.Lib.x64.lib")
    if(EXISTS "${_ndi_lib}")
        add_library(NDI::NDI INTERFACE IMPORTED)
        target_include_directories(NDI::NDI INTERFACE "${PLAYPLAY_NDI_ROOT}/Include")
        target_link_libraries(NDI::NDI INTERFACE "${_ndi_lib}")
        target_compile_definitions(NDI::NDI INTERFACE HAS_NDI=1)
        set(PLAYPLAY_HAVE_NDI ON)
        set(PLAYPLAY_NDI_RUNTIME_DLL "${PLAYPLAY_NDI_ROOT}/Bin/x64/Processing.NDI.Lib.x64.dll")
        message(STATUS "NDI SDK: ${PLAYPLAY_NDI_ROOT}")
    else()
        message(WARNING "NDI header found but x64 import lib missing: ${_ndi_lib}")
    endif()
else()
    message(STATUS "NDI SDK not found/disabled (PLAYPLAY_NDI_ROOT=${PLAYPLAY_NDI_ROOT})")
endif()

# ---------------------------------------------------------------------------
# DekTec DTAPI (optional, static lib) -> imported target DekTec::DTAPI, HAS_DEKTEC=1
# DTAPI.h auto-selects the matching .lib via #pragma comment(lib,...) based on
# _DEBUG/_DLL, so we only need the include dir + link directory + Win32 deps.
# ---------------------------------------------------------------------------
set(PLAYPLAY_HAVE_DEKTEC OFF)
if(PLAYPLAY_WITH_DEKTEC AND EXISTS "${PLAYPLAY_DEKTEC_ROOT}/Include/DTAPI.h")
    set(_dt_libdir "${PLAYPLAY_DEKTEC_ROOT}/Lib/VC15")
    if(EXISTS "${_dt_libdir}/DTAPI64MD.lib")
        add_library(DekTec::DTAPI INTERFACE IMPORTED)
        target_include_directories(DekTec::DTAPI INTERFACE "${PLAYPLAY_DEKTEC_ROOT}/Include")
        target_link_directories(DekTec::DTAPI INTERFACE "${_dt_libdir}")
        target_link_libraries(DekTec::DTAPI INTERFACE
            setupapi comctl32 wbemuuid psapi iphlpapi ws2_32)
        target_compile_definitions(DekTec::DTAPI INTERFACE HAS_DEKTEC=1)
        set(PLAYPLAY_HAVE_DEKTEC ON)
        message(STATUS "DekTec DTAPI: ${PLAYPLAY_DEKTEC_ROOT}")
    else()
        message(WARNING "DTAPI.h found but ${_dt_libdir}/DTAPI64MD.lib missing")
    endif()
else()
    message(STATUS "DekTec SDK not found/disabled (PLAYPLAY_DEKTEC_ROOT=${PLAYPLAY_DEKTEC_ROOT})")
endif()

# ---------------------------------------------------------------------------
# Blackmagic DeckLink SDK (optional). Windows ships IDL only: MIDL generates the
# COM header + GUID file at build time -> static lib DeckLink::SDK, HAS_DECKLINK=1.
# The driver (Desktop Video) is only needed at run time.
# ---------------------------------------------------------------------------
set(PLAYPLAY_HAVE_DECKLINK OFF)
if(WIN32 AND PLAYPLAY_WITH_DECKLINK AND EXISTS "${PLAYPLAY_DECKLINK_ROOT}/DeckLinkAPI.idl")
    file(GLOB _midl_hints "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64")
    list(SORT _midl_hints ORDER DESCENDING)
    find_program(PLAYPLAY_MIDL midl HINTS ${_midl_hints})
    if(PLAYPLAY_MIDL)
        set(_dl_out "${CMAKE_BINARY_DIR}/decklink")
        file(MAKE_DIRECTORY "${_dl_out}")
        file(GLOB _dl_idls "${PLAYPLAY_DECKLINK_ROOT}/*.idl")
        add_custom_command(
            OUTPUT "${_dl_out}/DeckLinkAPI_h.h" "${_dl_out}/DeckLinkAPI_i.c"
            COMMAND "${PLAYPLAY_MIDL}" /nologo /env x64 /h DeckLinkAPI_h.h /iid DeckLinkAPI_i.c
                    /tlb DeckLinkAPI.tlb /I "${PLAYPLAY_DECKLINK_ROOT}" /out "${_dl_out}"
                    "${PLAYPLAY_DECKLINK_ROOT}/DeckLinkAPI.idl"
            DEPENDS ${_dl_idls}
            COMMENT "MIDL: generating DeckLink COM interfaces"
            VERBATIM)
        add_library(DeckLinkSDK STATIC "${_dl_out}/DeckLinkAPI_i.c" "${_dl_out}/DeckLinkAPI_h.h")
        # The project is C++ only: build the MIDL GUID file as C++ (it is valid C++), no Qt moc.
        set_source_files_properties("${_dl_out}/DeckLinkAPI_i.c" PROPERTIES LANGUAGE CXX)
        set_target_properties(DeckLinkSDK PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF LINKER_LANGUAGE CXX)
        target_include_directories(DeckLinkSDK PUBLIC "${_dl_out}")
        target_compile_definitions(DeckLinkSDK PUBLIC HAS_DECKLINK=1)
        target_link_libraries(DeckLinkSDK PUBLIC ole32 oleaut32)
        add_library(DeckLink::SDK ALIAS DeckLinkSDK)
        set(PLAYPLAY_HAVE_DECKLINK ON)
        message(STATUS "DeckLink SDK: ${PLAYPLAY_DECKLINK_ROOT} (midl ${PLAYPLAY_MIDL})")
    else()
        message(WARNING "DeckLink SDK found but midl.exe not found; DeckLink output disabled")
    endif()
else()
    message(STATUS "DeckLink SDK not found/disabled (PLAYPLAY_DECKLINK_ROOT=${PLAYPLAY_DECKLINK_ROOT})")
endif()
