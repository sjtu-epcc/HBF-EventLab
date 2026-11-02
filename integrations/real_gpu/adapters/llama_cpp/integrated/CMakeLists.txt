cmake_minimum_required(VERSION 3.24)
# This frontend owns its backend add_subdirectory; never include it from the backend root.
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  message(FATAL_ERROR "Configure the integrated llama frontend separately; do not add it from the backend root")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/gpu_configuration.cmake")
project(realgpu_llama LANGUAGES C CXX CUDA)
# Localized MSVC dependency output must match Ninja's UTF-8 prefix.
if(MSVC AND CMAKE_CXX_CL_SHOWINCLUDES_PREFIX MATCHES "娉ㄦ剰")
  set(CMAKE_CL_SHOWINCLUDES_PREFIX "注意: 包含文件:  ")
endif()


set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_RUNTIME_LIBRARY Shared)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
get_filename_component(LAB_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(LLAMA_SOURCE "${LAB_ROOT}/third_party/llama.cpp-integrated")
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(HBFLAB_EVENTLAB_SOURCE "${LAB_ROOT}/../.." CACHE PATH "EventLab backend source root")
get_filename_component(HBFLAB_EVENTLAB_SOURCE "${HBFLAB_EVENTLAB_SOURCE}" REALPATH BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(HBFLAB_EVENTLAB_SOURCE "${HBFLAB_EVENTLAB_SOURCE}" CACHE PATH "EventLab backend source root" FORCE)
if(NOT EXISTS "${HBFLAB_EVENTLAB_SOURCE}/CMakeLists.txt" OR
   NOT EXISTS "${HBFLAB_EVENTLAB_SOURCE}/include/hbf_eventlab/runtime/device_driver.hpp")
  message(FATAL_ERROR "Set HBFLAB_EVENTLAB_SOURCE to the HBF-EventLab backend root (default: LAB_ROOT/../..)")
endif()
add_subdirectory("${HBFLAB_EVENTLAB_SOURCE}" eventlab)
if(MSVC)
  set_property(TARGET hbf_eventlab PROPERTY INTERFACE_COMPILE_OPTIONS "$<$<COMPILE_LANGUAGE:CXX>:/utf-8;/EHsc>")
endif()
find_package(CUDAToolkit REQUIRED)
if(NOT TARGET CUDA::cublas)
  message(FATAL_ERROR "The integrated model requires cuBLAS development headers/libraries. Select a complete CUDAToolkit_ROOT; an nvcc/runtime-only installation is insufficient. CPU and CUDA contract builds use the separate frontend entry.")
endif()
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/async_ptx_helper.ptx"
 COMMAND "${CMAKE_CUDA_COMPILER}" --ptx --relocatable-device-code=true -std=c++17 -arch=${HBFLAB_PTX_ARCHITECTURE} "-I${LAB_ROOT}/include" "${LAB_ROOT}/src/async_ptx_helper.cu" -o "${CMAKE_CURRENT_BINARY_DIR}/async_ptx_helper.ptx"
 DEPENDS "${LAB_ROOT}/src/async_ptx_helper.cu" "${LAB_ROOT}/include/hbflab/async_device.cuh" "${LAB_ROOT}/include/hbflab/async_protocol.hpp" "${LAB_ROOT}/include/hbflab/device.cuh" VERBATIM)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/async_ptx_helper_embedded.hpp"
 COMMAND "${CMAKE_COMMAND}" "-DINPUT=${CMAKE_CURRENT_BINARY_DIR}/async_ptx_helper.ptx" "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/async_ptx_helper_embedded.hpp" -DSYMBOL=embedded_async_ptx_helper -P "${LAB_ROOT}/cmake/embed_ptx.cmake"
 DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/async_ptx_helper.ptx" "${LAB_ROOT}/cmake/embed_ptx.cmake" VERBATIM)

add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/weight_kernel.ptx"
 COMMAND "${CMAKE_CUDA_COMPILER}" --ptx  -std=c++17 -arch=${HBFLAB_PTX_ARCHITECTURE}  "${LAB_ROOT}/adapters/llama_cpp/integrated_weight_mul.cu" -o "${CMAKE_CURRENT_BINARY_DIR}/weight_kernel.ptx"
 DEPENDS "${LAB_ROOT}/adapters/llama_cpp/integrated_weight_mul.cu" "${LAB_ROOT}/include/hbflab/async_device.cuh" "${LAB_ROOT}/include/hbflab/async_protocol.hpp" "${LAB_ROOT}/include/hbflab/device.cuh" VERBATIM)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/weight_kernel_embedded.hpp"
 COMMAND "${CMAKE_COMMAND}" "-DINPUT=${CMAKE_CURRENT_BINARY_DIR}/weight_kernel.ptx" "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/weight_kernel_embedded.hpp" -DSYMBOL=embedded_weight_kernel -P "${LAB_ROOT}/cmake/embed_ptx.cmake"
 DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/weight_kernel.ptx" "${LAB_ROOT}/cmake/embed_ptx.cmake" VERBATIM)

add_library(realgpu_runtime STATIC "${LAB_ROOT}/src/backend.cpp" "${LAB_ROOT}/src/async_runtime.cu" "${LAB_ROOT}/src/live_runtime.cpp" "${LAB_ROOT}/src/capacity.cpp" "${LAB_ROOT}/src/page_file.cpp" "${LAB_ROOT}/src/environment.cpp" "${LAB_ROOT}/src/kv.cpp" "${LAB_ROOT}/src/capacity_gpu.cpp" "${LAB_ROOT}/src/async_module.cpp" "${LAB_ROOT}/src/ptx.cpp" "${LAB_ROOT}/src/sha256.cpp" "${CMAKE_CURRENT_BINARY_DIR}/async_ptx_helper_embedded.hpp")
target_include_directories(realgpu_runtime PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" PUBLIC "${LAB_ROOT}/include")
target_link_libraries(realgpu_runtime PUBLIC HBFEventLab::core CUDA::cudart CUDA::cuda_driver)
set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
set(GGML_CUDA ON CACHE BOOL "" FORCE)
set(GGML_CUDA_GRAPHS OFF CACHE BOOL "" FORCE)
set(GGML_CUDA_FA OFF CACHE BOOL "" FORCE)
foreach(flag COMMON TESTS TOOLS EXAMPLES SERVER APP)
  set(LLAMA_BUILD_${flag} OFF CACHE BOOL "" FORCE)
endforeach()
set(LLAMA_OPENSSL OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_NUMBER 0 CACHE STRING "Pinned vendored framework build number" FORCE)
set(LLAMA_BUILD_COMMIT "b3fa69955ea81c3b60e77e3c1e2224c4a826ffb9" CACHE STRING "" FORCE)
# ggml's independent Git probe would otherwise walk up to this monorepo.
# A defined empty GIT_EXE suppresses find_program without changing vendor files.
set(GIT_EXE "")
set(GGML_BUILD_COMMIT "${LLAMA_BUILD_COMMIT}")
set(GGML_GIT_DIRTY 0)
add_subdirectory("${LLAMA_SOURCE}" llama)
add_custom_target(realgpu_kernel DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/weight_kernel_embedded.hpp")
add_dependencies(ggml-cuda realgpu_kernel)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/weight_kernel_embedded.hpp" TARGET_DIRECTORY ggml-cuda PROPERTIES GENERATED TRUE)
target_sources(ggml-cuda PRIVATE "${LAB_ROOT}/adapters/llama_cpp/integrated_weights.cu" "${LAB_ROOT}/adapters/llama_cpp/integrated_kv.cu" "${LAB_ROOT}/adapters/llama_cpp/integrated_ptx.cpp" "${LAB_ROOT}/adapters/llama_cpp/integrated_profile.cpp" "${CMAKE_CURRENT_BINARY_DIR}/weight_kernel_embedded.hpp")
set_source_files_properties("${LAB_ROOT}/adapters/llama_cpp/integrated_weights.cu" TARGET_DIRECTORY ggml-cuda PROPERTIES INCLUDE_DIRECTORIES "${LLAMA_SOURCE}/ggml/src/ggml-cuda")
target_include_directories(ggml-cuda PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${LAB_ROOT}/adapters/llama_cpp" "${LLAMA_SOURCE}/vendor")
target_compile_definitions(ggml-cuda PRIVATE HBFLAB_M2_BUILD)
target_link_libraries(ggml-cuda PRIVATE realgpu_runtime)
set(HBFLAB_CUPTI_ROOT "" CACHE PATH "Optional standalone CUPTI SDK root")
find_path(M2_CUPTI_INCLUDE cupti.h HINTS "${HBFLAB_CUPTI_ROOT}/include" "${CUDAToolkit_ROOT}/include" "${CUDAToolkit_ROOT}/extras/CUPTI/include" REQUIRED)
find_library(M2_CUPTI_LIBRARY NAMES cupti HINTS "${HBFLAB_CUPTI_ROOT}/lib" "${HBFLAB_CUPTI_ROOT}/lib/x64" "${HBFLAB_CUPTI_ROOT}/lib64" "${CUDAToolkit_ROOT}/lib" "${CUDAToolkit_ROOT}/lib/x64" "${CUDAToolkit_ROOT}/lib64" "${CUDAToolkit_ROOT}/extras/CUPTI/lib64" "${CUDAToolkit_ROOT}/extras/CUPTI/lib/x64" REQUIRED)
add_executable(realgpu_llama "${LAB_ROOT}/adapters/llama_cpp/integrated_runner.cpp")
target_include_directories(realgpu_llama PRIVATE "${LAB_ROOT}/include" "${LAB_ROOT}/adapters/llama_cpp" "${LLAMA_SOURCE}/vendor" "${M2_CUPTI_INCLUDE}")
target_link_libraries(realgpu_llama PRIVATE llama ggml-cuda CUDA::cudart ${M2_CUPTI_LIBRARY})
target_compile_definitions(realgpu_llama PRIVATE M0_LLAMA_COMMIT="${LLAMA_BUILD_COMMIT}")
if(MSVC)
  target_compile_options(realgpu_llama PRIVATE /utf-8 /W4)
endif()

add_executable(realgpu_attention_test "${LAB_ROOT}/adapters/llama_cpp/integrated_attention_test.cu" "${LAB_ROOT}/adapters/llama_cpp/integrated_kv.cu" "${LAB_ROOT}/adapters/llama_cpp/integrated_profile.cpp")
target_include_directories(realgpu_attention_test PRIVATE "${LAB_ROOT}/adapters/llama_cpp" "${LLAMA_SOURCE}/vendor")
target_compile_definitions(realgpu_attention_test PRIVATE HBFLAB_M2_BUILD)
target_link_libraries(realgpu_attention_test PRIVATE realgpu_runtime ggml-base CUDA::cudart)

add_executable(realgpu_ptx_test "${LAB_ROOT}/adapters/llama_cpp/integrated_ptx_test.cpp" "${CMAKE_CURRENT_BINARY_DIR}/weight_kernel_embedded.hpp")
add_dependencies(realgpu_ptx_test realgpu_kernel)
target_include_directories(realgpu_ptx_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${LLAMA_SOURCE}/vendor")
target_link_libraries(realgpu_ptx_test PRIVATE realgpu_runtime CUDA::cudart)
