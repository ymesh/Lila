if(VCPKG_TARGET_IS_WINDOWS)
    vcpkg_check_linkage(ONLY_STATIC_LIBRARY)
endif()

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO AngusJohnson/Clipper2
    REF "Clipper2_${VERSION}"
    SHA512 ce753ae3752b7516a9e0cb23c9788d9533c204819d5451ee50cf3b69a06e24165fa9f5270781764c036ad597eef83cf80532a5bebce5c96f5230179dc3ed499a
    HEAD_REF main
)

# GCC on Linux produces false-positive uninitialized variable warnings.
# Remove -Werror on non-MSVC platforms to allow compilation.
if(NOT MSVC)
    file(READ "${SOURCE_PATH}/CPP/CMakeLists.txt" CMAKELISTS_CONTENT)
    string(REPLACE 
        "target_compile_options(Clipper2 PRIVATE -Wall -Wextra -Wpedantic -Werror)"
        "target_compile_options(Clipper2 PRIVATE -Wall -Wextra -Wpedantic)"
        CMAKELISTS_CONTENT "${CMAKELISTS_CONTENT}")
    string(REPLACE 
        "target_compile_options(Clipper2Z PRIVATE -Wall -Wextra -Wpedantic -Werror)"
        "target_compile_options(Clipper2Z PRIVATE -Wall -Wextra -Wpedantic)"
        CMAKELISTS_CONTENT "${CMAKELISTS_CONTENT}")
    file(WRITE "${SOURCE_PATH}/CPP/CMakeLists.txt" "${CMAKELISTS_CONTENT}")
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}/CPP"
    DISABLE_PARALLEL_CONFIGURE
    OPTIONS
        -DCLIPPER2_EXAMPLES=OFF
        -DCLIPPER2_TESTS=OFF
        -DCLIPPER2_UTILS=ON
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH "lib/cmake/${PORT}")
vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
