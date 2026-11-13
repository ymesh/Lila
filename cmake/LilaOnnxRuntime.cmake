# ONNX Runtime for the motion engine (src/motion): the official prebuilt release, pinned by version and SHA-256.
# Windows takes the DirectML package (CPU and DirectML execution providers) plus DirectML.dll; Linux the CPU release.
# The library is loaded at run time, so only the headers are needed to build; the DLLs are copied beside LilaScene.
# Set LILA_ONNXRUNTIME_ARCHIVE_DIR to a folder holding the archives to build offline.
set(LILA_ORT_VERSION 1.24.4)
set(LILA_DIRECTML_VERSION 1.15.4)
set(LILA_ONNXRUNTIME_ARCHIVE_DIR "$ENV{LILA_ONNXRUNTIME_ARCHIVE_DIR}" CACHE PATH "Folder with pre-downloaded ONNX Runtime archives")

set(_ort_root ${CMAKE_BINARY_DIR}/_deps/onnxruntime-${LILA_ORT_VERSION})

function(_lila_fetch_archive name url sha256 out)
    set(_file ${CMAKE_BINARY_DIR}/_deps/downloads/${name})
    if(LILA_ONNXRUNTIME_ARCHIVE_DIR AND EXISTS ${LILA_ONNXRUNTIME_ARCHIVE_DIR}/${name})
        set(_file ${LILA_ONNXRUNTIME_ARCHIVE_DIR}/${name})
    endif()
    if(EXISTS ${_file})
        file(SHA256 ${_file} _have)
    endif()
    if(NOT _have STREQUAL sha256)
        message(STATUS "Downloading ${name}")
        file(DOWNLOAD ${url} ${_file} EXPECTED_HASH SHA256=${sha256} STATUS _status TLS_VERIFY ON)
        list(GET _status 0 _code)
        if(NOT _code EQUAL 0)
            message(FATAL_ERROR "Could not download ${url}: ${_status}. Put ${name} in LILA_ONNXRUNTIME_ARCHIVE_DIR to build offline.")
        endif()
    endif()
    set(${out} ${_file} PARENT_SCOPE)
endfunction()

if(NOT EXISTS ${_ort_root}/.done)
    file(REMOVE_RECURSE ${_ort_root})
    if(WIN32)
        _lila_fetch_archive(microsoft.ml.onnxruntime.directml.${LILA_ORT_VERSION}.nupkg
            https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.directml/${LILA_ORT_VERSION}/microsoft.ml.onnxruntime.directml.${LILA_ORT_VERSION}.nupkg
            57e9f11b73437bef7a309496135d4c1f96b1a8e9ddba60013fa27bfc1d788681 _ort)
        _lila_fetch_archive(microsoft.ai.directml.${LILA_DIRECTML_VERSION}.nupkg
            https://api.nuget.org/v3-flatcontainer/microsoft.ai.directml/${LILA_DIRECTML_VERSION}/microsoft.ai.directml.${LILA_DIRECTML_VERSION}.nupkg
            4e7cb7ddce8cf837a7a75dc029209b520ca0101470fcdf275c1f49736a3615b9 _dml)
        file(ARCHIVE_EXTRACT INPUT ${_ort} DESTINATION ${_ort_root}/ort PATTERNS build/native/include runtimes/win-x64/native LICENSE ThirdPartyNotices.txt)
        file(ARCHIVE_EXTRACT INPUT ${_dml} DESTINATION ${_ort_root}/dml PATTERNS bin/x64-win/DirectML.dll LICENSE.txt ThirdPartyNotices.txt)
        file(COPY ${_ort_root}/ort/build/native/include/ DESTINATION ${_ort_root}/include)
        file(GLOB _bins ${_ort_root}/ort/runtimes/win-x64/native/*.dll)
        file(COPY ${_bins} ${_ort_root}/dml/bin/x64-win/DirectML.dll DESTINATION ${_ort_root}/bin)
        file(COPY_FILE ${_ort_root}/ort/LICENSE ${_ort_root}/LICENSE-onnxruntime.txt)
        file(COPY_FILE ${_ort_root}/dml/LICENSE.txt ${_ort_root}/LICENSE-directml.txt)
    else()
        _lila_fetch_archive(onnxruntime-linux-x64-${LILA_ORT_VERSION}.tgz
            https://github.com/microsoft/onnxruntime/releases/download/v${LILA_ORT_VERSION}/onnxruntime-linux-x64-${LILA_ORT_VERSION}.tgz
            3a211fbea252c1e66290658f1b735b772056149f28321e71c308942cdb54b747 _ort)
        file(ARCHIVE_EXTRACT INPUT ${_ort} DESTINATION ${_ort_root}/ort)
        set(_src ${_ort_root}/ort/onnxruntime-linux-x64-${LILA_ORT_VERSION})
        file(COPY ${_src}/include/ DESTINATION ${_ort_root}/include)
        file(GLOB _bins ${_src}/lib/libonnxruntime*.so*)
        file(COPY ${_bins} DESTINATION ${_ort_root}/bin)
        file(COPY_FILE ${_src}/LICENSE ${_ort_root}/LICENSE-onnxruntime.txt)
    endif()
    file(TOUCH ${_ort_root}/.done)
endif()

add_library(lila_onnxruntime_headers INTERFACE)
target_include_directories(lila_onnxruntime_headers SYSTEM INTERFACE ${_ort_root}/include)
target_compile_definitions(lila_onnxruntime_headers INTERFACE ORT_API_MANUAL_INIT LILA_ORT_VERSION="${LILA_ORT_VERSION}")
file(GLOB LILA_ONNXRUNTIME_BINARIES ${_ort_root}/bin/*)
set(LILA_ONNXRUNTIME_LICENSE ${_ort_root}/LICENSE-onnxruntime.txt)
set(LILA_DIRECTML_LICENSE ${_ort_root}/LICENSE-directml.txt)

# The MotionBricks SOMA model is not in git: dev builds copy it from LILA_MOTIONBRICKS_MODEL_DIR, release staging
# (scripts/steam/stage.ps1) refuses to stage without it.
set(_mb_default "")
# Lila's copy is tools/motion/patch_model.py's output, which DirectML runs; the upstream file works on the CPU only.
foreach(_cand "$ENV{LILA_DATA}/motionbricks-lila" "X:/LilaData/motionbricks-lila" "$ENV{LILA_DATA}/motionbricks/motionbricks/assets/onnx/soma"
        "X:/LilaData/motionbricks/motionbricks/assets/onnx/soma")
    if(NOT _mb_default AND EXISTS "${_cand}/mmm_v6_soma_fp16.onnx")
        set(_mb_default "${_cand}")
    endif()
endforeach()
set(LILA_MOTIONBRICKS_MODEL_DIR "${_mb_default}" CACHE PATH "Folder holding mmm_v6_soma_fp16.onnx (MotionBricks SOMA)")
