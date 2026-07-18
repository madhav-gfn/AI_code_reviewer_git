# https://github.com/microsoft/onnxruntime/blob/v1.22.1/tools/python/util/vcpkg_helpers.py
message(WARNING "The port requires 'onnx' port build with CMake option ONNX_DISABLE_STATIC_REGISTRATION=ON")
if(VCPKG_TARGET_IS_OSX OR VCPKG_TARGET_IS_IOS)
    if("framework" IN_LIST FEATURES)
        # The Objective-C API requires onnxruntime_BUILD_SHARED_LIB
        vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)
    endif()
endif()

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO microsoft/onnxruntime
    REF "v${VERSION}"
    SHA512 373c51575ada457b8aead5d195a5f3eba62fb747b6370a2a9889fff875c40ea30af8fd49104d58cc86f79247410e829086b0979f37ca8635c6dd34960e9cc424
    PATCHES
        fix-cmake.patch # .framework install, external library workarounds(abseil-cpp, eigen3)
        fix-cmake-cuda.patch
        disable-rdc.patch
        fix-concat-link.patch
)

find_program(PROTOC NAMES protoc PATHS "${CURRENT_HOST_INSTALLED_DIR}/tools/protobuf" REQUIRED NO_DEFAULT_PATH NO_CMAKE_PATH)
message(STATUS "Using protoc: ${PROTOC}")

find_program(FLATC NAMES flatc PATHS "${CURRENT_HOST_INSTALLED_DIR}/tools/flatbuffers" REQUIRED NO_DEFAULT_PATH NO_CMAKE_PATH)
message(STATUS "Using flatc: ${FLATC}")

vcpkg_find_acquire_program(PYTHON3)
get_filename_component(PYTHON_PATH "${PYTHON3}" PATH)
message(STATUS "Using python3: ${PYTHON3}")

vcpkg_execute_required_process(
    COMMAND "${PYTHON3}" onnxruntime/core/flatbuffers/schema/compile_schema.py --flatc "${FLATC}"
    LOGNAME compile_schema_core
    WORKING_DIRECTORY "${SOURCE_PATH}"
)
vcpkg_execute_required_process(
    COMMAND "${PYTHON3}" onnxruntime/lora/adapter_format/compile_schema.py --flatc "${FLATC}"
    LOGNAME compile_schema_lora
    WORKING_DIRECTORY "${SOURCE_PATH}"
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        python    onnxruntime_ENABLE_PYTHON
        training  onnxruntime_ENABLE_TRAINING
        training  onnxruntime_ENABLE_TRAINING_APIS
        cuda      onnxruntime_USE_CUDA
        cuda      onnxruntime_USE_CUDA_NHWC_OPS
        openvino  onnxruntime_USE_OPENVINO
        tensorrt  onnxruntime_USE_TENSORRT
        tensorrt  onnxruntime_USE_TENSORRT_BUILTIN_PARSER
        directml  onnxruntime_USE_DML
        directml  onnxruntime_USE_CUSTOM_DIRECTML
        winml     onnxruntime_USE_WINML
        coreml    onnxruntime_USE_COREML
        mimalloc  onnxruntime_USE_MIMALLOC
        valgrind  onnxruntime_USE_VALGRIND
        xnnpack   onnxruntime_USE_XNNPACK
        nnapi     onnxruntime_USE_NNAPI_BUILTIN
        azure     onnxruntime_USE_AZURE
        test      onnxruntime_BUILD_UNIT_TESTS
        test      onnxruntime_BUILD_BENCHMARKS
        test      onnxruntime_RUN_ONNX_TESTS
        framework onnxruntime_BUILD_APPLE_FRAMEWORK
        framework onnxruntime_BUILD_OBJC
        nccl      onnxruntime_USE_NCCL
    INVERTED_FEATURES
        cuda      onnxruntime_USE_MEMORY_EFFICIENT_ATTENTION
)

if("cuda" IN_LIST FEATURES)
    vcpkg_find_cuda(OUT_CUDA_TOOLKIT_ROOT cuda_toolkit_root)
    list(APPEND FEATURE_OPTIONS
        "-DCMAKE_CUDA_COMPILER=${NVCC}"
        "-DCUDAToolkit_ROOT=${cuda_toolkit_root}"
        # Upstream default arch list includes compute_60/70, which CUDA
        # 13.x's nvcc no longer supports (min is sm_75) - build for the
        # local GPU's actual architecture only (RTX 2050 = sm_86).
        "-DCMAKE_CUDA_ARCHITECTURES=86-real"
        # too much warnings about attribute
        # CUDA 13's CCCL headers require the conforming MSVC preprocessor,
        # which isn't the default on this VS toolset - forward /Zc:preprocessor
        # to the host compiler or nvcc's cl.exe invocations hit C1189.
        # CUDA 13 soft-deprecated the unaligned longlong4/ulonglong4 vector
        # types in favor of longlong4_16a/32a; onnxruntime 1.23.2's own
        # contrib_ops/cuda/bert/attention_impl.h still uses the old names.
        # The types are unchanged and fully functional - only their name is
        # deprecated - and MSVC's /sdl flag promotes that C4996 to a hard
        # error, so use CUDA's own official suppression macro rather than
        # patching onnxruntime source.
        "-DCMAKE_CUDA_FLAGS=-Xcudafe --diag_suppress=2803 -Wno-deprecated-gpu-targets -Xcompiler=/Zc:preprocessor -D__NV_NO_VECTOR_DEPRECATION_DIAG"
        # Some cuda EP contrib_ops (e.g. llm/cutlass_heuristic.cc) are plain
        # .cc files compiled directly by cl.exe but still include CCCL/CUDA
        # vector-type headers, so they need the same flags outside of nvcc too.
        "-DCMAKE_CXX_FLAGS=/Zc:preprocessor -D__NV_NO_VECTOR_DEPRECATION_DIAG"
        # contrib_ops (flash/lean/memory-efficient attention, MoE, etc.) are
        # Microsoft's extra LLM-serving kernels layered on top of the
        # standard ONNX op set - onnxruntime_providers_cuda.cmake gates
        # their entire compilation behind this flag. Our embedding model
        # (checked via `onnx.load(...).graph.node` op_types) is exported
        # straight from torch.onnx.export as plain MatMul/Softmax/Gather/
        # etc. standard ops - it never touches a contrib op, so disabling
        # this whole category costs zero functionality or performance here.
        # It also happens to route around several build breaks specific to
        # this bleeding-edge toolchain: flash-attention's CUTLASS/cute
        # kernels are extremely RAM-hungry per translation unit and
        # OOM-crash the build on this 16GB machine, and contrib_ops/cuda/moe
        # hits a parser bug in nvcc's frontend against this Abseil version's
        # absl::Hash template metaprogramming.
        "-Donnxruntime_DISABLE_CONTRIB_OPS=ON"
    )
endif()

if("tensorrt" IN_LIST FEATURES)
    if(DEFINED ENV{TENSORRT_HOME})
        set(TENSORRT_HOME "$ENV{TENSORRT_HOME}")
    endif()
    if(DEFINED TENSORRT_HOME)
        message(STATUS "Using TensorRT: ${TENSORRT_HOME}")
        list(APPEND FEATURE_OPTIONS "-Donnxruntime_TENSORRT_HOME:PATH=${TENSORRT_HOME}")
    else()
        message(WARNING "Define TENSORRT_HOME for onnxruntime_TENSORRT_HOME")
    endif()
endif()

string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "dynamic" BUILD_SHARED)

# see tools/ci_build/build.py
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}/cmake"
    OPTIONS
        ${FEATURE_OPTIONS}
        "-DPython_EXECUTABLE:FILEPATH=${PYTHON3}"
        "-DProtobuf_PROTOC_EXECUTABLE:FILEPATH=${PROTOC}"
        "-DONNX_CUSTOM_PROTOC_EXECUTABLE:FILEPATH=${PROTOC}"
        -DBUILD_PKGCONFIG_FILES=ON
        -Donnxruntime_BUILD_SHARED_LIB=${BUILD_SHARED}
        -Donnxruntime_CROSS_COMPILING=${VCPKG_CROSSCOMPILING}
        -Donnxruntime_USE_EXTENSIONS=OFF
        -Donnxruntime_USE_NNAPI_BUILTIN=${VCPKG_TARGET_IS_ANDROID}
        -Donnxruntime_USE_VCPKG=ON
        -Donnxruntime_ENABLE_CPUINFO=ON
        -Donnxruntime_ENABLE_MICROSOFT_INTERNAL=OFF
        -Donnxruntime_ENABLE_BITCODE=OFF
        -Donnxruntime_ENABLE_PYTHON=OFF
        -Donnxruntime_ENABLE_EXTERNAL_CUSTOM_OP_SCHEMAS=OFF
        -Donnxruntime_ENABLE_MEMORY_PROFILE=OFF
        -Donnxruntime_ENABLE_LAZY_TENSOR=OFF
        -Donnxruntime_DISABLE_RTTI=OFF
        -Donnxruntime_DISABLE_ABSEIL=OFF
        # some other customizations ...
        --compile-no-warning-as-error
    OPTIONS_DEBUG
        -Donnxruntime_ENABLE_MEMLEAK_CHECKER=OFF
        -Donnxruntime_DEBUG_NODE_INPUTS_OUTPUTS=1
    MAYBE_UNUSED_VARIABLES
        Python_EXECUTABLE
        onnxruntime_TENSORRT_PLACEHOLDER_BUILDER
        onnxruntime_NVCC_THREADS
        CMAKE_CUDA_FLAGS
        onnxruntime_USE_CUSTOM_DIRECTML
)
if("cuda" IN_LIST FEATURES)
    vcpkg_cmake_build(TARGET onnxruntime_providers_cuda LOGFILE_BASE build-cuda)
endif()
if("tensorrt" IN_LIST FEATURES)
    vcpkg_cmake_build(TARGET onnxruntime_providers_tensorrt LOGFILE_BASE build-tensorrt)
endif()
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/onnxruntime)
vcpkg_fixup_pkgconfig() # pkg_check_modules(libonnxruntime)

# relocates the onnxruntime_providers_* binaries before vcpkg_copy_pdbs()
function(reolocate_ort_providers)
    if(VCPKG_TARGET_IS_WINDOWS AND (VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic"))
        # the target is expected to be used without the .lib files
        file(GLOB PROVIDE_BINS_DBG  "${CURRENT_PACKAGES_DIR}/debug/lib/onnxruntime_providers_*.dll")
        file(COPY ${PROVIDE_BINS_DBG} DESTINATION "${CURRENT_PACKAGES_DIR}/debug/bin")
        file(GLOB PROVIDE_BINS_REL "${CURRENT_PACKAGES_DIR}/lib/onnxruntime_providers_*.dll")
        file(COPY ${PROVIDE_BINS_REL} DESTINATION "${CURRENT_PACKAGES_DIR}/bin")
        file(REMOVE ${PROVIDE_BINS_DBG} ${PROVIDE_BINS_REL})
    endif()
endfunction()

reolocate_ort_providers()
vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
if(VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/bin" "${CURRENT_PACKAGES_DIR}/bin")
endif()

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
