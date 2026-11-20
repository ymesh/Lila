# Weathering: the extension loaded from its staged package (after Stagehand) and driven through its commands. One line
# per area for test files and for sources compiled in directly.
set(WEATHERING_TEST_FILES main.cpp test_pieces.cpp test_age.cpp test_ground.cpp)
list(APPEND WEATHERING_TEST_FILES test_masks.cpp)
list(APPEND WEATHERING_TEST_FILES test_edges.cpp)
list(APPEND WEATHERING_TEST_FILES test_ivy.cpp)
list(APPEND WEATHERING_TEST_FILES test_generators.cpp)
add_executable(lila_weathering_tests ${WEATHERING_TEST_FILES})
target_sources(lila_weathering_tests PRIVATE ${CMAKE_SOURCE_DIR}/extensions/Weathering/Pieces.cpp ${CMAKE_SOURCE_DIR}/extensions/Weathering/Placeholders.cpp)
target_sources(lila_weathering_tests PRIVATE ${CMAKE_SOURCE_DIR}/extensions/Weathering/IvyGrow.cpp)
target_sources(lila_weathering_tests PRIVATE ${CMAKE_SOURCE_DIR}/extensions/Weathering/GenCommon.cpp ${CMAKE_SOURCE_DIR}/extensions/Weathering/GenPlaceholders.cpp ${CMAKE_SOURCE_DIR}/extensions/Weathering/RoofLayout.cpp ${CMAKE_SOURCE_DIR}/extensions/Weathering/RooftopLayout.cpp ${CMAKE_SOURCE_DIR}/extensions/Weathering/Truss.cpp ${CMAKE_SOURCE_DIR}/extensions/Weathering/WindowLayout.cpp ${CMAKE_SOURCE_DIR}/extensions/Weathering/WireLayout.cpp)
target_sources(lila_weathering_tests PRIVATE ${CMAKE_SOURCE_DIR}/extensions/Weathering/MaskCompute.cpp)
target_include_directories(lila_weathering_tests PRIVATE ${CMAKE_SOURCE_DIR}/extensions/Weathering ${CMAKE_SOURCE_DIR}/sdk/include ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(lila_weathering_tests PRIVATE LilaUI doctest::doctest)
add_dependencies(lila_weathering_tests lila_weathering_extension lila_stagehand_extension)
target_compile_definitions(lila_weathering_tests PRIVATE
    LILA_TEST_STAGEHAND_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/Stagehand.lilaext"
    LILA_TEST_WEATHERING_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/Weathering.lilaext")
add_test(NAME lila_weathering_tests COMMAND lila_weathering_tests)
# Edges: geometry compiled in, Blockwork and Trim Sheets loaded by the edge tests.
target_sources(lila_weathering_tests PRIVATE ${CMAKE_SOURCE_DIR}/extensions/Weathering/EdgeGeometry.cpp)
add_dependencies(lila_weathering_tests lila_blockwork_extension lila_trimsheets_extension)
target_compile_definitions(lila_weathering_tests PRIVATE
    LILA_TEST_BLOCKWORK_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/Blockwork.lilaext"
    LILA_TEST_TRIM_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/TrimSheets.lilaext")
