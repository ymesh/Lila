# The Settlement extension: its layout compiled in directly, and the package driven through its commands.
set(_settlement ${CMAKE_SOURCE_DIR}/extensions/Settlement)
add_executable(lila_settlement_tests main.cpp test_settlement.cpp test_structure.cpp test_streets.cpp test_street.cpp test_lots.cpp test_terrain.cpp test_gen.cpp test_osm_buildings.cpp test_city.cpp test_roads_ground.cpp
    ${_settlement}/Geometry.cpp ${_settlement}/StreetGraph.cpp ${_settlement}/Profiles.cpp ${_settlement}/Generate.cpp
    ${_settlement}/Output.cpp ${_settlement}/Region.cpp
    ${_settlement}/Footprint.cpp ${_settlement}/Walk.cpp ${_settlement}/Edits.cpp
    ${_settlement}/Names.cpp ${_settlement}/RoadMesh.cpp ${_settlement}/Furniture.cpp ${_settlement}/Lanes.cpp
    ${_settlement}/Blocks.cpp ${_settlement}/Parcels.cpp ${_settlement}/Envelopes.cpp
    ${_settlement}/Siting.cpp ${_settlement}/Plan.cpp ${_settlement}/Inspect.cpp
    ${_settlement}/GenUtil.cpp ${_settlement}/GenSpec.cpp ${_settlement}/Tensor.cpp ${_settlement}/Eras.cpp ${_settlement}/Fortify.cpp
    ${_settlement}/OsmStreets.cpp ${_settlement}/OsmBuildings.cpp ${_settlement}/ContourStreets.cpp ${_settlement}/Levels.cpp ${_settlement}/Verify.cpp)
target_sources(lila_settlement_tests PRIVATE test_export.cpp ${_settlement}/Export.cpp)
target_include_directories(lila_settlement_tests PRIVATE ${_settlement})
target_link_libraries(lila_settlement_tests PRIVATE LilaUI doctest::doctest)
add_dependencies(lila_settlement_tests lila_settlement_extension lila_terrain_extension lila_masonry_extension)
target_compile_definitions(lila_settlement_tests PRIVATE
    LILA_TEST_SETTLEMENT_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/Settlement.lilaext"
    LILA_TEST_TERRAIN_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/Terrain.lilaext"
    LILA_TEST_MASONRY_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/Masonry.lilaext")
add_test(NAME lila_settlement_tests COMMAND lila_settlement_tests)
