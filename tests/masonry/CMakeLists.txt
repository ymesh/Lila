# The Masonry extension: its layout and stone meshes compiled in directly, and the package driven through its commands.
add_executable(lila_masonry_tests main.cpp test_masonry.cpp test_masonry_build.cpp test_masonry_more.cpp test_masonry_fit.cpp ${CMAKE_SOURCE_DIR}/extensions/Masonry/MasonryCore.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Masonry/MasonryArch.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Masonry/MasonryBuilding.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Masonry/MasonryRuin.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Masonry/MasonryDress.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Masonry/MasonryTools.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Masonry/MasonryShip.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Masonry/MasonryBake.cpp)
target_include_directories(lila_masonry_tests PRIVATE ${CMAKE_SOURCE_DIR}/extensions/Masonry)
target_link_libraries(lila_masonry_tests PRIVATE LilaUI lila_physics doctest::doctest)
add_dependencies(lila_masonry_tests lila_masonry_extension lila_destruction_extension)
target_compile_definitions(lila_masonry_tests PRIVATE
    LILA_TEST_MASONRY_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/Masonry.lilaext"
    LILA_TEST_DESTRUCTION_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/Destruction.lilaext")
add_test(NAME lila_masonry_tests COMMAND lila_masonry_tests)
