add_executable(lila_seq_tests
    main.cpp
    test_seq.cpp
    test_seq_nodes.cpp)
target_link_libraries(lila_seq_tests PRIVATE LilaScene doctest::doctest)
add_test(NAME lila_seq_tests COMMAND lila_seq_tests)
