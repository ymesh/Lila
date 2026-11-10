add_executable(lila_layout_tests main.cpp test_fit.cpp test_edit.cpp test_layout_node.cpp test_maps.cpp test_soft_net.cpp)
target_link_libraries(lila_layout_tests PRIVATE LilaScene doctest::doctest)
target_include_directories(lila_layout_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests)
add_test(NAME lila_layout_tests COMMAND lila_layout_tests)
