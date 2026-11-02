# Compiled CUDA targets and embedded PTX are separate configuration choices.
if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
  set(CMAKE_CUDA_ARCHITECTURES native CACHE STRING "CUDA target architectures")
endif()
set(HBFLAB_PTX_ARCHITECTURE compute_75 CACHE STRING "Virtual architecture for embedded PTX")
if(NOT HBFLAB_PTX_ARCHITECTURE MATCHES "^compute_[0-9]+[af]?$")
  message(FATAL_ERROR "HBFLAB_PTX_ARCHITECTURE must be a compute_* virtual architecture")
endif()
