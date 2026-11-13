lila_module(lila_tiles Scene
    Tiles3d.cpp
    Tiles3d.h)
target_link_libraries(lila_tiles PUBLIC lila_scene PRIVATE lila_gltf lila_pointcloud)
