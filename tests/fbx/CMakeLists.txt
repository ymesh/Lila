find_package(ZLIB REQUIRED)

add_executable(lila_fbx_tests
    main.cpp
    test_fbx.cpp
    test_fbx_instances.cpp
    test_fbx_rotation.cpp
    test_fbx_camera.cpp
    test_fbx_georef.cpp
    test_fbx_light.cpp
    test_fbx_spaces.cpp
    test_clean_bake.cpp
    test_game_export.cpp
    test_fbx_lod.cpp
    test_precision.cpp)
target_link_libraries(lila_fbx_tests PRIVATE LilaScene lila_ufbx ZLIB::ZLIB doctest::doctest)
add_test(NAME lila_fbx_tests COMMAND lila_fbx_tests)
