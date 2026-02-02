# Copyright (c) 2025 The Freycoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# Freycoin GPU Acceleration Library
#
# This library provides GPU-accelerated Fermat primality testing:
# - CUDA support for NVIDIA GPUs (compute capability 3.0+)
# - OpenCL support for AMD/Intel/NVIDIA GPUs

# OpenCL support (always try to find)
option(WITH_OPENCL "Enable OpenCL GPU support" ON)

if(WITH_OPENCL)
    find_package(OpenCL)
    if(OpenCL_FOUND)
        message(STATUS "OpenCL found: ${OpenCL_VERSION_STRING}")
        set(HAVE_OPENCL TRUE)
    else()
        message(STATUS "OpenCL not found - GPU mining will be limited to CUDA")
        set(HAVE_OPENCL FALSE)
    endif()
endif()

# CUDA support (optional)
option(WITH_CUDA "Enable CUDA GPU support" ON)

if(WITH_CUDA)
    include(CheckLanguage)
    check_language(CUDA)
    if(CMAKE_CUDA_COMPILER)
        enable_language(CUDA)
        message(STATUS "CUDA found: ${CMAKE_CUDA_COMPILER}")
        set(HAVE_CUDA TRUE)

        # Set CUDA architecture (default to common architectures)
        if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
            set(CMAKE_CUDA_ARCHITECTURES 50 52 60 61 70 75 80 86)
        endif()
    else()
        message(STATUS "CUDA not found - GPU mining will be limited to OpenCL")
        set(HAVE_CUDA FALSE)
    endif()
endif()

# Create GPU library
add_library(freycoin_gpu STATIC EXCLUDE_FROM_ALL
    cuda_fermat.h
    opencl_fermat.h
)

# Add OpenCL implementation
if(HAVE_OPENCL)
    target_sources(freycoin_gpu PRIVATE
        opencl_fermat.cpp
        fermat.cl
        fermat_cl_source.h
    )
    target_link_libraries(freycoin_gpu PRIVATE OpenCL::OpenCL)
    target_compile_definitions(freycoin_gpu PRIVATE HAVE_OPENCL=1)
else()
    # Stub implementation
    target_sources(freycoin_gpu PRIVATE opencl_fermat.cpp)
endif()

# Add CUDA implementation
if(HAVE_CUDA)
    target_sources(freycoin_gpu PRIVATE fermat.cu)
    set_target_properties(freycoin_gpu PROPERTIES
        CUDA_SEPARABLE_COMPILATION ON
        CUDA_STANDARD 14
    )
    target_compile_definitions(freycoin_gpu PRIVATE HAVE_CUDA=1)
endif()

target_include_directories(freycoin_gpu
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)

target_link_libraries(freycoin_gpu
    PRIVATE
        core_interface
)

# Export compile definitions for other targets
if(HAVE_CUDA)
    set(FREYCOIN_HAVE_CUDA TRUE CACHE INTERNAL "CUDA support available")
endif()
if(HAVE_OPENCL)
    set(FREYCOIN_HAVE_OPENCL TRUE CACHE INTERNAL "OpenCL support available")
endif()

# Set compile options
target_compile_options(freycoin_gpu PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra>
)
