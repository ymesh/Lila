set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Linux)

# Use Python 3.11+ for meson and other build tools
set(Python3_EXECUTABLE /usr/local/bin/python3.14)

# FFmpeg is LGPL and ships as shared libraries, as on Windows (src/media/CMakeLists.txt). Its static archives can't go
# into libLilaScene.so anyway: the x86 inline asm references exported constants directly (non-PIC relocations).
if(PORT STREQUAL "ffmpeg")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()

# Qt is shared too: Lila's libraries and each extension .so use Qt, and a static Qt would put a separate copy (its own
# QApplication, metatypes, plugins) in each of them. Same as Windows, and Qt's LGPL terms.
if(PORT MATCHES "^qt")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()
