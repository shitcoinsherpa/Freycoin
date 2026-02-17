# Bundled gwnum library (George Woltman's Number library)
# From GIMPS/Prime95 - provides FFT-based modular arithmetic.
#
# Uses pre-assembled x86-64 objects for the performance-critical FFT routines
# (linux64/gwnum.a for Linux, amd64/*.obj for Windows), plus C/C++ sources
# compiled at build time for the setup/utility code.
#
# x86-64 only. On other architectures, this library is not built and
# fast_nextprime falls back to GMP's mpz_nextprime.

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    message(STATUS "gwnum: skipping (x86-64 only, this is ${CMAKE_SYSTEM_PROCESSOR})")
    return()
endif()

# C sources compiled at build time
set(GWNUM_C_SOURCES
    cpuid.c
    gwnum.c
    gwtables.c
    gwini.c
    gwutil.c
    giants.c
    radix.c
    ecmstag1.c
    gwbench_stub.c  # replaces gwbench.c — avoids sqlite3.c conflict with Bitcoin Core
)

# C++ sources
set(GWNUM_CXX_SOURCES
    gwthread.cpp
    gwdbldbl.cpp
)

# Create imported library from pre-built assembly archive
if(WIN32 OR MINGW)
    # Windows: create archive from pre-built COFF .obj files at BUILD time.
    # Using add_custom_command ensures CMAKE_AR is the cross-compiler's ar
    # (x86_64-w64-mingw32-ar), not the host ar.
    file(GLOB GWNUM_ASM_OBJS "${CMAKE_CURRENT_SOURCE_DIR}/amd64/*.obj")
    set(GWNUM_ASM_ARCHIVE "${CMAKE_CURRENT_BINARY_DIR}/libgwnum_asm.a")
    add_custom_command(
        OUTPUT ${GWNUM_ASM_ARCHIVE}
        COMMAND ${CMAKE_AR} rcs ${GWNUM_ASM_ARCHIVE} ${GWNUM_ASM_OBJS}
        DEPENDS ${GWNUM_ASM_OBJS}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Creating gwnum assembly archive from COFF objects"
    )
    add_custom_target(gwnum_asm_build DEPENDS ${GWNUM_ASM_ARCHIVE})
    add_library(gwnum_asm STATIC IMPORTED)
    set_target_properties(gwnum_asm PROPERTIES IMPORTED_LOCATION ${GWNUM_ASM_ARCHIVE})
    add_dependencies(gwnum_asm gwnum_asm_build)
else()
    # Linux: use pre-built ELF archive directly
    add_library(gwnum_asm STATIC IMPORTED)
    set_target_properties(gwnum_asm PROPERTIES
        IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/linux64/gwnum.a
    )
endif()

# Build the C/C++ sources
add_library(gwnum_sources STATIC EXCLUDE_FROM_ALL
    ${GWNUM_C_SOURCES}
    ${GWNUM_CXX_SOURCES}
)

target_compile_definitions(gwnum_sources PRIVATE X86_64)
if(WIN32 OR MINGW)
    target_compile_definitions(gwnum_sources PRIVATE WINDOWS64)
endif()

target_include_directories(gwnum_sources
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/qd
)

# ecmstag1.c includes gmp.h — need GMP include path for cross-compilation
target_link_libraries(gwnum_sources PRIVATE GMP::GMP)

# Suppress warnings in third-party code
target_compile_options(gwnum_sources PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-w>
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-w>
)

# C sources need gnu99 standard
set_source_files_properties(${GWNUM_C_SOURCES} PROPERTIES
    COMPILE_FLAGS "-std=gnu99"
)

# Combined gwnum target — links sources + pre-built assembly.
# NOTE: No INTERFACE include directory here. Consumers must add the include
# path only to files that need it (to avoid gwnum's cpuid.h shadowing GCC's).
add_library(gwnum INTERFACE)
target_link_libraries(gwnum INTERFACE gwnum_sources gwnum_asm)


message(STATUS "gwnum: enabled (bundled, x86-64 FFT arithmetic)")
