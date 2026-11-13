add_executable(lila_rig_tests
    main.cpp
    test_joints.cpp
    test_skin.cpp
    test_weight_paint.cpp
    test_weight_proxy.cpp
    test_constraints.cpp
    test_center.cpp
    test_ik.cpp
    test_ik_limits.cpp
    test_unreal_check.cpp
    test_fbik.cpp
    test_edit_skeleton.cpp
    test_ghosts.cpp
    test_poses.cpp
    test_picker.cpp
    test_spaces.cpp
    test_rig_elements.cpp
    test_select_as.cpp
    test_anim_proxy.cpp
    test_offsets.cpp
    test_pins.cpp
    test_relocate.cpp
    test_spline_chain.cpp
    test_rubber_hose.cpp
    test_fbik_stretch.cpp
    test_hose_reskin.cpp
    test_pin_socket.cpp
    test_strings.cpp
    test_grapple.cpp)
target_link_libraries(lila_rig_tests PRIVATE LilaScene doctest::doctest)
add_test(NAME lila_rig_tests COMMAND lila_rig_tests)

# Viewport side: joint drawing/picking and the weight paint tool, driven headless (offscreen Qt platform).
add_executable(lila_rig_ui_tests ui_main.cpp test_rig_ui.cpp test_picker_ui.cpp test_relocate_extensions.cpp test_rig_handles.cpp)
target_link_libraries(lila_rig_ui_tests PRIVATE LilaUI doctest::doctest)
# relocateScene's classification guard loads every built extension.
foreach(_ext lila_destruction_extension lila_foliage_extension lila_ground_tiles_extension lila_lights_extension lila_masonry_extension
        lila_openusd_extension lila_overlap_extension lila_physics_animation_extension lila_puppeteer_extension lila_rig_templates_extension
        lila_settlement_extension lila_stagehand_extension lila_terrain_extension)
    if(TARGET ${_ext})
        add_dependencies(lila_rig_ui_tests ${_ext})
    endif()
endforeach()
target_compile_definitions(lila_rig_ui_tests PRIVATE LILA_TEST_EXTENSIONS_DIR="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions")
add_test(NAME lila_rig_ui_tests COMMAND lila_rig_ui_tests)
