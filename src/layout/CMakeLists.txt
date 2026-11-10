find_package(Eigen3 CONFIG REQUIRED)
lila_module(lila_layout Scene
    BakeMaps.cpp
    Edit.cpp
    Edit.h
    Fit.cpp
    Fit.h
    Layout.cpp
    Layout.h
    LayoutNode.cpp
    LayoutNode.h
    Lines.cpp
    Lines.h
    Maps.cpp
    Maps.h
    Path.cpp
    Path.h
    Resolve.cpp
    Resolve.h
    SoftNet.cpp
    SoftNet.h
    SoftNetBind.cpp
    SoftNetBind.h
    SoftNetExport.cpp
    SoftNetLayouts.cpp
    SoftNetPose.cpp
    Surface.cpp
    Surface.h)
target_link_libraries(lila_layout PUBLIC lila_geo lila_scene TBB::tbb PRIVATE lila_bake lila_link lila_deform lila_curve Eigen3::Eigen)
