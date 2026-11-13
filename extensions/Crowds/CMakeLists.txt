# Crowds, built as <bin>/<config>/extensions/Crowds.lilaext next to lila.exe, which loads it by default.
lila_add_extension(lila_crowds_extension ID Crowds MANIFEST extension.json UI
    SOURCES Crowd.cpp CrowdCore.cpp Orca.cpp CrowdBake.cpp CrowdUi.cpp CrowdDraw.cpp Crowd.h CrowdCore.h Orca.h CrowdDraw.h)
foreach(_host lila lilabatch)
    if(TARGET ${_host})
        add_dependencies(${_host} lila_crowds_extension)
    endif()
endforeach()
