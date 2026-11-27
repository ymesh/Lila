# Buildings, built as <bin>/<config>/extensions/Buildings.lilaext next to lila.exe, which loads it by default.
# Weathering's rooftop layout is compiled in for roof clutter (extensions don't link each other).
set(_weathering ${CMAKE_SOURCE_DIR}/extensions/Weathering)
set(_weathering_sources ${_weathering}/RooftopLayout.cpp ${_weathering}/GenCommon.cpp)
set(BUILDINGS_CORE_SOURCES Buildings.h Interior.h Geometry.cpp Style.cpp Grammar.cpp Modules.cpp Interior.cpp ResultCache.cpp)
set(BUILDINGS_SCENE_SOURCES BuildingsScene.h Buildings.cpp Styles.cpp Node.cpp Commands.cpp Check.cpp InteriorScene.cpp Ui.cpp PaintTool.cpp RoomTool.cpp Agent.cpp Unreal.cpp LiveRefresh.cpp ViewLod.cpp)
lila_add_extension(lila_buildings_extension ID Buildings MANIFEST extension.json UI RESOURCES resources
    SOURCES ${BUILDINGS_CORE_SOURCES} ${BUILDINGS_SCENE_SOURCES} ${_weathering_sources})
set_source_files_properties(${_weathering_sources} PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
target_include_directories(lila_buildings_extension PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${_weathering})
find_package(zstd CONFIG REQUIRED)
target_link_libraries(lila_buildings_extension PRIVATE zstd::libzstd)
foreach(_host lila lilabatch)
    if(TARGET ${_host})
        add_dependencies(${_host} lila_buildings_extension)
    endif()
endforeach()
