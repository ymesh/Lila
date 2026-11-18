# The Blockwork extension loaded from its staged package and driven through its commands, plus its geometry compiled in.
add_executable(lila_blockwork_tests main.cpp test_blockwork.cpp ${CMAKE_SOURCE_DIR}/extensions/Blockwork/Geometry.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Blockwork/Scene.cpp)
target_include_directories(lila_blockwork_tests PRIVATE ${CMAKE_SOURCE_DIR}/extensions/Blockwork ${CMAKE_SOURCE_DIR}/sdk/include)
target_link_libraries(lila_blockwork_tests PRIVATE LilaUI doctest::doctest)
add_dependencies(lila_blockwork_tests lila_blockwork_extension)
target_compile_definitions(lila_blockwork_tests PRIVATE
    LILA_TEST_BLOCKWORK_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/Blockwork.lilaext")
add_test(NAME lila_blockwork_tests COMMAND lila_blockwork_tests)
