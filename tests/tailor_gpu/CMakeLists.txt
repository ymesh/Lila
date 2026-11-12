# Tailor's GPU solver against the CPU one, on a headless renderer.
add_executable(lila_tailor_gpu_tests main.cpp test_cloth_gpu.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Tailor/ClothSolver.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Tailor/ClothGpu.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Tailor/Fabrics.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Tailor/Meshing.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Tailor/Blocks.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Tailor/Mannequin.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Tailor/Surface.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Tailor/Pattern.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Tailor/Binding.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Tailor/Details.cpp)
target_include_directories(lila_tailor_gpu_tests PRIVATE ${CMAKE_SOURCE_DIR}/extensions/Tailor ${CMAKE_SOURCE_DIR}/sdk/include)
target_link_libraries(lila_tailor_gpu_tests PRIVATE LilaUI doctest::doctest)
add_dependencies(lila_tailor_gpu_tests lila_tailor_extension_shaders)
add_test(NAME lila_tailor_gpu_tests COMMAND lila_tailor_gpu_tests)
