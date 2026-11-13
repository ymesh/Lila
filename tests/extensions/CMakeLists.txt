# The sample extension, built as a package for the tests on their own so these run without the root adding extensions/examples.
lila_add_extension(lila_test_hello_extension ID hello
    MANIFEST ${CMAKE_SOURCE_DIR}/extensions/examples/hello/extension.json
    SOURCES ${CMAKE_SOURCE_DIR}/extensions/examples/hello/Hello.cpp
    OUTPUT_DIR ${CMAKE_BINARY_DIR}/tests/extensions/$<CONFIG>/native)

lila_add_extension(lila_test_scene_points_extension ID scenePoints
    MANIFEST ${CMAKE_SOURCE_DIR}/extensions/examples/scene_points/extension.json
    SOURCES ${CMAKE_SOURCE_DIR}/extensions/examples/scene_points/ScenePoints.cpp
    OUTPUT_DIR ${CMAKE_BINARY_DIR}/tests/extensions/$<CONFIG>/native)

add_executable(lila_extensions_tests
    main.cpp
    test_extensions.cpp
    test_python_nodes.cpp
    test_scene_points.cpp
    test_sdk.cpp)
target_link_libraries(lila_extensions_tests PRIVATE LilaScene doctest::doctest)
add_dependencies(lila_extensions_tests lila_test_hello_extension lila_test_scene_points_extension)
target_compile_definitions(lila_extensions_tests PRIVATE
    LILA_TEST_HELLO_PACKAGE="${CMAKE_BINARY_DIR}/tests/extensions/$<CONFIG>/native/hello.lilaext"
    LILA_TEST_SCENE_POINTS_PACKAGE="${CMAKE_BINARY_DIR}/tests/extensions/$<CONFIG>/native/scenePoints.lilaext"
    LILA_TEST_PACKAGES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/packages"
    LILA_TEST_EXAMPLES_DIR="${CMAKE_SOURCE_DIR}/extensions/examples")

if(WIN32)
    cmake_path(GET Python_EXECUTABLE PARENT_PATH _pyhome)
    add_custom_command(TARGET lila_extensions_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_pyhome}/python3.dll ${_pyhome}/python${Python_VERSION_MAJOR}${Python_VERSION_MINOR}.dll $<TARGET_FILE_DIR:lila_extensions_tests>)
endif()

add_test(NAME lila_extensions_tests COMMAND lila_extensions_tests)

# The viewport side: the sample viewport extension loaded into the UI library's registries, and the same from Python.
lila_add_extension(lila_test_viewport_points_extension ID viewportPoints UI
    MANIFEST ${CMAKE_SOURCE_DIR}/extensions/examples/viewport_points/extension.json
    RESOURCES ${CMAKE_SOURCE_DIR}/extensions/examples/viewport_points/resources
    SOURCES ${CMAKE_SOURCE_DIR}/extensions/examples/viewport_points/ViewportPoints.cpp
    OUTPUT_DIR ${CMAKE_BINARY_DIR}/tests/extensions/$<CONFIG>/native)
add_executable(lila_viewport_extension_tests main.cpp test_viewport_points.cpp)
target_link_libraries(lila_viewport_extension_tests PRIVATE LilaUI doctest::doctest)
add_dependencies(lila_viewport_extension_tests lila_test_viewport_points_extension)
target_compile_definitions(lila_viewport_extension_tests PRIVATE
    LILA_TEST_VIEWPORT_POINTS_PACKAGE="${CMAKE_BINARY_DIR}/tests/extensions/$<CONFIG>/native/viewportPoints.lilaext")
if(WIN32)
    add_custom_command(TARGET lila_viewport_extension_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_pyhome}/python3.dll ${_pyhome}/python${Python_VERSION_MAJOR}${Python_VERSION_MINOR}.dll $<TARGET_FILE_DIR:lila_viewport_extension_tests>)
endif()
add_test(NAME lila_viewport_extension_tests COMMAND lila_viewport_extension_tests)
add_test(NAME lila_viewport_extension_python COMMAND lila_viewport_extension_tests --no-skip "--test-case=viewport extension points from Python*")
add_test(NAME lila_extensions_python_nodes COMMAND lila_extensions_tests --no-skip "--test-case=python nodes API*")
add_test(NAME lila_extensions_python_registries COMMAND lila_extensions_tests --no-skip "--test-case=scene extension points from Python*")

# The SDK package end to end: install it, build the samples as an outside project against the install alone, then
# load that build of scenePoints in the scene extension points test.
set(_sdk_prefix ${CMAKE_BINARY_DIR}/tests/extensions/sdk/install)
set(_sdk_build ${CMAKE_BINARY_DIR}/tests/extensions/sdk/examples)
add_test(NAME lila_sdk_install
    COMMAND ${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR} --config $<CONFIG> --component sdk --prefix ${_sdk_prefix})
add_test(NAME lila_sdk_build_examples
    COMMAND ${CMAKE_COMMAND} -DSDK=${_sdk_prefix} -DSOURCE=${_sdk_prefix}/examples -DBUILD=${_sdk_build} -DCONFIG=$<CONFIG>
            -DCXX=${CMAKE_CXX_COMPILER} -DMAKE=${CMAKE_MAKE_PROGRAM} -DDEPS=${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/SdkConsumer.cmake)
add_test(NAME lila_sdk_load_examples
    COMMAND lila_extensions_tests "--test-case=scene extension points: an extension*")
set_tests_properties(lila_sdk_install PROPERTIES FIXTURES_SETUP lila_sdk_installed)
set_tests_properties(lila_sdk_build_examples PROPERTIES FIXTURES_REQUIRED lila_sdk_installed FIXTURES_SETUP lila_sdk_built)
set_tests_properties(lila_sdk_load_examples PROPERTIES FIXTURES_REQUIRED lila_sdk_built
    ENVIRONMENT "LILA_TEST_SCENE_POINTS_PACKAGE=${_sdk_build}/$<CONFIG>/extensions/scenePoints.lilaext")

# An extension registering through the overloads that take no context: everything must still go when it unloads.
lila_add_extension(lila_test_plain_owner_extension ID plainOwner UI
    MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/plain_owner/extension.json
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/plain_owner/PlainOwner.cpp
    OUTPUT_DIR ${CMAKE_BINARY_DIR}/tests/extensions/$<CONFIG>/native)
add_executable(lila_ui_extension_tests main.cpp test_plain_owner.cpp)
target_link_libraries(lila_ui_extension_tests PRIVATE LilaUI doctest::doctest)
add_dependencies(lila_ui_extension_tests lila_test_plain_owner_extension)
target_compile_definitions(lila_ui_extension_tests PRIVATE
    LILA_TEST_PLAIN_OWNER_PACKAGE="${CMAKE_BINARY_DIR}/tests/extensions/$<CONFIG>/native/plainOwner.lilaext")
add_test(NAME lila_ui_extension_tests COMMAND lila_ui_extension_tests)
