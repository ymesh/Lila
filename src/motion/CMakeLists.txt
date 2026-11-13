lila_module(lila_motion Scene
    Engine.cpp
    Engine.h
    Character.cpp
    Character.h
    Generate.cpp
    Generate.h
    Scene.cpp
    Scene.h)
target_link_libraries(lila_motion PUBLIC lila_scene lila_retarget lila_curve PRIVATE lila_onnxruntime_headers)
if(NOT WIN32)
    target_link_libraries(lila_motion PRIVATE ${CMAKE_DL_LIBS})
endif()

# ONNX Runtime (and DirectML) beside LilaScene, plus the model and its licence files under models/motionbricks.
set(_mb_files
    ${CMAKE_SOURCE_DIR}/third_party/motionbricks/Notice.txt
    ${CMAKE_SOURCE_DIR}/third_party/motionbricks/NVIDIA-Open-Model-Agreement.txt
    ${CMAKE_SOURCE_DIR}/third_party/motionbricks/LICENSE)
set(_mb_model "")
if(LILA_MOTIONBRICKS_MODEL_DIR AND EXISTS "${LILA_MOTIONBRICKS_MODEL_DIR}/mmm_v6_soma_fp16.onnx")
    set(_mb_model "${LILA_MOTIONBRICKS_MODEL_DIR}/mmm_v6_soma_fp16.onnx")
else()
    message(STATUS "MotionBricks model not found (set LILA_MOTIONBRICKS_MODEL_DIR): motion generation will report it missing")
endif()
add_custom_target(lila_motion_runtime ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:LilaScene>/models/motionbricks
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LILA_ONNXRUNTIME_BINARIES} $<TARGET_FILE_DIR:LilaScene>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_mb_files} ${_mb_model} $<TARGET_FILE_DIR:LilaScene>/models/motionbricks
    COMMENT "Deploying ONNX Runtime and the motion model"
    VERBATIM)
add_dependencies(lila_motion lila_motion_runtime)
