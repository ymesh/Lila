# Puppeteer's animation tools: the key maths and walk solver directly, the commands through the loaded package.
add_executable(lila_puppeteer_anim_tests main.cpp test_key_math.cpp test_walk.cpp test_anim_commands.cpp test_tracks.cpp)
target_link_libraries(lila_puppeteer_anim_tests PRIVATE LilaUI doctest::doctest)
add_dependencies(lila_puppeteer_anim_tests lila_puppeteer_extension)
target_compile_definitions(lila_puppeteer_anim_tests PRIVATE
    LILA_TEST_PUPPETEER_PACKAGE="$<TARGET_FILE_DIR:lila_puppeteer_extension>"
    LILA_TEST_MANNY_FBX="${CMAKE_SOURCE_DIR}/tests/data/rig/SKM_Manny_skeleton.fbx")
if(WIN32)
    cmake_path(GET Python_EXECUTABLE PARENT_PATH _pyhome)
    add_custom_command(TARGET lila_puppeteer_anim_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_pyhome}/python3.dll ${_pyhome}/python${Python_VERSION_MAJOR}${Python_VERSION_MINOR}.dll $<TARGET_FILE_DIR:lila_puppeteer_anim_tests>)
endif()
add_test(NAME lila_puppeteer_anim_tests COMMAND lila_puppeteer_anim_tests)
