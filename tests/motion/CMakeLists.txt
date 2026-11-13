add_executable(lila_motion_tests main.cpp test_engine.cpp test_character.cpp)
target_link_libraries(lila_motion_tests PRIVATE LilaScene doctest::doctest)
target_compile_definitions(lila_motion_tests PRIVATE LILA_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data")
add_test(NAME lila_motion_tests COMMAND lila_motion_tests)
