# Regenerates the docs site's Python API pages (<LILA_DOCS_REPO>/docs/python/) whenever Lila, an extension or the lila
# package changes, so the published reference always matches the script editor. Pages are only rewritten when their
# text changes; review and commit them in the docs repo as usual.
cmake_path(ABSOLUTE_PATH CMAKE_SOURCE_DIR NORMALIZE OUTPUT_VARIABLE _source)
cmake_path(GET _source PARENT_PATH _siblings)
set(LILA_DOCS_REPO "${_siblings}/Lila-3D" CACHE PATH "Docs site checkout the API pages are written into; empty turns it off")

# The roadmap the app ships (core/Features.h) is the one the site shows: copied in whenever the site's changes.
if(LILA_DOCS_REPO AND EXISTS "${LILA_DOCS_REPO}/roadmap.json")
    configure_file("${LILA_DOCS_REPO}/roadmap.json" "${CMAKE_SOURCE_DIR}/resources/roadmap/roadmap.json" COPYONLY)
endif()

if(LILA_DOCS_REPO AND EXISTS "${LILA_DOCS_REPO}/docs")
    get_property(_extensions GLOBAL PROPERTY LILA_EXTENSION_TARGETS)
    set(_binaries $<TARGET_FILE:lila> $<TARGET_FILE:LilaFoundation> $<TARGET_FILE:LilaScene> $<TARGET_FILE:LilaUI>)
    foreach(_ext IN LISTS _extensions)
        list(APPEND _binaries $<TARGET_FILE:${_ext}>)
    endforeach()
    file(GLOB_RECURSE _python CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/python/lila/*.py)
    set(_stamp ${CMAKE_BINARY_DIR}/docs-python.stamp)
    add_custom_command(OUTPUT ${_stamp}
        COMMAND $<TARGET_FILE:lila> -b -c "import lila._apipages as p; p.write(r'${LILA_DOCS_REPO}/docs')"
        COMMAND ${CMAKE_COMMAND} -E touch ${_stamp}
        DEPENDS ${_binaries} ${_python}
        COMMENT "Writing the Python API docs to ${LILA_DOCS_REPO}/docs/python"
        VERBATIM)
    add_custom_target(lila_docs ALL DEPENDS ${_stamp})
    add_dependencies(lila_docs lila ${_extensions})
endif()
