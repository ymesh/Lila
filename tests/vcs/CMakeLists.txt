add_executable(lila_vcs_tests
    main.cpp
    test_git.cpp
    test_perforce.cpp
    test_svn.cpp)
target_link_libraries(lila_vcs_tests PRIVATE LilaScene doctest::doctest)
add_test(NAME lila_vcs_tests COMMAND lila_vcs_tests)
