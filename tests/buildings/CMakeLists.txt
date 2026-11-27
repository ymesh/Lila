# Buildings: the grammar compiled in directly (with Weathering's rooftop layout, as the extension does), and the package
# driven through its commands with Settlement and Terrain for the OSM fixture town.
set(_bld ${CMAKE_SOURCE_DIR}/extensions/Buildings)
set(_weathering ${CMAKE_SOURCE_DIR}/extensions/Weathering)
add_executable(lila_buildings_tests main.cpp test_grammar.cpp test_commands.cpp test_interior.cpp
    ${_bld}/Geometry.cpp ${_bld}/Style.cpp ${_bld}/Grammar.cpp ${_bld}/Modules.cpp ${_bld}/Interior.cpp ${_bld}/ResultCache.cpp ${_weathering}/RooftopLayout.cpp ${_weathering}/GenCommon.cpp)
set_source_files_properties(${_weathering}/RooftopLayout.cpp ${_weathering}/GenCommon.cpp TARGET_DIRECTORY lila_buildings_tests PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
target_include_directories(lila_buildings_tests PRIVATE ${_bld} ${_weathering})
target_link_libraries(lila_buildings_tests PRIVATE LilaUI doctest::doctest)
add_dependencies(lila_buildings_tests lila_buildings_extension lila_settlement_extension lila_terrain_extension lila_stagehand_extension)
target_compile_definitions(lila_buildings_tests PRIVATE
    LILA_TEST_BUILDINGS_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/Buildings.lilaext"
    LILA_TEST_SETTLEMENT_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/Settlement.lilaext"
    LILA_TEST_TERRAIN_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/Terrain.lilaext"
    LILA_TEST_STAGEHAND_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/Stagehand.lilaext"
    LILA_TEST_BUILDINGS_STYLES="${_bld}/resources/styles")
add_test(NAME lila_buildings_tests COMMAND lila_buildings_tests)
