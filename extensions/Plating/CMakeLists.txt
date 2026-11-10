# Plating, built as <bin>/<config>/extensions/Plating.lilaext next to lila.exe, which loads it by default.
lila_add_extension(lila_plating_extension ID Plating MANIFEST extension.json UI
    SOURCES Plating.cpp Plates.cpp PlatesNode.cpp Fasteners.cpp FastenersNode.cpp Patterns.cpp PatternsNode.cpp Plating.h Plates.h Fasteners.h Patterns.h)
foreach(_host lila lilabatch)
    if(TARGET ${_host})
        add_dependencies(${_host} lila_plating_extension)
    endif()
endforeach()
