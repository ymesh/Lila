# World Remix and site kits: their cores compiled in directly (with Terrain's readers, erosion and router, as the extension does), and
# the package driven through its commands. Fixtures are small synthetic files in data/ (make_fixtures.py); no network.
set(_ow ${CMAKE_SOURCE_DIR}/extensions/OpenWorld)
set(_terrain ${CMAKE_SOURCE_DIR}/extensions/Terrain)
add_executable(lila_openworld_tests main.cpp test_remix.cpp test_remix_look.cpp test_remix_commands.cpp test_remix_w1.cpp test_zones.cpp test_sites.cpp test_names_perf.cpp test_world.cpp test_world_replace.cpp test_world_unreal.cpp test_world_fetch.cpp ${_ow}/WorldSpec.cpp ${_ow}/WorldFetch.cpp
    ${_ow}/OsmDoc.cpp ${_ow}/RemixSpec.cpp ${_ow}/Warp.cpp ${_ow}/Remix.cpp ${_ow}/Features.cpp ${_ow}/Repair.cpp ${_ow}/Check.cpp ${_ow}/Zones.cpp ${_ow}/SitesLayout.cpp ${_ow}/SitesCoast.cpp ${_ow}/SitesLand.cpp ${_ow}/SitesCompound.cpp
    ${_terrain}/Core.cpp ${_terrain}/Erosion.cpp ${_terrain}/Inputs.cpp ${_terrain}/Projection.cpp ${_terrain}/Osm.cpp
    ${_terrain}/Fetch.cpp ${_terrain}/LandCover.cpp ${_terrain}/Stamps.cpp ${_terrain}/Brushes.cpp ${_terrain}/Shaping.cpp ${_terrain}/Painted.cpp
    ${_terrain}/Analysis.cpp ${_terrain}/Bathymetry.cpp ${_terrain}/ErosionGpu.cpp ${_terrain}/Landform.cpp ${_terrain}/LandformGpu.cpp ${_terrain}/ErosionFilter.cpp ${CMAKE_SOURCE_DIR}/src/curve/third_party/clipper2/clipper.engine.cpp)
set_source_files_properties(${_terrain}/Core.cpp ${_terrain}/Erosion.cpp ${_terrain}/Inputs.cpp ${_terrain}/Projection.cpp ${_terrain}/Osm.cpp
    ${_terrain}/Fetch.cpp ${_terrain}/LandCover.cpp ${_terrain}/Stamps.cpp ${_terrain}/Brushes.cpp ${_terrain}/Shaping.cpp ${_terrain}/Painted.cpp
    ${_terrain}/Analysis.cpp ${_terrain}/Bathymetry.cpp ${_terrain}/ErosionGpu.cpp ${_terrain}/Landform.cpp ${_terrain}/LandformGpu.cpp ${_terrain}/ErosionFilter.cpp ${CMAKE_SOURCE_DIR}/src/curve/third_party/clipper2/clipper.engine.cpp
    TARGET_DIRECTORY lila_openworld_tests PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
target_include_directories(lila_openworld_tests PRIVATE ${_ow} ${_terrain} ${CMAKE_SOURCE_DIR}/src/curve/third_party/clipper2/include)
find_package(miniz CONFIG REQUIRED)
target_link_libraries(lila_openworld_tests PRIVATE LilaUI doctest::doctest miniz::miniz)
add_dependencies(lila_openworld_tests lila_openworld_extension lila_terrain_extension lila_settlement_extension lila_buildings_extension)
target_compile_definitions(lila_openworld_tests PRIVATE
    LILA_TEST_OPENWORLD_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/OpenWorld.lilaext"
    LILA_TEST_TERRAIN_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/Terrain.lilaext"
    LILA_TEST_SETTLEMENT_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/Settlement.lilaext"
    LILA_TEST_BUILDINGS_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/Buildings.lilaext"
    LILA_TEST_OPENWORLD_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
add_test(NAME lila_openworld_tests COMMAND lila_openworld_tests)
# Clipper2 (vendored, from src/curve) trips MSVC unreachable-code warnings when its templates are instantiated.
target_compile_options(lila_openworld_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/wd4702>)
# lilabatch builds a small world, saves it and exits, then empty sessions with the Jolt extensions: exit 0, no crash report.
if(TARGET lilabatch)
    add_test(NAME lila_openworld_batch_exit COMMAND ${CMAKE_COMMAND} -DLILABATCH=$<TARGET_FILE:lilabatch>
        -DEXT_DIR=${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions -DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/world_exit.py
        -DWORK=${CMAKE_CURRENT_BINARY_DIR}/batch_exit -P ${CMAKE_CURRENT_SOURCE_DIR}/world_exit.cmake)
endif()
