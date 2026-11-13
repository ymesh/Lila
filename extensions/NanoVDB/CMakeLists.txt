# NanoVDB, built as <bin>/<config>/extensions/NanoVDB.lilaext next to lila.exe: volumes to and from .nvdb (NanoVDB headers,
# Apache-2.0, from OpenVDB 12.0.1, vendored under third_party) and .vdb (OpenVDB from vcpkg, linked into this extension only).
find_package(ZLIB REQUIRED)
find_package(OpenVDB CONFIG REQUIRED)
lila_add_extension(lila_nanovdb_extension ID NanoVDB MANIFEST extension.json SOURCES NanoVDB.cpp Vdb.cpp Volumes.h)
target_include_directories(lila_nanovdb_extension SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nanovdb)
target_compile_definitions(lila_nanovdb_extension PRIVATE NANOVDB_USE_ZIP)
target_link_libraries(lila_nanovdb_extension PRIVATE ZLIB::ZLIB OpenVDB::openvdb)
if(NOT WIN32)
    # FindOpenVDB passes Blosc on only for a shared OpenVDB, and the Linux triplet's is static.
    find_package(blosc CONFIG REQUIRED)
    target_link_libraries(lila_nanovdb_extension PRIVATE blosc_static)
endif()
foreach(_host lila lilabatch)
    if(TARGET ${_host})
        add_dependencies(${_host} lila_nanovdb_extension)
    endif()
endforeach()
# OpenVDB calls log() unqualified, which clashes with lila::log once another file in the batch says using namespace lila.
set_source_files_properties(Vdb.cpp PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
if(MSVC)
    # NanoVDB.h has a branch the optimizer finds unreachable; C4702 comes from code generation, past /external:W0.
    target_compile_options(lila_nanovdb_extension PRIVATE /wd4702)
endif()
