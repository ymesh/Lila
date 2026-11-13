find_package(E57Format CONFIG REQUIRED)
find_path(LASZIP_INCLUDE_DIR laszip/laszip_api.h REQUIRED)
find_library(LASZIP_LIBRARY NAMES laszip3 laszip REQUIRED)

lila_module(lila_pointcloud Scene
    Octree.cpp
    Octree.h
    PointCloudNode.cpp
    PointCloudNode.h
    PointSource.cpp
    PointSource.h)
target_include_directories(lila_pointcloud PRIVATE ${LASZIP_INCLUDE_DIR}/laszip)
target_link_libraries(lila_pointcloud PUBLIC lila_scene lila_geo PRIVATE E57Format ${LASZIP_LIBRARY} TBB::tbb)
