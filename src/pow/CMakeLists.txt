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
    combined_sieve.h
    combined_sieve.cpp
    avx512_primality.h
    avx512_primality.cpp
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
        MPFR::MPFR
        GMP::GMP
        $<TARGET_NAME_IF_EXISTS:freycoin_gpu>
    PUBLIC
        bitcoin_crypto
)

# GPU support flags (set by gpu/CMakeLists.txt)
# OpenCL and CUDA: always available via dynamic loading (no compile flags needed)
# CGBN: optional, requires nvcc + bundled headers
if(TARGET freycoin_gpu)
    if(FREYCOIN_HAVE_CGBN)
        target_compile_definitions(freycoin_pow PRIVATE HAVE_CGBN=1)
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

    # Check for AVX-512 IFMA support (VPMADD52 — Ice Lake+, Zen 4+)
    check_cxx_compiler_flag("-mavx512ifma" HAVE_AVX512_IFMA)
    if(HAVE_AVX512_IFMA AND HAVE_AVX512)
        # Compile avx512_primality.cpp with IFMA enabled
        set_source_files_properties(avx512_primality.cpp PROPERTIES
            COMPILE_FLAGS "-mavx512f -mavx512ifma"
        )
        target_compile_definitions(freycoin_pow PRIVATE HAVE_AVX512_IFMA=1)
        message(STATUS "AVX-512 IFMA support enabled for vectorized primality testing")
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
