# Plating: the plate builder compiled in directly, driven by layouts drawn in the test.
add_executable(lila_plating_tests main.cpp test_plates.cpp test_fasteners.cpp test_patterns.cpp)
target_sources(lila_plating_tests PRIVATE ${CMAKE_SOURCE_DIR}/extensions/Plating/Plates.cpp ${CMAKE_SOURCE_DIR}/extensions/Plating/Fasteners.cpp ${CMAKE_SOURCE_DIR}/extensions/Plating/Patterns.cpp)
target_include_directories(lila_plating_tests PRIVATE ${CMAKE_SOURCE_DIR}/extensions/Plating ${CMAKE_SOURCE_DIR}/sdk/include ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(lila_plating_tests PRIVATE LilaScene doctest::doctest)
add_test(NAME lila_plating_tests COMMAND lila_plating_tests)
