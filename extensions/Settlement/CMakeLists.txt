# Settlement, built as <bin>/<config>/extensions/Settlement.lilaext next to lila.exe, which loads it by default.
lila_add_extension(lila_settlement_extension ID Settlement MANIFEST extension.json UI RESOURCES resources
    SOURCES Settlement.cpp Geometry.cpp StreetGraph.cpp Profiles.cpp Generate.cpp Output.cpp Region.cpp Footprint.cpp Walk.cpp Edits.cpp Commands.cpp Unreal.cpp Ui.cpp
            Names.cpp RoadMesh.cpp Furniture.cpp Lanes.cpp StreetscapeCommands.cpp
            Blocks.cpp Parcels.cpp Envelopes.cpp LotCommands.cpp
            Siting.cpp Plan.cpp Inspect.cpp TerrainLink.cpp
            GenUtil.cpp GenSpec.cpp Tensor.cpp Eras.cpp Fortify.cpp OsmStreets.cpp OsmBuildings.cpp ContourStreets.cpp Levels.cpp Verify.cpp GenCommands.cpp
            Settlement.h SettlementCore.h Streetscape.h Parcels.h SettlementTerrain.h Generators.h)
foreach(_host lila lilabatch)
    if(TARGET ${_host})
        add_dependencies(${_host} lila_settlement_extension)
    endif()
endforeach()
# Export: files, engine folders and the Unreal cell sync.
target_sources(lila_settlement_extension PRIVATE Export.cpp ExportCommands.cpp Export.h)
