lila_module(lila_vcolor Scene
    Masks.cpp
    Paint.cpp
    Paint.h
    VertexColors.cpp
    VertexColors.h)
target_link_libraries(lila_vcolor PUBLIC lila_scene lila_geo lila_brush TBB::tbb)
