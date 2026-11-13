# ctest step: configures and builds the sample extensions as an outside project, against the installed SDK only.
# -DSDK=<install prefix> -DSOURCE=<examples dir> -DBUILD=<build dir> -DCONFIG=<config> -DCXX=<compiler> -DMAKE=<ninja>
# -DDEPS=<prefix of the samples' own third-party packages (blend_scene's zstd), as an outside project brings them>
file(REMOVE_RECURSE ${BUILD})
execute_process(COMMAND ${CMAKE_COMMAND} -S ${SOURCE} -B ${BUILD} -G Ninja -DCMAKE_MAKE_PROGRAM=${MAKE} -DCMAKE_CXX_COMPILER=${CXX}
                        -DCMAKE_BUILD_TYPE=${CONFIG} "-DCMAKE_PREFIX_PATH=${SDK};${DEPS}"
                RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "configuring the examples against the SDK failed")
endif()
execute_process(COMMAND ${CMAKE_COMMAND} --build ${BUILD} RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "building the examples against the SDK failed")
endif()
