add_executable(lila_deform_tests
    main.cpp
    test_bend_bones.cpp
    test_deform_eval.cpp
    test_deform_math.cpp
    test_deformers.cpp
    test_fields.cpp
    test_mesh_cache.cpp
    test_morphs.cpp
    test_rigid_bind.cpp
    test_shape.cpp
    test_shape_deformers.cpp
    test_skin_bake.cpp
    test_surface.cpp
    test_texture_deformers.cpp)
target_link_libraries(lila_deform_tests PRIVATE LilaScene lila_ufbx doctest::doctest)
add_test(NAME lila_deform_tests COMMAND lila_deform_tests)

# GPU deformers against the CPU reference; the GPU cases pass without checking when no Vulkan device can be created.
add_executable(lila_deform_gpu_tests
    gpu_main.cpp
    test_deform_gpu.cpp)
target_link_libraries(lila_deform_gpu_tests PRIVATE LilaUI doctest::doctest)
add_test(NAME lila_deform_gpu_tests COMMAND lila_deform_gpu_tests)
