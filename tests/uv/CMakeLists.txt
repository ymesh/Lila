add_executable(lila_uv_tests
    main.cpp
    test_uv.cpp
    test_uv_opt.cpp
    test_pack.cpp
    test_live.cpp
    test_bevel_uv.cpp
    test_hose.cpp
    test_final_mesh.cpp
    test_uv_flow.cpp)
target_link_libraries(lila_uv_tests PRIVATE LilaScene doctest::doctest)
add_test(NAME lila_uv_tests COMMAND lila_uv_tests)

add_executable(lila_uv_cmd_tests
    main_cmds.cpp
    test_uv_commands.cpp
    test_uv_opt_commands.cpp
    test_live_commands.cpp
    test_uv_symmetry.cpp)
target_link_libraries(lila_uv_cmd_tests PRIVATE LilaScene doctest::doctest)
add_test(NAME lila_uv_cmd_tests COMMAND lila_uv_cmd_tests)

# Visual check of the UV editor; not built by default.
add_executable(lila_uv_editor_demo EXCLUDE_FROM_ALL uv_editor_demo.cpp)
target_link_libraries(lila_uv_editor_demo PRIVATE LilaUI)
