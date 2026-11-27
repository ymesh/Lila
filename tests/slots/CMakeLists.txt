# Slots: types, stable ids, rules, overrides, pins, persistence and the Slots geometry node.
add_executable(lila_slots_tests main.cpp test_slots.cpp test_slots_gn.cpp)
target_link_libraries(lila_slots_tests PRIVATE LilaScene doctest::doctest)
add_test(NAME lila_slots_tests COMMAND lila_slots_tests)
