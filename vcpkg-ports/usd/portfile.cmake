# vcpkg's usd port plus a python feature: the pxr bindings built against the Python Lila embeds, installed as a package
# under lib/python (debug/lib/python) for Lila to deploy beside the libraries it loads.

# USD plugins do not produce .lib
set(VCPKG_POLICY_DLLS_WITHOUT_LIBS enabled)

# Proper support for a true static usd build is left as a future port improvement.
vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)

# zero-pad version components to two digits
string(REPLACE "." ";" version_components ${VERSION})
foreach(component IN LISTS version_components)
    string(LENGTH ${component} component_length)
    if(component_length LESS 2)
        list(APPEND USD_VERSION "0${component}")
    else()
        list(APPEND USD_VERSION "${component}")
    endif()
endforeach()
string(JOIN "." USD_VERSION ${USD_VERSION})

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO PixarAnimationStudios/OpenUSD
    REF "v${USD_VERSION}"
    SHA512 82f7bb4f77b295be79cd36f591f242276d36c0d589b0fca383344c27e70daf7e29151bf587528bb2fc2b2a8b974387a253993ace3caf5f79ae70f9ebbc71cf1a
    HEAD_REF release
    PATCHES
        003-fix-dep.patch
        004-fix_cmake_package.patch
        007-fix_cmake_hgi_interop.patch
        008-fix_clang8_compiler_error.patch
        009-vcpkg_install_folder_conventions.patch
        010-cmake_export_plugin_as_modules.patch
        011-hgivulkan-cached-readback.patch
)

# Clang 21 at -O0 fails about one build in five on Sdf's text file parser ("symbol ... is already defined" for PEGTL's
# sor<...>::match instances, which change with address randomization); at -O1 and above it never did. Optimize that
# one file under Clang.
file(APPEND "${SOURCE_PATH}/pxr/usd/sdf/CMakeLists.txt" [=[

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set_source_files_properties(textFileFormatParser.cpp PROPERTIES COMPILE_OPTIONS "-O1")
endif()
]=])

# OpenExec builds with hidden visibility on Linux, tested with GCC only (Apple, the Clang platform, is left out). Clang
# reads ARCH_EXPORT_TYPE as type_visibility, which leaves the members of the explicitly instantiated templates hidden:
# libusd_exec.so then fails to load, missing Vdf_VectorImplContiguous<TfToken>'s constructor from libusd_vdf.so. Leave
# Clang out as Apple is.
file(READ "${SOURCE_PATH}/pxr/exec/CMakeLists.txt" _exec_cmake)
string(REPLACE "if(UNIX AND NOT APPLE AND NOT PXR_BUILD_MONOLITHIC)"
    "if(UNIX AND NOT APPLE AND NOT PXR_BUILD_MONOLITHIC AND NOT CMAKE_CXX_COMPILER_ID MATCHES \"Clang\")"
    _exec_cmake_fixed "${_exec_cmake}")
if(_exec_cmake_fixed STREQUAL _exec_cmake)
    message(FATAL_ERROR "pxr/exec/CMakeLists.txt changed: the Clang visibility fix no longer applies")
endif()
file(WRITE "${SOURCE_PATH}/pxr/exec/CMakeLists.txt" "${_exec_cmake_fixed}")

# Changes accompanying 003-fix-dep.patch
file(REMOVE
    "${SOURCE_PATH}/cmake/modules/FindOpenColorIO.cmake"
    "${SOURCE_PATH}/pxr/imaging/hgiVulkan/vk_mem_alloc.cpp"
    "${SOURCE_PATH}/pxr/imaging/hgiVulkan/vk_mem_alloc.h"
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        imaging        PXR_BUILD_IMAGING
        imaging        PXR_BUILD_USD_IMAGING
        imaging        PXR_ENABLE_GL_SUPPORT
        materialx      PXR_ENABLE_MATERIALX_SUPPORT
        openimageio    PXR_BUILD_OPENIMAGEIO_PLUGIN
        vulkan         PXR_ENABLE_VULKAN_SUPPORT
        python         PXR_ENABLE_PYTHON_SUPPORT
        embree         PXR_BUILD_EMBREE_PLUGIN
)

if("embree" IN_LIST FEATURES)
    list(APPEND FEATURE_OPTIONS "-DEMBREE_LOCATION=${CURRENT_INSTALLED_DIR}")
endif()

if("python" IN_LIST FEATURES)
    # Not a vcpkg dependency: the bindings must match the interpreter Lila embeds, not vcpkg's python3. vcpkg passes
    # LILA_PYTHON_DIR through only when it is listed in VCPKG_KEEP_ENV_VARS.
    set(LILA_PYTHON_DIR "$ENV{LILA_PYTHON_DIR}")
    if(NOT LILA_PYTHON_DIR)
        set(LILA_PYTHON_DIR "C:/Python314")
    endif()
    file(TO_CMAKE_PATH "${LILA_PYTHON_DIR}" LILA_PYTHON_DIR)
    if(VCPKG_TARGET_IS_WINDOWS)
        file(GLOB LILA_PYTHON_LIB "${LILA_PYTHON_DIR}/libs/python3[0-9]*.lib")
        list(FILTER LILA_PYTHON_LIB EXCLUDE REGEX "_d\\.lib$")
        set(LILA_PYTHON_EXE "${LILA_PYTHON_DIR}/python.exe")
        set(LILA_PYTHON_INCLUDE "${LILA_PYTHON_DIR}/include")
    else()
        # <prefix>/bin/python3, <prefix>/include/python3.x and <prefix>/lib/libpython3.x.so (a --enable-shared build,
        # such as scripts/fetch-python.sh's).
        file(GLOB LILA_PYTHON_LIB "${LILA_PYTHON_DIR}/lib/libpython3.[0-9]*.so")
        file(GLOB LILA_PYTHON_INCLUDE LIST_DIRECTORIES true "${LILA_PYTHON_DIR}/include/python3.[0-9]*")
        set(LILA_PYTHON_EXE "${LILA_PYTHON_DIR}/bin/python3")
    endif()
    if(NOT EXISTS "${LILA_PYTHON_EXE}" OR NOT LILA_PYTHON_LIB OR NOT LILA_PYTHON_INCLUDE)
        message(FATAL_ERROR "usd[python] needs a Python install with headers and import libraries at ${LILA_PYTHON_DIR}")
    endif()
    list(APPEND FEATURE_OPTIONS
        "-DPython3_EXECUTABLE=${LILA_PYTHON_EXE}"
        "-DPython3_INCLUDE_DIR=${LILA_PYTHON_INCLUDE}"
        "-DPython3_LIBRARY=${LILA_PYTHON_LIB}"
        -DPython3_FIND_REGISTRY=NEVER
        -DPXR_PYTHON_INSTALL_DIR=lib/python
        -DPXR_BUILD_USDVIEW:BOOL=OFF
    )
endif()

vcpkg_cmake_configure(
    SOURCE_PATH ${SOURCE_PATH}
    OPTIONS ${FEATURE_OPTIONS}
        -DPXR_BUILD_DOCUMENTATION:BOOL=OFF
        -DPXR_BUILD_EXAMPLES:BOOL=OFF
        -DPXR_BUILD_TESTS:BOOL=OFF
        -DPXR_BUILD_TUTORIALS:BOOL=OFF
        -DPXR_BUILD_USD_TOOLS:BOOL=OFF

        -DPXR_BUILD_ALEMBIC_PLUGIN:BOOL=OFF
        -DPXR_BUILD_DRACO_PLUGIN:BOOL=OFF
        -DPXR_BUILD_PRMAN_PLUGIN:BOOL=OFF

        -DPXR_ENABLE_OPENVDB_SUPPORT:BOOL=OFF
        -DPXR_ENABLE_PTEX_SUPPORT:BOOL=OFF

        -DPXR_PREFER_SAFETY_OVER_SPEED:BOOL=ON

        -DPXR_USE_DEBUG_PYTHON:BOOL=OFF

        # USD never includes GNUInstallDirs, but its install rules use these.
        -DCMAKE_INSTALL_BINDIR:STRING=bin
        -DCMAKE_INSTALL_LIBDIR:STRING=lib
    MAYBE_UNUSED_VARIABLES
        PXR_ENABLE_PTEX_SUPPORT
        PXR_USE_PYTHON_3
        PYTHON_EXECUTABLE
        Python3_FIND_REGISTRY
        PXR_BUILD_USDVIEW
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()

# Handle debug path for USD plugins
if(NOT DEFINED VCPKG_BUILD_TYPE OR VCPKG_BUILD_TYPE STREQUAL "debug")
    file(GLOB_RECURSE debug_targets
        "${CURRENT_PACKAGES_DIR}/debug/share/pxr/*-debug.cmake"
        )
    foreach(debug_target IN LISTS debug_targets)
        file(READ "${debug_target}" contents)
        string(REPLACE "\${_IMPORT_PREFIX}/plugin" "\${_IMPORT_PREFIX}/debug/plugin" contents "${contents}")
        file(WRITE "${debug_target}" "${contents}")
    endforeach()
endif()

vcpkg_cmake_config_fixup(PACKAGE_NAME "pxr")

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
)

if(VCPKG_TARGET_IS_WINDOWS)
    # Move all dlls to bin
    file(GLOB RELEASE_DLL ${CURRENT_PACKAGES_DIR}/lib/*.dll)
    file(MAKE_DIRECTORY ${CURRENT_PACKAGES_DIR}/bin)
    if(NOT VCPKG_BUILD_TYPE)
      file(GLOB DEBUG_DLL ${CURRENT_PACKAGES_DIR}/debug/lib/*.dll)
      file(MAKE_DIRECTORY ${CURRENT_PACKAGES_DIR}/debug/bin)
    endif()
    foreach(CURRENT_FROM ${RELEASE_DLL} ${DEBUG_DLL})
        string(REPLACE "/lib/" "/bin/" CURRENT_TO ${CURRENT_FROM})
        file(RENAME ${CURRENT_FROM} ${CURRENT_TO})
    endforeach()

    function(file_replace_regex filename match_string replace_string)
        file(READ ${filename} _contents)
        string(REGEX REPLACE "${match_string}" "${replace_string}" _contents "${_contents}")
        file(WRITE ${filename} "${_contents}")
    endfunction()

    # fix dll path for cmake
    if(NOT VCPKG_BUILD_TYPE)
      file_replace_regex(${CURRENT_PACKAGES_DIR}/share/pxr/pxrTargets-debug.cmake "debug/lib/([a-zA-Z0-9_]+)\\.dll" "debug/bin/\\1.dll")
    endif()
    file_replace_regex(${CURRENT_PACKAGES_DIR}/share/pxr/pxrTargets-release.cmake "lib/([a-zA-Z0-9_]+)\\.dll" "bin/\\1.dll")
endif()

# Handle copyright
vcpkg_install_copyright(FILE_LIST ${SOURCE_PATH}/LICENSE.txt)

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
