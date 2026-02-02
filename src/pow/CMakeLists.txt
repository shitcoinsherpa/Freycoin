# Copyright (c) 2025 The Freycoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# Freycoin PoW Mining Library
#
# This library implements prime gap proof-of-work mining:
# - PoW validation and difficulty calculation
# - Segmented sieve for candidate generation
# - SIMD-accelerated presieve (AVX-512, AVX2, SSE2)
# - BPSW primality testing
# - GPU acceleration via CUDA/OpenCL (optional)

add_library(freycoin_pow STATIC EXCLUDE_FROM_ALL
    pow_common.h
    pow_utils.h
    pow_utils.cpp
    pow_processor.h
    pow.h
    pow.cpp
    wheel_tables.h
    wheel_tables.cpp
    simd_presieve.h
    simd_presieve.cpp
    mining_engine.h
    mining_engine.cpp
)

target_include_directories(freycoin_pow
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)

target_link_libraries(freycoin_pow
    PRIVATE
        core_interface
        $<TARGET_NAME_IF_EXISTS:GMP::GMP>
        $<TARGET_NAME_IF_EXISTS:MPFR::MPFR>
        $<TARGET_NAME_IF_EXISTS:freycoin_gpu>
    PUBLIC
        bitcoin_crypto
)

# GPU support flags (set by gpu/CMakeLists.txt)
if(TARGET freycoin_gpu)
    if(FREYCOIN_HAVE_CUDA)
        target_compile_definitions(freycoin_pow PRIVATE HAVE_CUDA=1)
    endif()
    if(FREYCOIN_HAVE_OPENCL)
        target_compile_definitions(freycoin_pow PRIVATE HAVE_OPENCL=1)
    endif()
endif()

# Platform-specific SIMD support
include(CheckCXXCompilerFlag)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64|i[3456]86")
    # Check for AVX-512 support
    check_cxx_compiler_flag("-mavx512f -mavx512bw" HAVE_AVX512)
    if(HAVE_AVX512)
        target_compile_definitions(freycoin_pow PRIVATE HAVE_AVX512=1)
    endif()

    # Check for AVX2 support
    check_cxx_compiler_flag("-mavx2" HAVE_AVX2)
    if(HAVE_AVX2)
        target_compile_definitions(freycoin_pow PRIVATE HAVE_AVX2=1)
    endif()

    # Check for SSE2 support (almost always available on x86-64)
    check_cxx_compiler_flag("-msse2" HAVE_SSE2)
    if(HAVE_SSE2)
        target_compile_definitions(freycoin_pow PRIVATE HAVE_SSE2=1)
    endif()
endif()

# Set compile options
target_compile_options(freycoin_pow PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra>
)
