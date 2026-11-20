# Weathering, built as <bin>/<config>/extensions/Weathering.lilaext next to lila.exe, which loads it by default after
# Stagehand. One sources line per area.
set(WEATHERING_CORE_SOURCES Weathering.cpp Weathering.h Pieces.cpp Pieces.h Placeholders.cpp Placeholders.h)
set(WEATHERING_AGE_SOURCES Age.cpp Ground.cpp BasePieces.cpp Unreal.cpp WeatheringUi.cpp)
set(WEATHERING_MASK_SOURCES Masks.cpp Masks.h MaskCompute.cpp MaskCompute.h MasksUi.cpp MasksUnreal.cpp)
set(WEATHERING_EDGE_SOURCES Edges.cpp EdgeGeometry.cpp EdgeGeometry.h EdgeSheets.cpp EdgeSheets.h)
set(WEATHERING_IVY_SOURCES Ivy.cpp Ivy.h IvyGrow.cpp IvyGrow.h IvyUi.cpp IvyPanelKit.h)
set(WEATHERING_GENERATOR_SOURCES Generators.cpp GenCommon.cpp GenCommon.h GenNodes.cpp GenNodes.h GenPlaceholders.cpp GenPlaceholders.h RoofLayout.cpp RoofLayout.h RooftopLayout.cpp RooftopLayout.h Truss.cpp Truss.h WindowLayout.cpp WindowLayout.h WireLayout.cpp WireLayout.h)
lila_add_extension(lila_weathering_extension ID Weathering MANIFEST extension.json UI
    SOURCES ${WEATHERING_CORE_SOURCES} ${WEATHERING_AGE_SOURCES} ${WEATHERING_MASK_SOURCES} ${WEATHERING_EDGE_SOURCES}
            ${WEATHERING_IVY_SOURCES} ${WEATHERING_GENERATOR_SOURCES})
foreach(_host lila lilabatch)
    if(TARGET ${_host})
        add_dependencies(${_host} lila_weathering_extension)
    endif()
endforeach()
