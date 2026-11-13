# The OpenUSD extension loaded from its staged package next to lila.exe and driven through importUsd and exportUsd:
# round trips of Lila scenes, imports of the hand-written stages in fixtures/, live stages and their Attribute Editor
# sections (offscreen), and Lila's scene and live stages drawn through Hydra Storm as a viewport renderer.
add_executable(lila_usd_tests main.cpp test_usd_roundtrip.cpp test_usd_fixtures.cpp test_usd_stage.cpp test_usd_panels.cpp test_usd_hydra.cpp test_usd_georef.cpp
    test_usd_materialx.cpp test_usd_components.cpp test_usd_lighting_channels.cpp)
# LilaUI: the extension's Attribute Editor sections link it, so its DLLs must sit beside the tests.
target_link_libraries(lila_usd_tests PRIVATE LilaScene LilaUI doctest::doctest)
add_dependencies(lila_usd_tests lila_openusd_extension)
target_compile_definitions(lila_usd_tests PRIVATE
    LILA_TEST_OPENUSD_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/OpenUSD.lilaext"
    LILA_TEST_USD_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
if(WIN32)
    cmake_path(GET Python_EXECUTABLE PARENT_PATH _pyhome)
    add_custom_command(TARGET lila_usd_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_pyhome}/python3.dll ${_pyhome}/python${Python_VERSION_MAJOR}${Python_VERSION_MINOR}.dll $<TARGET_FILE_DIR:lila_usd_tests>
        COMMENT "Deploying the Python runtime for lila_usd_tests")
    lila_deploy_qt(lila_usd_tests)
endif()
add_test(NAME lila_usd_tests COMMAND lila_usd_tests)

# lila.usd in lilabatch: pxr imported in Lila's Python, and live stages edited through it.
get_property(_usd_python GLOBAL PROPERTY LILA_USD_PYTHON)
if(TARGET lilabatch AND _usd_python)
    add_test(NAME lila_usd_python COMMAND lilabatch --batch ${CMAKE_SOURCE_DIR}/python/tests/test_usd.py)
    set_tests_properties(lila_usd_python PROPERTIES ENVIRONMENT "LILA_CONFIG_DIR=${CMAKE_CURRENT_BINARY_DIR}/usd_python_cfg")
    # Its own process, so the stage it opens is the first to load USD's plugins; a deadlock shows as the timeout.
    add_test(NAME lila_usd_python_threads COMMAND lilabatch --batch ${CMAKE_SOURCE_DIR}/python/tests/test_usd_threads.py)
    set_tests_properties(lila_usd_python_threads PROPERTIES TIMEOUT 300 ENVIRONMENT "LILA_CONFIG_DIR=${CMAKE_CURRENT_BINARY_DIR}/usd_python_cfg")
endif()
