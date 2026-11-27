# Terrain, built as <bin>/<config>/extensions/Terrain.lilaext next to lila.exe, which loads it by default.
lila_add_extension(lila_terrain_extension ID Terrain MANIFEST extension.json UI RESOURCES resources
    SOURCES Terrain.cpp Core.cpp Nodes.cpp Commands.cpp Export.cpp Erosion.cpp ErosionGpu.cpp Inputs.cpp Stamps.cpp Analysis.cpp SculptTool.cpp Ui.cpp ExportEngines.cpp ExportEnginesCommands.cpp
            Water.cpp WaterCommands.cpp Roads.cpp RoadCommands.cpp Volume.cpp VolumeCommands.cpp TerrainEdit.cpp
            Landform.cpp LandformGpu.cpp LandformNodes.cpp LandformCommands.cpp ErosionFilter.cpp Shaping.cpp Brushes.cpp
            Projection.cpp Painted.cpp Osm.cpp Fetch.cpp LandCover.cpp DemRefine.cpp Http.cpp InputCommands.cpp
            Materials.cpp GroundCover.cpp MaterialNodes.cpp MaterialCommands.cpp SurfaceUi.cpp Query.cpp Spec.cpp Wedge.cpp Checks.cpp Cliffs.cpp CliffNodes.cpp CliffCommands.cpp CliffUi.cpp
            Bathymetry.cpp Channels.cpp WaterRoutes.cpp CoastCommands.cpp CoastUi.cpp CoastTools.cpp
            Terrain.h Export.h ExportEngines.h Erosion.h ErosionGpu.h Inputs.h Stamps.h Analysis.h Water.h Roads.h Volume.h
            Landform.h LandformGpu.h LandformCommands.h ErosionFilter.h Shaping.h Brushes.h
            Projection.h Painted.h Osm.h Fetch.h DemRefine.h Http.h Materials.h GroundCover.h MaterialNodes.h Query.h Spec.h Wedge.h Checks.h Cliffs.h CliffScene.h Coast.h)
lila_add_shaders(lila_terrain_extension FILES shaders/terrain_erosion.slang shaders/terrain_landform.slang shaders/terrain_talus.slang
    INCLUDES shaders/terrain_landform.slang)
find_package(miniz CONFIG REQUIRED)
find_package(Qt6 COMPONENTS Network REQUIRED)
target_link_libraries(lila_terrain_extension PRIVATE miniz::miniz Qt6::Network)
foreach(_host lila lilabatch)
    if(TARGET ${_host})
        add_dependencies(${_host} lila_terrain_extension)
    endif()
endforeach()
