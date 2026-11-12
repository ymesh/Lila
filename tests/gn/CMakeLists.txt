add_executable(lila_gn_tests
    main.cpp
    test_clipboard.cpp
    test_gn.cpp
    test_gn_groups.cpp
    test_gn_relocate.cpp
    test_gn_sim.cpp
    test_gn_tool.cpp
    test_gn_voronoi.cpp
    test_nodes.cpp
    test_nodes_meshops.cpp
    test_nodes_topology.cpp
    test_nodes_topology2.cpp
    test_nodes_field.cpp
    test_nodes_view.cpp
    test_nodes_curveops.cpp
    test_nodes_game.cpp
    test_nodes_procedural.cpp
    test_nodes_volume.cpp
    test_nodes_grid.cpp
    test_nodes_wrangle.cpp
    test_nodes_zones.cpp
    test_nodes_canvas_zones.cpp
    test_routing.cpp
    test_ties.cpp
    test_nodes_build.cpp
    test_nodes_deform2.cpp
    test_nodes_curve2.cpp
    test_nodes_instances2.cpp
    test_nodes_fields2.cpp
    test_nodes_scene2.cpp)
target_link_libraries(lila_gn_tests PRIVATE LilaScene doctest::doctest)
add_test(NAME lila_gn_tests COMMAND lila_gn_tests)

# Throwaway visual check of the node editor: builds a group, opens the editor offscreen and grabs it to a PNG.
add_executable(lila_gn_demo EXCLUDE_FROM_ALL demo.cpp)
target_link_libraries(lila_gn_demo PRIVATE LilaUI)
