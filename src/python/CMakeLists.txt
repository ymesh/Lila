nanobind_build_library(nanobind-static)
if(MSVC)
    # nanobind's own sources shadow names under /W4.
    target_compile_options(nanobind-static PRIVATE /wd4457 /wd4459)
endif()

lila_module(lila_python Scene
    Bindings.cpp
    MathBindings.cpp
    DebugDrawBindings.cpp
    SceneBindings.cpp
    MeshBindings.cpp
    ExtensionBindings.cpp
    RegistryBindings.cpp
    Python.cpp)

target_link_libraries(lila_python PUBLIC lila_scene lila_math lila_debug lila_vcolor lila_stream PRIVATE lila_rigmath nanobind-static Python::Python)
nanobind_compile_options(lila_python)
nanobind_link_options(lila_python)

if(WIN32)
    cmake_path(GET Python_EXECUTABLE PARENT_PATH LILA_PYTHON_HOME)
else()
    # The interpreter is <home>/bin/python3.x here, not <home>/python.exe.
    cmake_path(GET Python_EXECUTABLE PARENT_PATH LILA_PYTHON_HOME)
    cmake_path(GET LILA_PYTHON_HOME PARENT_PATH LILA_PYTHON_HOME)
endif()
target_compile_definitions(lila_python PRIVATE LILA_PYTHON_HOME="${LILA_PYTHON_HOME}")
