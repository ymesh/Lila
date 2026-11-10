# GroomLab's own core and io tests (GoogleTest), run as two ctest entries: everything but the timing suites, and the
# Budget/Performance suites alone (they time parallel loops).
find_package(GTest CONFIG REQUIRED)
find_package(Alembic CONFIG REQUIRED)
add_executable(lila_groomlab_tests
    core/test_async_evaluation.cpp
    core/test_attributes.cpp
    core/test_brush.cpp
    core/test_brush_alpha.cpp
    core/test_brush_pack.cpp
    core/test_budgets.cpp
    core/test_budgets_ops.cpp
    core/test_budgets_paint.cpp
    core/test_budgets_stroke.cpp
    core/test_camera_solve.cpp
    core/test_card_maps.cpp
    core/test_card_separation.cpp
    core/test_cards.cpp
    core/test_cinematic.cpp
    core/test_color_patterns.cpp
    core/test_compat.cpp
    core/test_compound.cpp
    core/test_continuity.cpp
    core/test_correctives.cpp
    core/test_deform.cpp
    core/test_determinism.cpp
    core/test_display.cpp
    core/test_distance_field.cpp
    core/test_evaluator_extras.cpp
    core/test_example_node.cpp
    core/test_expression.cpp
    core/test_feather_cards.cpp
    core/test_feathers.cpp
    core/test_fields.cpp
    core/test_flow.cpp
    core/test_follow.cpp
    core/test_game_exports.cpp
    core/test_geometry.cpp
    core/test_golden.cpp
    core/test_graph.cpp
    core/test_graph_pins.cpp
    core/test_grooming.cpp
    core/test_hair_color.cpp
    core/test_hierarchy.cpp
    core/test_incremental.cpp
    core/test_keyed_params.cpp
    core/test_layers.cpp
    core/test_length_field.cpp
    core/test_locality.cpp
    core/test_lod.cpp
    core/test_look.cpp
    core/test_maps.cpp
    core/test_motion.cpp
    core/test_node_common.cpp
    core/test_operators.cpp
    core/test_parity_nodes.cpp
    core/test_parts.cpp
    core/test_pin_morph.cpp
    core/test_pipeline.cpp
    core/test_plume.cpp
    core/test_presets.cpp
    core/test_preview_invariance.cpp
    core/test_ptex.cpp
    core/test_rachis.cpp
    core/test_random.cpp
    core/test_reference_edit.cpp
    core/test_reference_guides.cpp
    core/test_regions.cpp
    core/test_render_overrides.cpp
    core/test_renderman_look.cpp
    core/test_robustness.cpp
    core/test_scale_export.cpp
    core/test_scale_invariance.cpp
    core/test_scale_with_scalp.cpp
    core/test_scales.cpp
    core/test_scalp_paint.cpp
    core/test_scalp_pieces.cpp
    core/test_script_node.cpp
    core/test_sculpt.cpp
    core/test_sculpt_node.cpp
    core/test_self_collide.cpp
    core/test_semver.cpp
    core/test_session.cpp
    core/test_shapes.cpp
    core/test_sim_attributes.cpp
    core/test_simulation.cpp
    core/test_static.cpp
    core/test_strand_geometry.cpp
    core/test_strand_lod.cpp
    core/test_strand_set.cpp
    core/test_stroke.cpp
    core/test_subdivision.cpp
    core/test_surface_paths.cpp
    core/test_transfer.cpp
    core/test_transfer_markers_edit.cpp
    core/test_transfer_methods.cpp
    core/test_tubes.cpp
    core/test_usd_layout.cpp
    core/test_value_inputs.cpp
    core/test_vrscene_writer.cpp
    core/test_wet.cpp
    io/test_alembic.cpp
    io/test_import_curves.cpp
    io/test_guide_cache.cpp
    io/test_sim_cache.cpp
    example_node_src/example_op.cpp)
target_link_libraries(lila_groomlab_tests PRIVATE groomlab_core lila_groom_evaluator GTest::gtest_main nlohmann_json::nlohmann_json Alembic::Alembic psapi)
target_include_directories(lila_groomlab_tests PRIVATE core ${CMAKE_SOURCE_DIR}/third_party/stb ${CMAKE_SOURCE_DIR}/src/groom/groomlab/core/src)
target_compile_definitions(lila_groomlab_tests PRIVATE "GROOMLAB_TEST_DATA=\"${CMAKE_CURRENT_SOURCE_DIR}/data\""
    "GROOMLAB_SHIPPED_PRESETS=\"${CMAKE_CURRENT_SOURCE_DIR}/data/shipped_presets\""
    "GROOMLAB_SOURCE_ROOT=\"${CMAKE_SOURCE_DIR}/src/groom/groomlab\"")
set_source_files_properties(example_node_src/example_op.cpp PROPERTIES INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/core/example_node)
target_compile_options(lila_groomlab_tests PRIVATE /wd4456 /wd4457 /wd4267 /wd4005 /wd4189)
set_target_properties(lila_groomlab_tests PROPERTIES LILA_NO_UNITY ON CXX_STANDARD 17)
add_test(NAME lila_groomlab_tests COMMAND lila_groomlab_tests --gtest_filter=-Budget.*:Performance.*)
add_test(NAME lila_groomlab_timing COMMAND lila_groomlab_tests --gtest_filter=Budget.*:Performance.*)
set_tests_properties(lila_groomlab_timing PROPERTIES RUN_SERIAL TRUE)

# The look engine's kernels on Lila's Vulkan compute against the CPU's (upstream's GPU tests, a headless renderer).
add_executable(lila_groomlab_gpu_tests gpu/main.cpp gpu/test_look_engine.cpp)
target_link_libraries(lila_groomlab_gpu_tests PRIVATE groomlab_core lila_groom_evaluator lila_groom_look LilaUI GTest::gtest nlohmann_json::nlohmann_json)
target_include_directories(lila_groomlab_gpu_tests PRIVATE core ${CMAKE_SOURCE_DIR}/src/groom/groomlab/core/src)
target_compile_definitions(lila_groomlab_gpu_tests PRIVATE "GROOMLAB_TEST_DATA=\"${CMAKE_CURRENT_SOURCE_DIR}/data\""
    "GROOMLAB_SHIPPED_PRESETS=\"${CMAKE_CURRENT_SOURCE_DIR}/data/shipped_presets\"")
target_compile_options(lila_groomlab_gpu_tests PRIVATE /wd4456 /wd4457 /wd4267 /wd4005 /wd4189)
target_compile_definitions(lila_groomlab_gpu_tests PRIVATE _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING)
set_target_properties(lila_groomlab_gpu_tests PROPERTIES LILA_NO_UNITY ON)
add_test(NAME lila_groomlab_gpu_tests COMMAND lila_groomlab_gpu_tests --gtest_filter=-*Bench*)
set_tests_properties(lila_groomlab_gpu_tests PROPERTIES RUN_SERIAL TRUE)
