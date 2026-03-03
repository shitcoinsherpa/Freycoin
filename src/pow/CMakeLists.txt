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
# - gwnum FFT-accelerated nextprime (x86-64, bundled)

# ─── GPU batch BPSW nextprime (statically linked) ────────────────────────────
#
# Pre-compiled CUDA fatbin kernels are embedded as linkable objects via objcopy.
# The GPU driver loads them at runtime via the CUDA Driver API (nvcuda.dll/libcuda.so).
# No CUDA SDK is needed at build or run time. If no NVIDIA GPU is present,
# gpu_nextprime_init() fails gracefully and fast_nextprime falls back to gwnum → GMP.

set(GPU_ACCEL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/gpu_accel)
set(GPU_ACCEL_FATBINS
    ecpp_tonelli
    ecpp_cornacchia
    ecpp_bpsw
    ecpp_ec
)

# Determine objcopy output format based on target platform
if(WIN32 OR MINGW)
    set(OBJCOPY_FORMAT "pe-x86-64")
else()
    set(OBJCOPY_FORMAT "elf64-x86-64")
endif()

# Convert each .fatbin to a linkable object with _binary_*_fatbin_start/end symbols.
# objcopy symbol names are derived from the input filename, so we cd into the
# directory to get clean names like _binary_ecpp_bpsw_fatbin_start.
set(GPU_FATBIN_OBJECTS "")
foreach(kernel ${GPU_ACCEL_FATBINS})
    set(fatbin_input ${GPU_ACCEL_DIR}/${kernel}.fatbin)
    set(fatbin_output ${CMAKE_CURRENT_BINARY_DIR}/${kernel}_cubin.o)
    add_custom_command(
        OUTPUT ${fatbin_output}
        COMMAND ${CMAKE_OBJCOPY} -I binary -O ${OBJCOPY_FORMAT} -B i386:x86-64
                ${kernel}.fatbin ${fatbin_output}
        WORKING_DIRECTORY ${GPU_ACCEL_DIR}
        DEPENDS ${fatbin_input}
        COMMENT "Embedding GPU fatbin: ${kernel}.fatbin"
    )
    list(APPEND GPU_FATBIN_OBJECTS ${fatbin_output})
endforeach()

add_library(freycoin_pow STATIC EXCLUDE_FROM_ALL
    pow_common.h
    pow_utils.h
    pow_utils.cpp
    pow_processor.h
    pow.h
    pow.cpp
    fast_nextprime.h
    fast_nextprime.cpp
    gpu_coordinator.h
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
    gpu_accel/gpu_nextprime.h
    gpu_accel/gpu_nextprime_lib.cpp
    gpu_accel/ecpp_gpu_shim.h
    gpu_accel/ecpp_gpu_shim.c
    gpu_accel/ecpp_gpu_driver.h
    gpu_accel/ecpp_gpu_driver.cpp
    ${GPU_FATBIN_OBJECTS}
)

target_include_directories(freycoin_pow
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    PRIVATE
        ${GPU_ACCEL_DIR}
)

target_link_libraries(freycoin_pow
    PRIVATE
        core_interface
        MPFR::MPFR
        GMP::GMP
        ${CMAKE_DL_LIBS}
        $<TARGET_NAME_IF_EXISTS:freycoin_gpu>
    PUBLIC
        bitcoin_crypto
)

# Bundled gwnum FFT library (accelerates nextprime by ~2.5x at 12000 bits).
# Built from src/gwnum/ — x86-64 only, skipped on other architectures.
# When gwnum is not available, fast_nextprime falls back to GMP mpz_nextprime.
if(TARGET gwnum)
    target_compile_definitions(freycoin_pow PUBLIC HAVE_GWNUM=1)
    # Only add gwnum include dir to fast_nextprime.cpp to avoid gwnum's cpuid.h
    # shadowing the GCC <cpuid.h> intrinsics header needed by simd_presieve/avx512.
    set_source_files_properties(fast_nextprime.cpp PROPERTIES
        COMPILE_FLAGS "-I${PROJECT_SOURCE_DIR}/src/gwnum"
    )
    target_link_libraries(freycoin_pow PRIVATE gwnum pthread)
endif()

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
