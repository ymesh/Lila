# The Terrain extension: its core compiled in directly, and the package driven through its commands.
add_executable(lila_terrain_tests main.cpp test_terrain.cpp test_stamps.cpp test_contract.cpp test_export_engines.cpp test_water.cpp test_roads.cpp test_volume.cpp test_landform.cpp test_brushes.cpp test_materials.cpp test_heightfield_draw.cpp test_agents.cpp test_cliffs.cpp ${CMAKE_SOURCE_DIR}/extensions/Terrain/Core.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Terrain/Export.cpp ${CMAKE_SOURCE_DIR}/extensions/Terrain/ExportEngines.cpp ${CMAKE_SOURCE_DIR}/extensions/Terrain/Erosion.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Terrain/ErosionGpu.cpp ${CMAKE_SOURCE_DIR}/extensions/Terrain/Inputs.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Terrain/Stamps.cpp ${CMAKE_SOURCE_DIR}/extensions/Terrain/Analysis.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Terrain/Water.cpp ${CMAKE_SOURCE_DIR}/extensions/Terrain/Roads.cpp ${CMAKE_SOURCE_DIR}/extensions/Terrain/Volume.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Terrain/Landform.cpp ${CMAKE_SOURCE_DIR}/extensions/Terrain/LandformGpu.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Terrain/ErosionFilter.cpp ${CMAKE_SOURCE_DIR}/extensions/Terrain/Shaping.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Terrain/Brushes.cpp
    test_inputs.cpp ${CMAKE_SOURCE_DIR}/extensions/Terrain/Projection.cpp ${CMAKE_SOURCE_DIR}/extensions/Terrain/Painted.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Terrain/Osm.cpp ${CMAKE_SOURCE_DIR}/extensions/Terrain/Fetch.cpp ${CMAKE_SOURCE_DIR}/extensions/Terrain/LandCover.cpp ${CMAKE_SOURCE_DIR}/extensions/Terrain/DemRefine.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Terrain/Materials.cpp ${CMAKE_SOURCE_DIR}/extensions/Terrain/GroundCover.cpp ${CMAKE_SOURCE_DIR}/extensions/Terrain/Cliffs.cpp)
target_include_directories(lila_terrain_tests PRIVATE ${CMAKE_SOURCE_DIR}/extensions/Terrain)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GCC 15.2 crashes in scalar-evolution final value replacement on this file at -O2.
    set_source_files_properties(test_heightfield_draw.cpp PROPERTIES COMPILE_OPTIONS -fno-tree-scev-cprop SKIP_UNITY_BUILD_INCLUSION ON)
endif()
find_package(miniz CONFIG REQUIRED)
target_link_libraries(lila_terrain_tests PRIVATE LilaUI doctest::doctest miniz::miniz)
add_dependencies(lila_terrain_tests lila_terrain_extension)
target_compile_definitions(lila_terrain_tests PRIVATE
    LILA_TEST_TERRAIN_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/Terrain.lilaext"
    LILA_TEST_TERRAIN_SCRIPTS="${CMAKE_SOURCE_DIR}/extensions/Terrain/resources/engines")
add_test(NAME lila_terrain_tests COMMAND lila_terrain_tests "--test-case-exclude=*network*")
# Downloads real data; runs only with LILA_NETWORK_TESTS=1 set.
add_test(NAME lila_terrain_network_tests COMMAND lila_terrain_tests "--test-case=*network*")
set_tests_properties(lila_terrain_network_tests PROPERTIES LABELS network)
