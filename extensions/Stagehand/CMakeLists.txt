# Stagehand, built as <bin>/<config>/extensions/Stagehand.lilaext next to lila.exe, which loads it by default. Jolt comes
# in statically with lila_physics.
lila_add_extension(lila_stagehand_extension ID Stagehand MANIFEST extension.json UI
    SOURCES Stagehand.cpp PenetrationCheck.cpp Bodies.cpp World.cpp Session.cpp Commands.cpp Ui.cpp Library.cpp Place.cpp LibraryUi.cpp
            Arrange.cpp ArrangeNodes.cpp WearNodes.cpp ArrangeCommands.cpp LevelKit.cpp KitTool.cpp Realize.cpp Starter.cpp UnrealExport.cpp Presets.cpp Kit.h Stagehand.h Session.h Library.h Arrange.h NodeSockets.h)
target_link_libraries(lila_stagehand_extension PRIVATE lila_physics)
foreach(_host lila lilabatch)
    if(TARGET ${_host})
        add_dependencies(${_host} lila_stagehand_extension)
    endif()
endforeach()
