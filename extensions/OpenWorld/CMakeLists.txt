# Open World, built as <bin>/<config>/extensions/OpenWorld.lilaext next to lila.exe, which loads it by default.
# Terrain's readers, erosion and router are compiled in (as its tests do), so a remix works on real data before any
# terrain exists; extensions don't link each other.
set(_terrain ${CMAKE_SOURCE_DIR}/extensions/Terrain)
set(_terrain_sources ${_terrain}/Core.cpp ${_terrain}/Erosion.cpp ${_terrain}/Inputs.cpp ${_terrain}/Projection.cpp ${_terrain}/Osm.cpp
    ${_terrain}/Fetch.cpp ${_terrain}/LandCover.cpp ${_terrain}/Stamps.cpp ${_terrain}/Brushes.cpp ${_terrain}/Shaping.cpp ${_terrain}/Painted.cpp
    ${_terrain}/Analysis.cpp ${_terrain}/Bathymetry.cpp ${_terrain}/ErosionGpu.cpp ${_terrain}/Http.cpp
    ${_terrain}/Landform.cpp ${_terrain}/LandformGpu.cpp ${_terrain}/ErosionFilter.cpp)
set(_clipper ${CMAKE_SOURCE_DIR}/src/curve/third_party/clipper2)
lila_add_extension(lila_openworld_extension ID OpenWorld MANIFEST extension.json UI
    SOURCES OpenWorld.cpp OpenWorld.h RemixNode.h OsmDoc.cpp OsmDoc.h RemixSpec.cpp RemixSpec.h Warp.cpp Warp.h Remix.cpp Remix.h RemixWork.h
            Features.cpp Repair.cpp Check.cpp RemixNode.cpp RemixCommands.cpp RemixUi.cpp Sites.h SitesKit.h SitesLayout.cpp SitesCoast.cpp SitesLand.cpp SitesCompound.cpp SitesNode.h SitesNode.cpp SitesCommands.cpp SitesUi.cpp
            Zones.cpp Zones.h ZoneScene.h ZoneCommands.cpp ZoneUi.cpp
            World.h WorldScene.h WorldSpec.cpp WorldCommands.cpp WorldUnreal.cpp WorldCheck.cpp WorldMemory.cpp WorldUi.cpp WorldFetch.h WorldFetch.cpp WorldFetchCommands.cpp
            ${_terrain_sources} ${_clipper}/clipper.engine.cpp)
set_source_files_properties(${_terrain_sources} ${_clipper}/clipper.engine.cpp WorldMemory.cpp PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
target_include_directories(lila_openworld_extension PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${_terrain} ${_clipper}/include)
find_package(miniz CONFIG REQUIRED)
find_package(Qt6 COMPONENTS Network REQUIRED)
target_link_libraries(lila_openworld_extension PRIVATE miniz::miniz Qt6::Network)
foreach(_host lila lilabatch)
    if(TARGET ${_host})
        add_dependencies(${_host} lila_openworld_extension)
    endif()
endforeach()
# Clipper2 (vendored, from src/curve) trips MSVC unreachable-code warnings when its templates are instantiated.
target_compile_options(lila_openworld_extension PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/wd4702>)
