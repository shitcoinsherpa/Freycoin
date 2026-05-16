# Copyright (c) 2025 The Freycoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# Freycoin GPU Acceleration Library
#
# OpenCL is loaded dynamically at runtime — no SDK needed at build time.
# Works for all GPU vendors (NVIDIA, AMD, Intel).

# Create GPU library — OpenCL + CUDA Driver API (both dynamically loaded)
# No SDK required at build time for either path.
add_library(freycoin_gpu STATIC EXCLUDE_FROM_ALL
    opencl_loader.h
    opencl_loader.cpp
    opencl_fermat.h
    opencl_fermat.cpp
    fermat_cl_source.h
    cuda_loader.h
    cuda_loader.cpp
    cuda_fermat.h
    cuda_fermat_driver.cpp
    cgbn_fermat.h
    pinned_pool.h
    pinned_pool.cpp
)

# Platform-specific dynamic linking support
if(WIN32)
    # LoadLibrary is in kernel32 (linked automatically)
else()
    # dlopen/dlsym require -ldl on Linux
    target_link_libraries(freycoin_gpu PRIVATE ${CMAKE_DL_LIBS})
endif()

target_include_directories(freycoin_gpu
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)

target_link_libraries(freycoin_gpu
    PRIVATE
        core_interface
)

# Set compile options
target_compile_options(freycoin_gpu PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra>
)
