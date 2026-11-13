# Run as a script (cmake -P) after building lila: THIRD-PARTY-NOTICES.md beside the executable and each component's full
# license text in licenses/<id>.txt, which Help > Licenses shows. -DSOURCE_DIR, -DSHARE_DIR (vcpkg's share), -DOUT_DIR.
file(MAKE_DIRECTORY "${OUT_DIR}/licenses")
file(COPY_FILE "${SOURCE_DIR}/THIRD-PARTY-NOTICES.md" "${OUT_DIR}/THIRD-PARTY-NOTICES.md" ONLY_IF_DIFFERENT)
file(COPY_FILE "${SOURCE_DIR}/LICENSE" "${OUT_DIR}/LICENSE" ONLY_IF_DIFFERENT)
if(SHARE_DIR AND IS_DIRECTORY "${SHARE_DIR}")
    file(GLOB _ports LIST_DIRECTORIES true "${SHARE_DIR}/*")
    foreach(_port ${_ports})
        if(EXISTS "${_port}/copyright")
            get_filename_component(_id "${_port}" NAME)
            file(COPY_FILE "${_port}/copyright" "${OUT_DIR}/licenses/${_id}.txt" ONLY_IF_DIFFERENT)
        endif()
    endforeach()
endif()
foreach(_pair
        "cgltf|third_party/cgltf/LICENSE"
        "dem-bones|third_party/dem-bones/LICENSE.md"
        "archivo|resources/fonts/Archivo-LICENSE.txt"
        "inter|resources/fonts/Inter-LICENSE.txt"
        "jetbrains-mono|resources/fonts/JetBrainsMono-LICENSE.txt"
        "tabler-icons|resources/icons/tabler/LICENSE"
        "onnx-runtime|third_party/onnxruntime/LICENSE"
        "directml|third_party/onnxruntime/DirectML-LICENSE.txt"
        "motionbricks-soma-model|third_party/motionbricks/NVIDIA-Open-Model-Agreement.txt")
    string(REPLACE "|" ";" _parts "${_pair}")
    list(GET _parts 0 _id)
    list(GET _parts 1 _file)
    if(EXISTS "${SOURCE_DIR}/${_file}")
        file(COPY_FILE "${SOURCE_DIR}/${_file}" "${OUT_DIR}/licenses/${_id}.txt" ONLY_IF_DIFFERENT)
    endif()
endforeach()
# Components without a vcpkg copyright file: texts kept in the repo, and the ones read from where the build got them.
file(GLOB _texts "${SOURCE_DIR}/resources/licenses/*.txt")
foreach(_text ${_texts})
    get_filename_component(_name "${_text}" NAME)
    file(COPY_FILE "${_text}" "${OUT_DIR}/licenses/${_name}" ONLY_IF_DIFFERENT)
endforeach()
foreach(_pair "python|${PYTHON_HOME}/LICENSE.txt" "ufbx|${UFBX_DIR}/LICENSE" "nanobind|${NANOBIND_DIR}/LICENSE")
    string(REPLACE "|" ";" _parts "${_pair}")
    list(GET _parts 0 _id)
    list(GET _parts 1 _file)
    if(EXISTS "${_file}" AND NOT IS_DIRECTORY "${_file}")
        file(COPY_FILE "${_file}" "${OUT_DIR}/licenses/${_id}.txt" ONLY_IF_DIFFERENT)
    endif()
endforeach()
file(GLOB _qtpy "${OUT_DIR}/python-site/qt_py-*.dist-info/licenses/LICENSE")
if(_qtpy)
    list(GET _qtpy 0 _qtpy)
    file(COPY_FILE "${_qtpy}" "${OUT_DIR}/licenses/qt-py.txt" ONLY_IF_DIFFERENT)
endif()
# GroomLab's Apache-2.0 text with its NOTICE after it, as the license's section 4(d) asks.
file(READ "${SOURCE_DIR}/src/groom/groomlab/LICENSE" _groomlab)
file(READ "${SOURCE_DIR}/src/groom/groomlab/NOTICE" _groomlab_notice)
file(WRITE "${OUT_DIR}/licenses/groomlab.txt" "${_groomlab}\n\nNOTICE\n\n${_groomlab_notice}")
# MotionBricks code (Apache-2.0) with its Notice after it; the model's agreement is its own entry above.
file(READ "${SOURCE_DIR}/third_party/motionbricks/LICENSE" _mb)
file(READ "${SOURCE_DIR}/third_party/motionbricks/Notice.txt" _mb_notice)
file(WRITE "${OUT_DIR}/licenses/motionbricks.txt" "${_mb}\n\nNotice\n\n${_mb_notice}")
