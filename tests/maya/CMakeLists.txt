add_executable(lila_maya_tests
    main.cpp
    test_maya.cpp
    test_maya_blendshape.cpp
    test_maya_progress.cpp
    test_maya_rig.cpp)
target_link_libraries(lila_maya_tests PRIVATE LilaScene lila_ufbx doctest::doctest)
target_compile_definitions(lila_maya_tests PRIVATE LILA_MAYA_TEST_DATA="${CMAKE_SOURCE_DIR}/tests/data/maya")
add_test(NAME lila_maya_tests COMMAND lila_maya_tests)
