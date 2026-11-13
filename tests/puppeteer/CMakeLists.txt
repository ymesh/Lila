# The Puppeteer extension's rig side: its maths directly, the rest loaded as the extension.
add_executable(lila_puppeteer_tests main.cpp test_puppeteer_math.cpp test_puppeteer_rig.cpp test_spinner.cpp test_spinner_draw.cpp test_strings_pose.cpp test_hands.cpp test_grapple_tool.cpp test_props.cpp)
target_link_libraries(lila_puppeteer_tests PRIVATE LilaUI doctest::doctest)
target_include_directories(lila_puppeteer_tests PRIVATE ${CMAKE_SOURCE_DIR}/extensions/Puppeteer)
add_dependencies(lila_puppeteer_tests lila_puppeteer_extension lila_rig_templates_extension)
target_compile_definitions(lila_puppeteer_tests PRIVATE LILA_TEST_RIG_TEMPLATES_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/RigTemplates.lilaext")
target_compile_definitions(lila_puppeteer_tests PRIVATE LILA_TEST_PUPPETEER_PACKAGE="$<TARGET_FILE_DIR:lila_puppeteer_extension>")
if(WIN32)
    cmake_path(GET Python_EXECUTABLE PARENT_PATH _pyhome)
    add_custom_command(TARGET lila_puppeteer_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_pyhome}/python3.dll ${_pyhome}/python${Python_VERSION_MAJOR}${Python_VERSION_MINOR}.dll $<TARGET_FILE_DIR:lila_puppeteer_tests>)
endif()
add_test(NAME lila_puppeteer_tests COMMAND lila_puppeteer_tests)
