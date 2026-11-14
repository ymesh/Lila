add_executable(lila_retopo_tests
    main.cpp
    test_anglesnap.cpp
    test_border.cpp
    test_controls.cpp
    test_corpus.cpp
    test_liverelax.cpp
    test_patch.cpp
    test_repair.cpp
    test_poles.cpp
    test_retopo.cpp
    test_flow.cpp
    test_tools.cpp
    test_topology.cpp)
target_link_libraries(lila_retopo_tests PRIVATE LilaScene doctest::doctest)
target_include_directories(lila_retopo_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests)
add_test(NAME lila_retopo_tests COMMAND lila_retopo_tests)
