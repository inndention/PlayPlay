# ---------------------------------------------------------------------------
# Lane "inhouse-media", DEP-10..12 (internal plan inhouse-deps.md §6.4, §9): PlayPlaySpeech.exe, the in-house replacement for
# scripts/spot_tts.py (and its PowerShell System.Speech / Node sherpa-onnx engines; ASR later).
#   PlayPlaySpeechKit  static library: every tools/speech/*.cpp except the entry point (globbed with CONFIGURE_DEPENDS, so
#                      later slices only add files), linked by the exe and by the tests.
#                      DEP-10: voicemap (Kokoro id -> SAPI voice, Kokoro speaker table, speed/rate), sapitts (SAPI 5 through
#                      COM: ISpVoice into an ISpStream bound to a 48 kHz mono WAV, desktop + OneCore voices), ttscli (the
#                      spot_tts.py argument / stdout contract, levelling through PlayPlayInhouseCommon's wavio).
#   PlayPlaySpeech     the exe (sub-commands tts, voices, env, version).
#   test_inhouse_speech (and test_inhouse_asr once DEP-12 adds it): linked against the kit, built after the exe
#                      (PP_SPEECH_EXE = its path).
# Windows only (SAPI). EXCLUDE_FROM_ALL; nothing is added to PlayPlay / PlayPlaySupervisor / PlayPlayChannelEngine
# (DEP-13 wires the switch), so a full build of any tree, the live one included, compiles nothing from here.
# ---------------------------------------------------------------------------
include("${CMAKE_CURRENT_LIST_DIR}/lane-inhouse-common.cmake")   # PlayPlayInhouseCommon, playplay_add_inhouse_test
include_guard(GLOBAL)

if(NOT WIN32)
    return()
endif()

file(GLOB _pp_speech_sources CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/tools/speech/*.cpp" "${CMAKE_SOURCE_DIR}/tools/speech/*.h")
list(FILTER _pp_speech_sources EXCLUDE REGEX "/tools/speech/speech_main\\.cpp$")
add_library(PlayPlaySpeechKit STATIC EXCLUDE_FROM_ALL ${_pp_speech_sources})
target_include_directories(PlayPlaySpeechKit PUBLIC "${CMAKE_SOURCE_DIR}/tools/speech")
# sapi.lib holds the SAPI CLSIDs / format GUIDs; ole32 / oleaut32 for COM. No ATL (sphelper.h is not used).
target_link_libraries(PlayPlaySpeechKit PUBLIC Qt6::Core PlayPlayInhouseCommon sapi ole32 oleaut32)
set_target_properties(PlayPlaySpeechKit PROPERTIES AUTOUIC OFF AUTORCC OFF)

add_executable(PlayPlaySpeech EXCLUDE_FROM_ALL "${CMAKE_SOURCE_DIR}/tools/speech/speech_main.cpp")
target_link_libraries(PlayPlaySpeech PRIVATE PlayPlaySpeechKit)
set_target_properties(PlayPlaySpeech PROPERTIES AUTOUIC OFF AUTORCC OFF)

file(GLOB _pp_speech_tests CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tests/inhouse/test_inhouse_*.cpp")
list(FILTER _pp_speech_tests INCLUDE REGEX "/test_inhouse_(speech|asr)\\.cpp$")
foreach(_src IN LISTS _pp_speech_tests)
    get_filename_component(_name "${_src}" NAME_WE)
    playplay_add_inhouse_test(${_name} SOURCES "${_src}" LIBS PlayPlaySpeechKit)
    if(TARGET ${_name})
        add_dependencies(${_name} PlayPlaySpeech)
        target_compile_definitions(${_name} PRIVATE PP_SPEECH_EXE="$<TARGET_FILE:PlayPlaySpeech>")
    endif()
endforeach()
