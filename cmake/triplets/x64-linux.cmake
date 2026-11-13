set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)

# Shared like on Windows. Qt: every Lila library and extension uses it, and static copies would each carry their own
# QApplication, metatypes and plugins. FFmpeg: LGPL, and its x86 asm isn't PIC, so it can't go into a .so statically.
if(PORT MATCHES "^qt" OR PORT STREQUAL "ffmpeg")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()

# Clipper2 builds with -Werror and GCC raises false maybe-uninitialized warnings in it. An explicit -Wno wins over -Wall.
if(PORT STREQUAL "clipper2")
    set(VCPKG_CXX_FLAGS "${VCPKG_CXX_FLAGS} -Wno-maybe-uninitialized -Wno-uninitialized")
    set(VCPKG_C_FLAGS "${VCPKG_C_FLAGS} -Wno-maybe-uninitialized -Wno-uninitialized")
endif()

# Meson-based ports need a newer Python than Enterprise Linux's 3.9; Lila's own is 3.14.
if(DEFINED ENV{LILA_PYTHON_DIR} AND EXISTS "$ENV{LILA_PYTHON_DIR}/bin/python3")
    set(Python3_EXECUTABLE "$ENV{LILA_PYTHON_DIR}/bin/python3")
endif()
