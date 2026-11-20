add_executable(lila_tex_tests
    main.cpp
    test_brush_tilt.cpp
    test_brush_stroke_methods.cpp
    test_tex_commands.cpp
    test_tex_eval.cpp
    test_tex_files.cpp
    test_tex_graph.cpp
    test_tex_layers.cpp
    test_tex_library.cpp
    test_material_library.cpp
    test_tex_paint_buffer.cpp
    test_tex_paint_data.cpp
    test_tex_painter.cpp
    test_tex_particles.cpp
    test_tex_pipeline.cpp
    test_tex_plan.cpp
    test_tex_projection.cpp
    test_tex_decal_layers.cpp
    test_tex_layer_clipboard.cpp
    test_tex_material_from_maps.cpp
    test_tex_more_nodes.cpp
    test_tex_ptex.cpp
    test_tex_patch_fill.cpp
    test_tex_patch_fill_seams.cpp
    test_tex_variants.cpp
    test_tex_export_presets.cpp
    test_tex_routing.cpp
    test_tex_sequence.cpp
    test_tex_shading_channels.cpp
    test_tex_material_sets.cpp
    test_tex_snapshots.cpp
    test_tex_ties.cpp)
target_link_libraries(lila_tex_tests PRIVATE LilaScene doctest::doctest)
target_compile_definitions(lila_tex_tests PRIVATE LILA_TEST_TEX_DATA="${CMAKE_SOURCE_DIR}/tests/data/tex")
add_dependencies(lila_tex_tests lila_slang_runtime)
add_test(NAME lila_tex_tests COMMAND lila_tex_tests)

# The GPU evaluator against the CPU reference, and texture sets in the viewport; cases pass without checking when no
# Vulkan device can be created.
add_executable(lila_tex_gpu_tests
    gpu_main.cpp
    test_material_thumbs.cpp
    test_tex_displacement.cpp
    test_tex_pathtrace.cpp
    test_thumbs_live_viewport.cpp
    test_tex_gpu.cpp
    test_tex_paint.cpp
    test_tex_paint_symmetry.cpp
    test_tex_paint_tools.cpp
    test_tex_stencil.cpp
    test_tex_live_projection.cpp
    test_tex_path.cpp
    test_tex_pick.cpp
    test_tex_tilt.cpp
    test_tex_selection.cpp
    test_tex_quick_mask_overlay.cpp
    test_tex_particles_gpu.cpp
    test_tex_gradient.cpp
    test_tex_retarget.cpp
    test_tex_image_editor.cpp
    test_tex_multi_stroke.cpp
    test_tex_brush_limits.cpp
    test_tex_uv_mirror.cpp
    test_tex_tileable.cpp
    test_tex_sculpt_paint.cpp
    test_tex_stroke_edit.cpp
    test_tex_vt.cpp)
target_link_libraries(lila_tex_gpu_tests PRIVATE LilaUI doctest::doctest)
add_dependencies(lila_tex_gpu_tests lila_slang_runtime)
add_test(NAME lila_tex_gpu_tests COMMAND lila_tex_gpu_tests)

# Every shipped .lilamat (resources/materials and the material packs in extensions/) evaluated on the CPU and linted. It
# takes about 25 minutes of CPU, so it is labelled slow: day-to-day runs use `ctest -LE slow`, `ctest -L slow` runs it alone.
add_executable(lila_material_pack_tests
    main.cpp
    test_material_packs.cpp)
target_link_libraries(lila_material_pack_tests PRIVATE LilaScene doctest::doctest)
add_dependencies(lila_material_pack_tests lila_slang_runtime)
# The materials case compiles a CPU kernel per pass, so it runs as four shards that ctest -j runs side by side.
foreach(_shard RANGE 3)
    add_test(NAME lila_material_pack_tests_${_shard} COMMAND lila_material_pack_tests "--test-case=*every shipped material loads*")
    set_tests_properties(lila_material_pack_tests_${_shard} PROPERTIES LABELS slow ENVIRONMENT "LILA_MATERIAL_SHARD=${_shard}/4")
endforeach()
add_test(NAME lila_material_pack_tests_smart COMMAND lila_material_pack_tests "--test-case=*smart material and mask*")
set_tests_properties(lila_material_pack_tests_smart PROPERTIES LABELS slow)
