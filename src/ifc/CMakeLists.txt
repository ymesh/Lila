find_package(web-ifc CONFIG REQUIRED)

lila_module(lila_ifc Scene
    IfcImport.cpp
    IfcImport.h)
# web-ifc's headers include the header-only libraries it bundles, installed beside them.
get_target_property(_webifc_include web-ifc::web-ifc-library INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(lila_ifc SYSTEM PRIVATE ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include/web-ifc/third_party)
target_link_libraries(lila_ifc PUBLIC lila_scene lila_geo PRIVATE web-ifc::web-ifc-library)
