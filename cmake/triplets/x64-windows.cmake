set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
# The one MSVC toolset for dependencies and Lila alike. scripts/env.cmd, the CMake configure check and
# CMakePresets.json (toolset version) read or match it; mixing toolsets breaks at link time.
set(VCPKG_PLATFORM_TOOLSET v143)
set(VCPKG_PLATFORM_TOOLSET_VERSION 14.44)
# Avoid NASM being misidentified as MSVC because its path contains "Microsoft".
list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS
    "-DCMAKE_ASM_NASM_COMPILER_ID=NASM"
)
