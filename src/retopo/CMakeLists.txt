find_package(Eigen3 CONFIG REQUIRED)
find_package(meshoptimizer CONFIG REQUIRED)

lila_module(lila_retopo Scene
    AngleSnap.cpp
    Border.cpp
    Budget.cpp
    CommandArgs.h
    Construct.cpp
    Construct.h
    Field.cpp
    Generators.cpp
    Grid.cpp
    Handles.cpp
    Handles.h
    IntegerGrid.cpp
    IntegerGrid.h
    Internal.h
    Isotropic.cpp
    Isotropic.h
    LiveRelax.cpp
    LiveRelax.h
    Metrics.cpp
    Patch.cpp
    Patch.h
    Poles.cpp
    QuadRemesh.cpp
    Relax.cpp
    Repair.cpp
    Retopo.h
    Surface.cpp
    SurfaceStroke.cpp
    SurfaceStroke.h
    Voxel.cpp)
target_link_libraries(lila_retopo PUBLIC lila_geo TBB::tbb PRIVATE Eigen3::Eigen meshoptimizer::meshoptimizer)
if(MSVC)
    # Eigen's own headers trip a few /W4 checks.
    target_compile_options(lila_retopo PRIVATE /wd4127 /wd4244)
endif()

lila_module(lila_retopo_cmds Scene RetopoCommands.cpp RetopoCommands.h TopologyCommands.cpp)
target_link_libraries(lila_retopo_cmds PUBLIC lila_retopo lila_scene)

set(_ar ${CMAKE_SOURCE_DIR}/third_party/autoremesher)
lila_quiet_module(lila_autoremesher Scene
    ${_ar}/src/AutoRemesher/autoremesher.cpp
    ${_ar}/src/AutoRemesher/constrainedleastsquares.cpp
    ${_ar}/src/AutoRemesher/framefield.cpp
    ${_ar}/src/AutoRemesher/isotropicremesher.cpp
    ${_ar}/src/AutoRemesher/meshseparator.cpp
    ${_ar}/src/AutoRemesher/mixedintegerleastsquares.cpp
    ${_ar}/src/AutoRemesher/parameterizer.cpp
    ${_ar}/src/AutoRemesher/positionkey.cpp
    ${_ar}/src/AutoRemesher/quadextractor.cpp
    ${_ar}/src/AutoRemesher/quadparameterizer.cpp
    ${_ar}/src/AutoRemesher/singularitysimplifier.cpp
    ${_ar}/src/AutoRemesher/surfacemesh.cpp
    ${_ar}/isotropicremesher/axisalignedboundingboxtree.cpp
    ${_ar}/isotropicremesher/isotropichalfedgemesh.cpp
    ${_ar}/isotropicremesher/isotropicremesher.cpp)
target_include_directories(lila_autoremesher SYSTEM PUBLIC ${_ar}/include PRIVATE ${_ar}/isotropicremesher)
target_compile_definitions(lila_autoremesher PRIVATE NDEBUG _USE_MATH_DEFINES NOMINMAX)
target_link_libraries(lila_autoremesher PUBLIC TBB::tbb PRIVATE Eigen3::Eigen meshoptimizer::meshoptimizer)
set_target_properties(lila_autoremesher PROPERTIES LILA_NO_UNITY ON)
target_link_libraries(lila_retopo PRIVATE lila_autoremesher)
