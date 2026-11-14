# Clang on Linux, for Lila and (through the x64-linux-clang triplet) every vcpkg port, so both sides build with one
# compiler. The `linux-clang` preset chainloads it from vcpkg's toolchain. A distro Clang brings its own C++ library
# choice: Enterprise Linux's Clang uses the newest gcc-toolset's libstdc++ (GCC 15 on 9.8, which has <format>).
if(NOT CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER clang)
endif()
if(NOT CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER clang++)
endif()

# A port build (vcpkg passes the triplet's VCPKG_CRT_LINKAGE and its root): chainloading replaces vcpkg's own Linux
# toolchain, which ports still need (system name, -fPIC, the triplet's flags), so take it after the compiler. Lila's
# own configure must not: its CMAKE_SYSTEM_NAME would make CMake treat the build as a cross-compile.
if(DEFINED VCPKG_CRT_LINKAGE AND DEFINED _VCPKG_ROOT_DIR)
    include("${_VCPKG_ROOT_DIR}/scripts/toolchains/linux.cmake")
endif()
