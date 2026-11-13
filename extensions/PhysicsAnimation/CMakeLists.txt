# Physics Animation, built as <bin>/<config>/extensions/PhysicsAnimation.lilaext next to lila.exe, which loads it by
# default. Jolt comes in statically with lila_physics.
lila_add_extension(lila_physics_animation_extension ID PhysicsAnimation MANIFEST extension.json UI
    SOURCES Solver.cpp Model.cpp World.cpp Ui.cpp GrappleRagdoll.cpp PhysicsAnimation.h World.h)
target_link_libraries(lila_physics_animation_extension PRIVATE lila_physics)
foreach(_host lila lilabatch)
    if(TARGET ${_host})
        add_dependencies(${_host} lila_physics_animation_extension)
    endif()
endforeach()
