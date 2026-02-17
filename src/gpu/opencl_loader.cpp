// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * Dynamic OpenCL Loader Implementation
 *
 * Loads OpenCL.dll / libOpenCL.so at runtime using platform-native
 * dynamic library loading. No OpenCL SDK needed at build time.
 */

#include "opencl_loader.h"

#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
typedef HMODULE lib_handle_t;
#define LIB_LOAD(name)    LoadLibraryA(name)
#define LIB_SYM(lib, sym) GetProcAddress(lib, sym)
#define LIB_CLOSE(lib)    FreeLibrary(lib)
#else
#include <dlfcn.h>
typedef void* lib_handle_t;
#define LIB_LOAD(name)    dlopen(name, RTLD_LAZY)
#define LIB_SYM(lib, sym) dlsym(lib, sym)
#define LIB_CLOSE(lib)    dlclose(lib)
#endif

// ============================================================================
// Function Pointer Globals
// ============================================================================

pfn_clGetPlatformIDs          ocl_clGetPlatformIDs = nullptr;
pfn_clGetDeviceIDs            ocl_clGetDeviceIDs = nullptr;
pfn_clGetDeviceInfo           ocl_clGetDeviceInfo = nullptr;
pfn_clCreateContext           ocl_clCreateContext = nullptr;
pfn_clCreateCommandQueue      ocl_clCreateCommandQueue = nullptr;
pfn_clCreateProgramWithSource ocl_clCreateProgramWithSource = nullptr;
pfn_clBuildProgram            ocl_clBuildProgram = nullptr;
pfn_clGetProgramBuildInfo     ocl_clGetProgramBuildInfo = nullptr;
pfn_clCreateKernel            ocl_clCreateKernel = nullptr;
pfn_clSetKernelArg            ocl_clSetKernelArg = nullptr;
pfn_clEnqueueNDRangeKernel    ocl_clEnqueueNDRangeKernel = nullptr;
pfn_clEnqueueReadBuffer       ocl_clEnqueueReadBuffer = nullptr;
pfn_clCreateBuffer            ocl_clCreateBuffer = nullptr;
pfn_clReleaseMemObject        ocl_clReleaseMemObject = nullptr;
pfn_clReleaseKernel           ocl_clReleaseKernel = nullptr;
pfn_clReleaseProgram          ocl_clReleaseProgram = nullptr;
pfn_clReleaseCommandQueue     ocl_clReleaseCommandQueue = nullptr;
pfn_clReleaseContext          ocl_clReleaseContext = nullptr;

// ============================================================================
// Library state
// ============================================================================

static lib_handle_t g_opencl_lib = nullptr;
static int g_loaded = 0;

// ============================================================================
// Helper: resolve one symbol, return false on failure
// ============================================================================

static bool resolve(lib_handle_t lib, const char* name, void** out) {
    *out = (void*)LIB_SYM(lib, name);
    if (!*out) {
        fprintf(stderr, "OpenCL loader: missing symbol '%s'\n", name);
        return false;
    }
    return true;
}

// ============================================================================
// Public API
// ============================================================================

int opencl_load(void) {
    if (g_loaded) return 0;

    // Try platform-specific library names
#ifdef _WIN32
    const char* lib_names[] = { "OpenCL.dll", nullptr };
#elif defined(__APPLE__)
    const char* lib_names[] = {
        "/System/Library/Frameworks/OpenCL.framework/OpenCL",
        "libOpenCL.dylib",
        nullptr
    };
#else
    const char* lib_names[] = {
        "libOpenCL.so.1",
        "libOpenCL.so",
        nullptr
    };
#endif

    for (int i = 0; lib_names[i]; i++) {
        g_opencl_lib = LIB_LOAD(lib_names[i]);
        if (g_opencl_lib) break;
    }

    if (!g_opencl_lib) {
        return -1;  // Library not found
    }

    // Resolve all function pointers
    bool ok = true;

#define RESOLVE(fn) ok = resolve(g_opencl_lib, #fn, (void**)&ocl_##fn) && ok

    RESOLVE(clGetPlatformIDs);
    RESOLVE(clGetDeviceIDs);
    RESOLVE(clGetDeviceInfo);
    RESOLVE(clCreateContext);
    RESOLVE(clCreateCommandQueue);
    RESOLVE(clCreateProgramWithSource);
    RESOLVE(clBuildProgram);
    RESOLVE(clGetProgramBuildInfo);
    RESOLVE(clCreateKernel);
    RESOLVE(clSetKernelArg);
    RESOLVE(clEnqueueNDRangeKernel);
    RESOLVE(clEnqueueReadBuffer);
    RESOLVE(clCreateBuffer);
    RESOLVE(clReleaseMemObject);
    RESOLVE(clReleaseKernel);
    RESOLVE(clReleaseProgram);
    RESOLVE(clReleaseCommandQueue);
    RESOLVE(clReleaseContext);

#undef RESOLVE

    if (!ok) {
        opencl_unload();
        return -2;  // Missing symbols
    }

    g_loaded = 1;
    return 0;
}

void opencl_unload(void) {
    if (g_opencl_lib) {
        LIB_CLOSE(g_opencl_lib);
        g_opencl_lib = nullptr;
    }

    ocl_clGetPlatformIDs = nullptr;
    ocl_clGetDeviceIDs = nullptr;
    ocl_clGetDeviceInfo = nullptr;
    ocl_clCreateContext = nullptr;
    ocl_clCreateCommandQueue = nullptr;
    ocl_clCreateProgramWithSource = nullptr;
    ocl_clBuildProgram = nullptr;
    ocl_clGetProgramBuildInfo = nullptr;
    ocl_clCreateKernel = nullptr;
    ocl_clSetKernelArg = nullptr;
    ocl_clEnqueueNDRangeKernel = nullptr;
    ocl_clEnqueueReadBuffer = nullptr;
    ocl_clCreateBuffer = nullptr;
    ocl_clReleaseMemObject = nullptr;
    ocl_clReleaseKernel = nullptr;
    ocl_clReleaseProgram = nullptr;
    ocl_clReleaseCommandQueue = nullptr;
    ocl_clReleaseContext = nullptr;

    g_loaded = 0;
}

int opencl_is_loaded(void) {
    return g_loaded;
}
