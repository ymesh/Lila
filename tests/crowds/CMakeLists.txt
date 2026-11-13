# The Crowds extension: its runtime compiled in directly, and the package driven through its commands.
set(_crowds ${CMAKE_SOURCE_DIR}/extensions/Crowds)
add_executable(lila_crowd_tests main.cpp test_core.cpp test_scene.cpp
    ${_crowds}/CrowdCore.cpp ${_crowds}/Orca.cpp)
target_include_directories(lila_crowd_tests PRIVATE ${_crowds})
target_link_libraries(lila_crowd_tests PRIVATE LilaUI doctest::doctest)
add_dependencies(lila_crowd_tests lila_crowds_extension lila_rig_templates_extension)
target_compile_definitions(lila_crowd_tests PRIVATE
    LILA_TEST_CROWDS_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/Crowds.lilaext"
    LILA_TEST_RIGTEMPLATES_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/RigTemplates.lilaext")
add_test(NAME lila_crowd_tests COMMAND lila_crowd_tests)
