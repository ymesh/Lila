find_package(Eigen3 CONFIG REQUIRED)

lila_module(lila_uv Scene
    Align.cpp
    Bff.cpp
    Chart.cpp
    FinalMesh.cpp
    Flow.cpp
    Guided.cpp
    Hose.cpp
    Hotspot.cpp
    KeepApart.cpp
    Layout.cpp
    LiveSolver.cpp
    Metrics.cpp
    OptCuts.cpp
    Optimize.cpp
    Pack.cpp
    Project.cpp
    Seams.cpp
    SymmetryUv.cpp
    Unwrap.cpp
    Uv.cpp)
target_link_libraries(lila_uv PUBLIC lila_geo TBB::tbb PRIVATE Eigen3::Eigen)
if(MSVC)
    # Eigen's own headers trip a few /W4 checks.
    target_compile_options(lila_uv PRIVATE /wd4127 /wd4244)
endif()

lila_module(lila_uv_cmds Scene UvCommands.cpp UvCommands.h LiveCommands.cpp LiveSession.cpp OptimizeCommands.cpp)
target_link_libraries(lila_uv_cmds PUBLIC lila_uv lila_scene)
