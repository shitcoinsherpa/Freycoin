# Copyright (c) 2025 The Freycoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# Freycoin GPU Acceleration Library
#
# Both OpenCL and CUDA are ALWAYS compiled — loaded dynamically at runtime.
# OpenCL: loads OpenCL.dll / libOpenCL.so (any GPU vendor)
# CUDA:   loads nvcuda.dll / libcuda.so (NVIDIA only, Driver API)
#
# No GPU SDK is needed at build time. PTX kernels are embedded as string
# constants and JIT-compiled by the driver at runtime.
#
# Optional: if nvcc is present, the .cu files can also be compiled directly
# for dev/testing (HAVE_CUDA_NVCC). This is NOT required for shipping.

# Check for nvcc (optional — for dev builds and PTX regeneration)
option(WITH_CUDA_NVCC "Enable nvcc compilation of .cu files (dev only)" ON)

set(HAVE_CUDA_NVCC FALSE)
if(WITH_CUDA_NVCC)
    include(CheckLanguage)
    check_language(CUDA)
    if(CMAKE_CUDA_COMPILER)
        enable_language(CUDA)
        message(STATUS "nvcc found: ${CMAKE_CUDA_COMPILER} (dev builds enabled)")
        set(HAVE_CUDA_NVCC TRUE)

        if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
            set(CMAKE_CUDA_ARCHITECTURES 50 52 60 61 70 75 80 86)
        endif()
    else()
        message(STATUS "nvcc not found — using embedded PTX (this is normal for release builds)")
    endif()
endif()

# Create GPU library — both OpenCL and CUDA always included (dynamic loading)
add_library(freycoin_gpu STATIC EXCLUDE_FROM_ALL
    # OpenCL (dynamic loading, no SDK needed)
    opencl_loader.h
    opencl_loader.cpp
    opencl_fermat.h
    opencl_fermat.cpp
    fermat_cl_source.h

    # CUDA Driver API (dynamic loading, no SDK needed)
    cuda_loader.h
    cuda_loader.cpp
    cuda_fermat.h
    cuda_fermat_driver.cpp
    fermat_ptx_source.h

    # CGBN stubs (always available as interface)
    cgbn_fermat.h
)

# Platform-specific dynamic linking support
if(WIN32)
    # LoadLibrary is in kernel32 (linked automatically)
else()
    # dlopen/dlsym require -ldl on Linux
    target_link_libraries(freycoin_gpu PRIVATE ${CMAKE_DL_LIBS})
endif()

# Optional: nvcc compilation of .cu files (for dev/testing only)
if(HAVE_CUDA_NVCC)
    target_sources(freycoin_gpu PRIVATE fermat.cu)
    set_target_properties(freycoin_gpu PROPERTIES
        CUDA_SEPARABLE_COMPILATION ON
        CUDA_STANDARD 14
    )
    target_compile_definitions(freycoin_gpu PRIVATE HAVE_CUDA_NVCC=1)

    # CGBN (Cooperative Groups Big Numbers) — bundled from NVlabs/CGBN (MIT license)
    set(CGBN_BUNDLED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cgbn_lib/include")
    if(EXISTS "${CGBN_BUNDLED_DIR}/cgbn/cgbn.h")
        message(STATUS "CGBN enabled (bundled): ${CGBN_BUNDLED_DIR}")
        target_sources(freycoin_gpu PRIVATE cgbn_fermat.cu)
        target_include_directories(freycoin_gpu PRIVATE ${CGBN_BUNDLED_DIR})
        target_compile_definitions(freycoin_gpu PRIVATE HAVE_CGBN=1)
        set(FREYCOIN_HAVE_CGBN TRUE CACHE INTERNAL "CGBN support available")
    else()
        message(WARNING "Bundled CGBN headers missing at ${CGBN_BUNDLED_DIR}/cgbn/cgbn.h — CGBN disabled")
    endif()
endif()

target_include_directories(freycoin_gpu
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)

target_link_libraries(freycoin_gpu
    PRIVATE
        core_interface
)

# OpenCL and CUDA are always available (dynamic loading) — no compile flags needed

# Set compile options
target_compile_options(freycoin_gpu PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra>
)
