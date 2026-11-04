# ---------------------------------------------------------------------------
# Lane "studio-film", slice ST-30 (internal plan lane-plan.json; internal plan starmedia-studio.md 2.5 / 2.6): the publishing
# lane's hard rails in PlayPlay's Publisher. playout/supervisor/pubrails.{h,cpp} is pure (QtCore only); publisher.cpp
# calls it and adds its routes through laneext, so supervisorcore*.cpp and CMakeLists.txt stay untouched.
#
# UNCONDITIONAL on purpose: PlayPlaySupervisor's publisher.cpp needs pubrails.cpp, so the live build tree must link it
# too (a guarded add here would break the next live rebuild).
# ---------------------------------------------------------------------------
target_sources(PlayPlaySupervisor PRIVATE
    ${CMAKE_SOURCE_DIR}/playout/supervisor/pubrails.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/pubrails.h)

# test_pubrails: every rail as a table test (approval binding, caps and gap, outcome classes, reconcile matching,
# restart recovery, the item ladder). Links only the module under test.
if(PLAYPLAY_BUILD_TESTS AND COMMAND playplay_add_schedule_test AND EXISTS "${CMAKE_SOURCE_DIR}/tests/test_pubrails.cpp")
    playplay_add_schedule_test(test_pubrails
        ${CMAKE_SOURCE_DIR}/tests/test_pubrails.cpp
        ${CMAKE_SOURCE_DIR}/playout/supervisor/pubrails.cpp ${CMAKE_SOURCE_DIR}/playout/supervisor/pubrails.h)
    target_include_directories(test_pubrails PRIVATE ${CMAKE_SOURCE_DIR})
endif()
