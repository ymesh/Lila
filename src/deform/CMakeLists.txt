find_package(Eigen3 CONFIG REQUIRED)

lila_module(lila_deform Scene
    BendBones.cpp
    BendBones.h
    Deform.cpp
    Deform.h
    DeformUtil.h
    EigenGlm.h
    Handles.cpp
    Handles.h
    RigidBind.cpp
    RigidBind.h
    Shape.cpp
    Shape.h
    SoftNet.cpp
    SoftNet.h
    Skinning.cpp
    Skinning.h
    Ssdr.cpp
    Ssdr.h
    Surface.cpp
    Surface.h)
target_link_libraries(lila_deform PUBLIC lila_geo TBB::tbb PRIVATE Eigen3::Eigen)
target_include_directories(lila_deform PRIVATE ${CMAKE_SOURCE_DIR}/third_party/dem-bones/include)
if(MSVC)
    set_source_files_properties(Ssdr.cpp PROPERTIES COMPILE_OPTIONS /bigobj)
endif()
if(MSVC)
    # Eigen's own headers trip a few /W4 checks.
    target_compile_options(lila_deform PRIVATE /wd4127 /wd4244)
endif()
