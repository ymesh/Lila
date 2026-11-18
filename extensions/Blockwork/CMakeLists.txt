# Blockwork, built as <bin>/<config>/extensions/Blockwork.lilaext next to lila.exe, which loads it by default.
lila_add_extension(lila_blockwork_extension ID Blockwork MANIFEST extension.json RESOURCES resources UI
    SOURCES Blockwork.cpp Geometry.cpp Scene.cpp Commands.cpp Texture.cpp Tool.cpp Ui.cpp TexturePanel.cpp Entities.cpp Organise.cpp OrganisePanels.cpp Entities.h Blockwork.h Tool.h)
foreach(_host lila lilabatch)
    if(TARGET ${_host})
        add_dependencies(${_host} lila_blockwork_extension)
    endif()
endforeach()
