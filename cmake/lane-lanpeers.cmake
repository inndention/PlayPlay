if(TARGET PlayPlaySupervisor)
    target_sources(PlayPlaySupervisor PRIVATE "${CMAKE_SOURCE_DIR}/playout/supervisor/lanpeers.cpp")
endif()
if(PLAYPLAY_BUILD_TESTS AND COMMAND playplay_add_schedule_test)
    playplay_add_schedule_test(test_lanpeers "${CMAKE_SOURCE_DIR}/tests/test_lanpeers.cpp"
        "${CMAKE_SOURCE_DIR}/playout/supervisor/lanpeers.cpp"
        "${CMAKE_SOURCE_DIR}/playout/supervisor/laneext.cpp"
        "${CMAKE_SOURCE_DIR}/playout/supervisor/restapiserver.cpp")
    target_link_libraries(test_lanpeers PRIVATE Qt6::Network)
endif()
