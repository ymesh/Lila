vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO ThatOpen/engine_web-ifc
    REF "${VERSION}"
    SHA512 fa72a072b66edb41f3ffd90b6684de21a0b02ee272e4686c366fe71ada11f870ca425afdf80b37295aa1d0ed8fb732a31365eaecb7364f857110c0f12b69543c
    HEAD_REF main
)

# The commits web-ifc's CMakeLists fetches, so the build needs no network.
function(web_ifc_dep name repo ref sha)
    vcpkg_from_github(OUT_SOURCE_PATH dep REPO ${repo} REF ${ref} SHA512 ${sha} HEAD_REF master)
    string(TOUPPER "${name}" upper)
    set(WEB_IFC_DEP_OPTIONS ${WEB_IFC_DEP_OPTIONS} "-DFETCHCONTENT_SOURCE_DIR_${upper}=${dep}" PARENT_SCOPE)
    set(DEP_${name} "${dep}" PARENT_SCOPE)
endfunction()
web_ifc_dep(fastfloat fastfloat/fast_float b0ab987b3dfdde13fa1915f65ef2a5c068d9208c 695c77144819a3dfbe90a08efd592e969082b01349a1082b98144a3f87c65a5f89cbdeb3aa9a034b90044719056c35b4e862757fdfc24a41b5ae310a913f8ca8)
web_ifc_dep(tinynurbs QuimMoya/tinynurbs 47115cd9b6e922b27bbc4ab01fdeac2e9ea597a4 16089a32f16d9ae27fa7588f6d978f102fc989a60e5708f281eace459fdf96600d5686760dcbd006e2608d92e9edf38f92d674c68410123fb5049b4c9bc19a73)
web_ifc_dep(tinycpptest kovacsv/TinyCppTest 12e42c8ac6e032ce450fb3f772ebdfd1ddc6008c 928f30272f8d7c90b4bd142f37e0b941348ca6a86d82028699d2b507628024d47912baa1b5c58a21345c6c87329fd89879e88f1b8e1e855c00ba16cc2fcf5a1f)
web_ifc_dep(glm g-truc/glm 8d1fd52e5ab5590e2c81768ace50c72bae28f2ed e05779baab888e13e64f0f8bd955887398e8de2a7f05360a1e9e84a5e91cf21e9be0bccaaef94ca3511f418e91a509f78a92e3dd6ab9a3cb045fa3e84c57d7d8)
web_ifc_dep(earcut mapbox/earcut.hpp c68c8835ccff2b7532d31d8fa8dfcf398f629498 0de63ffbf384e5816c3baba4d066e7669daf2702af580821424ef1ba4dc78360aa0608b3343b62f10bf58e83a5a41fae841a55b030b9965d514360bf1b95c43f)
web_ifc_dep(cdt artem-ogre/CDT 2068d015b9db3c92481e869b0c1f669b96a1d70a 41035ada4b195a20ff986df87cb39acc1b7311c464ff02e37760060f18b256c92678ef61b0ff694cf360dae3f43c020160c17b454c48a339074ec3db4f7fb442)
web_ifc_dep(spdlog gabime/spdlog 79524ddd08a4ec981b7fea76afd08ee05f83755d 1b3fb02bcf8f6c09ce403112eeee9b30fb0235b72f654fef70d39cd0665abe69a3eca1d313e0464ab648d59a8dee3415ee788b53125af2afb1276b77ccaebaa0)
web_ifc_dep(stduuid mariusbancila/stduuid 3afe7193facd5d674de709fccc44d5055e144d7a 3e250aa6075c23c555c3859077ce24dccde8f062168b91a70e1c311cd1ae4d10a7b339f5916fab748c684131378d383d7a9556f772a33ab5359ab414b7873c4e)
web_ifc_dep(unordered_dense martinus/unordered_dense 1c8636e810d7e4a485d17bb9cbd6dccb5fe4e284 5e8a3c95cf99c1f9f63d38c0cf652c3aae84aa180eefbfd17c97ffcbcad04563fb6fc2d56d5608c08f13b8350b7581e5a4f6c52601e576774a75c447692b159a)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}/src/cpp"
    OPTIONS
        -DFETCHCONTENT_FULLY_DISCONNECTED=ON
        -DRELEASE=ON
        ${WEB_IFC_DEP_OPTIONS}
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/web-ifc)

# Every header: upstream installs leave out some its public headers include (boolean-utils, mesh_utils.h).
file(INSTALL "${SOURCE_PATH}/src/cpp/web-ifc/" DESTINATION "${CURRENT_PACKAGES_DIR}/include/web-ifc" FILES_MATCHING PATTERN "*.h")

# glm comes from vcpkg's own port; the other bundled headers web-ifc's headers include go beside them, out of the way of any port.
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/include/glm" "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")
set(third "${CURRENT_PACKAGES_DIR}/include/web-ifc/third_party")
file(INSTALL "${DEP_earcut}/include/mapbox" DESTINATION "${third}")
file(INSTALL "${DEP_tinynurbs}/include/tinynurbs" DESTINATION "${third}")
file(INSTALL "${DEP_unordered_dense}/include/ankerl" DESTINATION "${third}")
file(INSTALL "${DEP_spdlog}/include/spdlog" DESTINATION "${third}")
file(INSTALL "${DEP_fastfloat}/include/fast_float" DESTINATION "${third}")
file(INSTALL "${DEP_cdt}/CDT/include/" DESTINATION "${third}/CDT")
file(INSTALL "${DEP_stduuid}/include/uuid.h" DESTINATION "${third}")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.md")
