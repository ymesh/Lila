# GroomLab's engine (github.com/thekesslereffect/groomlab 3e7ffec, Apache-2.0, see LICENSE and NOTICE): the groom
# stack's graph, fields, expressions and operators. A static library of its own: unity off, built with its
# determinism settings, nothing of it exported from LilaScene.

# Their release flags (this directory's targets only): inline what is worth it, under /fp:precise. Never /fp:fast,
# the determinism tests check it.
string(REPLACE "/Ob1" "/Ob2" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")

# The engine's own version (version.h), not Lila's.
set(PROJECT_VERSION 0.40.0)
set(PROJECT_VERSION_MAJOR 0)
set(PROJECT_VERSION_MINOR 40)
set(PROJECT_VERSION_PATCH 0)
find_package(Alembic CONFIG REQUIRED)
configure_file(core/include/groomlab/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/groomlab/version.h @ONLY)

# look.slang's kernels as C++ for the CPU look engine, made by vcpkg's slangc into the build dir.
set(_kernel_list ${CMAKE_CURRENT_SOURCE_DIR}/core/include/groomlab/look/look_kernels.inc)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_kernel_list})
file(STRINGS ${_kernel_list} _kernel_lines REGEX "^GROOMLAB_KERNEL\\(")
set(_cpu_entries "")
foreach(_line IN LISTS _kernel_lines)
    string(REGEX REPLACE "^GROOMLAB_KERNEL\\(([A-Za-z0-9_]+)\\).*" "\\1CS" _k "${_line}")
    list(APPEND _cpu_entries -entry ${_k} -stage compute)
endforeach()
set(_gen ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(_kernels_cpp ${_gen}/look_kernels_cpu.cpp)
add_custom_command(OUTPUT ${_kernels_cpp}
    BYPRODUCTS ${_gen}/look_kernels_cpu.raw.cpp
    COMMAND ${SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/look.slang -I ${CMAKE_CURRENT_SOURCE_DIR}/core/include
            ${_cpu_entries} -target cpp -o ${_gen}/look_kernels_cpu.raw.cpp
    COMMAND ${CMAKE_COMMAND} -DIN=${_gen}/look_kernels_cpu.raw.cpp -DOUT=${_kernels_cpp}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/portable_cpu_kernels.cmake
    DEPENDS shaders/look.slang core/include/groomlab/shared/groom_math.h cmake/portable_cpu_kernels.cmake
    COMMENT "slangc: GroomLab look kernels for the CPU"
    VERBATIM)
set_source_files_properties(${_kernels_cpp} PROPERTIES GENERATED ON HEADER_FILE_ONLY ON)

add_library(groomlab_core STATIC
    core/src/attributes.cpp
    core/src/brush.cpp
    core/src/brush_catalog.cpp
    core/src/brush_alpha.cpp
    core/src/camera_solve.cpp
    core/src/card_atlas.cpp
    core/src/card_bake.cpp
    core/src/card_feathers.cpp
    core/src/card_files.cpp
    core/src/card_maps.cpp
    core/src/card_presets.cpp
    core/src/card_separation.cpp
    core/src/cards.cpp
    core/src/color_patterns.cpp
    core/src/correctives.cpp
    core/src/curve.cpp
    core/src/display.cpp
    core/src/display_overlays.cpp
    core/src/distance_field.cpp
    core/src/export_view.cpp
    core/src/expression.cpp
    core/src/feathers.cpp
    core/src/field.cpp
    core/src/flow.cpp
    core/src/follow_scalp.cpp
    core/src/frame_time.cpp
    core/src/frames.cpp
    core/src/gltf_writer.cpp
    core/src/graph.cpp
    core/src/graph_edit.cpp
    core/src/graph_io.cpp
    core/src/graph_pins.cpp
    core/src/groom.cpp
    core/src/groom_file.cpp
    core/src/guide_hierarchy.cpp
    core/src/guide_selection.cpp
    core/src/guide_set.cpp
    core/src/hair_color.cpp
    core/src/instances.cpp
    core/src/length_field.cpp
    core/src/length_key_tool.cpp
    core/src/lod.cpp
    core/src/look/look_cpu.cpp
    core/src/look/look_engine.cpp
    core/src/look/look_engine_cpu.cpp
    core/src/look/look_run.cpp
    core/src/look/look_stage_params.cpp
    core/src/node_catalog.cpp
    core/src/node_common.cpp
    core/src/noise.cpp
    core/src/operators/braid_op.cpp
    core/src/operators/cinematic_ops.cpp
    core/src/operators/color_ops.cpp
    core/src/operators/curl_op.cpp
    core/src/operators/deform_ops.cpp
    core/src/operators/extra_ops.cpp
    core/src/operators/feather_ops.cpp
    core/src/operators/field_ops.cpp
    core/src/operators/group_op.cpp
    core/src/operators/hair_grid.cpp
    core/src/operators/incremental.cpp
    core/src/operators/instance_op.cpp
    core/src/operators/interpolate.cpp
    core/src/operators/look_ops.cpp
    core/src/operators/mesh_cut_op.cpp
    core/src/operators/mirror_op.cpp
    core/src/operators/motion_ops.cpp
    core/src/operators/neighbor_ops.cpp
    core/src/operators/plume_op.cpp
    core/src/operators/registry.cpp
    core/src/operators/render_data_op.cpp
    core/src/operators/scatter.cpp
    core/src/operators/sculpt_op.cpp
    core/src/operators/self_collide_op.cpp
    core/src/operators/shape_ops.cpp
    core/src/operators/simulate_op.cpp
    core/src/operators/sources.cpp
    core/src/operators/static_op.cpp
    core/src/operators/subdivide_op.cpp
    core/src/operators/to_mesh_op.cpp
    core/src/operators/transform_ops.cpp
    core/src/operators/wet_op.cpp
    core/src/parallel_tbb.cpp
    core/src/parts.cpp
    core/src/paths.cpp
    core/src/pin_morph.cpp
    core/src/png.cpp
    core/src/point_index.cpp
    core/src/presets.cpp
    core/src/preview_core.cpp
    core/src/process_caches.cpp
    core/src/ptex.cpp
    core/src/ramp.cpp
    core/src/reference_edit.cpp
    core/src/reference_guides.cpp
    core/src/reference_look.cpp
    core/src/reference_orientation.cpp
    core/src/render_overrides.cpp
    core/src/renderman_look.cpp
    core/src/scale_maps.cpp
    core/src/scales.cpp
    core/src/scalp_history.cpp
    core/src/scalp_maps.cpp
    core/src/scalp_mesh.cpp
    core/src/scalp_mirror.cpp
    core/src/scalp_paint.cpp
    core/src/scalp_pieces.cpp
    core/src/schema.cpp
    core/src/script.cpp
    core/src/sculpt.cpp
    core/src/semver.cpp
    core/src/simulation.cpp
    core/src/strand_geometry.cpp
    core/src/strand_lod.cpp
    core/src/strand_set.cpp
    core/src/stroke.cpp
    core/src/stroke_tools.cpp
    core/src/subdivision.cpp
    core/src/surface_paths.cpp
    core/src/texture.cpp
    core/src/transfer.cpp
    core/src/transfer_markers_edit.cpp
    core/src/tube.cpp
    core/src/usd_writer.cpp
    core/src/view_projection.cpp
    core/src/vrscene_writer.cpp
    io/src/alembic_io.cpp
    io/src/alembic_mesh.cpp
    io/src/engine_export.cpp
    io/src/guide_cache_op.cpp
    io/src/import_curves_op.cpp
    io/src/sim_cache.cpp
    gpu/src/look_chain.cpp
    ${_kernels_cpp})
target_include_directories(groomlab_core
    PUBLIC core/include io/include gpu/include ${_gen}
    PRIVATE core/src/look ${CMAKE_SOURCE_DIR}/third_party/stb)
target_compile_definitions(groomlab_core PUBLIC GROOMLAB_HAS_TBB=1 GROOMLAB_PORTABLE_MATH=1)
target_link_libraries(groomlab_core PUBLIC TBB::tbb PRIVATE nlohmann_json::nlohmann_json Alembic::Alembic)

# The kernels' C++ through clang-cl when it is installed (about twice as fast for Interpolate, Collide and SelfCollide,
# the same floats: no contraction into FMAs, no strict aliasing since the Slang prelude type-puns), else cl.
find_program(GROOMLAB_CLANG_CL clang-cl HINTS C:/Tools/llvm/bin "$ENV{ProgramFiles}/LLVM/bin")
if(GROOMLAB_CLANG_CL)
    set(_kernels_obj ${_gen}/look_kernels_cpu_$<CONFIG>.obj)
    set(_kernels_dep ${_gen}/look_kernels_cpu_$<CONFIG>.d)
    add_custom_command(OUTPUT ${_kernels_obj}
        COMMAND ${GROOMLAB_CLANG_CL} /nologo /c /TP -std:c++17 /EHsc /utf-8 -w -Z7
                "$<IF:$<CONFIG:Debug>,/Od;/MDd;/D_DEBUG,/O2;/MD;/DNDEBUG>"
                /clang:-fno-strict-aliasing /clang:-ffp-contract=off
                /I${CMAKE_CURRENT_SOURCE_DIR}/core/include /I${CMAKE_CURRENT_SOURCE_DIR}/core/src/look
                /I${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include
                /DGROOMLAB_HAS_TBB=1 /DGROOMLAB_PORTABLE_MATH=1
                /clang:-MD /clang:-MF${_kernels_dep} /Fo${_kernels_obj} ${_kernels_cpp}
        DEPENDS ${_kernels_cpp}
        DEPFILE ${_kernels_dep}
        COMMENT "clang-cl: GroomLab look kernels"
        COMMAND_EXPAND_LISTS
        VERBATIM)
    set_source_files_properties(${_kernels_obj} PROPERTIES EXTERNAL_OBJECT ON GENERATED ON)
    target_sources(groomlab_core PRIVATE ${_kernels_obj})
else()
    target_sources(groomlab_core PRIVATE LookKernelsCpu.cpp)
    set_source_files_properties(LookKernelsCpu.cpp PROPERTIES COMPILE_OPTIONS /bigobj)
endif()
# Shadowed locals and size_t narrowing are their style; the code is kept as imported.
target_compile_options(groomlab_core PRIVATE /wd4456 /wd4457 /wd4267)
set_target_properties(groomlab_core PROPERTIES LILA_NO_UNITY ON CXX_STANDARD 17 FOLDER groom)

# look.slang's kernels as SPIR-V for the Vulkan look engine (src/groom/LookEngineDevice.cpp), next to the executables.
set(_look_spv ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/shaders/look.spv)
add_custom_command(OUTPUT ${_look_spv}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/shaders
    COMMAND ${SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/look.slang -I ${CMAKE_CURRENT_SOURCE_DIR}/core/include -target spirv
            -profile spirv_1_6 -fp-mode precise -fvk-use-entrypoint-name -o ${_look_spv}
    DEPENDS shaders/look.slang core/include/groomlab/shared/groom_math.h
    COMMENT "slangc: GroomLab look kernels for Vulkan")
add_custom_target(groomlab_look_spirv DEPENDS ${_look_spv})
add_dependencies(groomlab_core groomlab_look_spirv)
