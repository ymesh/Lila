lila_module(lila_maya Scene
    MayaAscii.cpp
    MayaBinary.cpp
    MayaCommands.cpp
    MayaImport.cpp
    MayaIndex.h
    MayaRig.cpp
    MayaRig.h
    MayaTypes.cpp)
target_link_libraries(lila_maya PUBLIC lila_scene PRIVATE lila_io lila_fbx)
