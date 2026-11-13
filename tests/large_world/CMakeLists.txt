# Camera-relative rendering and the CPU interaction paths far from the world origin. The GPU cases pass without
# checking when no Vulkan device can be created.
add_executable(lila_large_world_tests
    main.cpp
    test_camera_relative.cpp
    test_large_world_cpu.cpp)
target_link_libraries(lila_large_world_tests PRIVATE LilaUI doctest::doctest)
add_test(NAME lila_large_world_tests COMMAND lila_large_world_tests)
