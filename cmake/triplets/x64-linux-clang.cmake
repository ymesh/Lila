# x64-linux built with Clang (the `linux-clang` preset): the same linkage rules, every port compiled by the Clang
# toolchain file. Packages go to their own vcpkg_installed-clang, apart from the GCC build's.
include("${CMAKE_CURRENT_LIST_DIR}/x64-linux.cmake")
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/../toolchains/linux-clang.cmake")

# x64-linux silences GCC's false maybe-uninitialized for Clipper2 (built with -Werror); Clang has no such warning and
# -Werror would turn its "unknown warning option" into an error.
if(PORT STREQUAL "clipper2")
    foreach(_flags VCPKG_CXX_FLAGS VCPKG_C_FLAGS)
        string(REPLACE "-Wno-maybe-uninitialized" "" ${_flags} "${${_flags}}")
    endforeach()
endif()
