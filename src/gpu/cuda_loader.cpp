// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * Dynamic CUDA Driver API Loader Implementation
 *
 * Loads nvcuda.dll / libcuda.so at runtime using platform-native
 * dynamic library loading. No CUDA Toolkit needed at build time.
 *
 * Many CUDA Driver API functions have _v2 suffixes in the actual
 * exported symbols (since CUDA 3.2). We try _v2 first, then fall
 * back to the base name for older drivers.
 */

#include "cuda_loader.h"

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

pfn_cuInit               cu_cuInit = nullptr;
pfn_cuDeviceGetCount     cu_cuDeviceGetCount = nullptr;
pfn_cuDeviceGet          cu_cuDeviceGet = nullptr;
pfn_cuDeviceGetName      cu_cuDeviceGetName = nullptr;
pfn_cuDeviceTotalMem     cu_cuDeviceTotalMem = nullptr;
pfn_cuDeviceGetAttribute cu_cuDeviceGetAttribute = nullptr;
pfn_cuCtxCreate          cu_cuCtxCreate = nullptr;
pfn_cuCtxDestroy         cu_cuCtxDestroy = nullptr;
pfn_cuCtxSetCurrent      cu_cuCtxSetCurrent = nullptr;
pfn_cuModuleLoadDataEx   cu_cuModuleLoadDataEx = nullptr;
pfn_cuModuleLoadData     cu_cuModuleLoadData = nullptr;
pfn_cuModuleGetFunction  cu_cuModuleGetFunction = nullptr;
pfn_cuModuleUnload       cu_cuModuleUnload = nullptr;
pfn_cuMemAlloc           cu_cuMemAlloc = nullptr;
pfn_cuMemFree            cu_cuMemFree = nullptr;
pfn_cuMemcpyHtoD         cu_cuMemcpyHtoD = nullptr;
pfn_cuMemcpyDtoH         cu_cuMemcpyDtoH = nullptr;
pfn_cuLaunchKernel       cu_cuLaunchKernel = nullptr;
pfn_cuCtxSynchronize     cu_cuCtxSynchronize = nullptr;

// ============================================================================
// Library state
// ============================================================================

static lib_handle_t g_cuda_lib = nullptr;
static int g_loaded = 0;

// ============================================================================
// Helpers: resolve symbols with _v2 fallback
// ============================================================================

static bool resolve(lib_handle_t lib, const char* name, void** out) {
    *out = (void*)LIB_SYM(lib, name);
    if (!*out) {
        fprintf(stderr, "CUDA loader: missing symbol '%s'\n", name);
        return false;
    }
    return true;
}

/** Try name_v2 first (CUDA 3.2+), fall back to name */
static bool resolve_v2(lib_handle_t lib, const char* name, void** out) {
    char v2_name[128];
    snprintf(v2_name, sizeof(v2_name), "%s_v2", name);

    *out = (void*)LIB_SYM(lib, v2_name);
    if (*out) return true;

    /* Fall back to non-_v2 name */
    *out = (void*)LIB_SYM(lib, name);
    if (*out) return true;

    fprintf(stderr, "CUDA loader: missing symbol '%s' (tried _v2)\n", name);
    return false;
}

// ============================================================================
// Public API
// ============================================================================

int cuda_load(void) {
    if (g_loaded) return 0;

    // Try platform-specific library names
#ifdef _WIN32
    const char* lib_names[] = { "nvcuda.dll", nullptr };
#elif defined(__APPLE__)
    const char* lib_names[] = {
        "/usr/local/cuda/lib/libcuda.dylib",
        "libcuda.dylib",
        nullptr
    };
#else
    const char* lib_names[] = {
        "libcuda.so.1",
        "libcuda.so",
        nullptr
    };
#endif

    for (int i = 0; lib_names[i]; i++) {
        g_cuda_lib = LIB_LOAD(lib_names[i]);
        if (g_cuda_lib) break;
    }

    if (!g_cuda_lib) {
        return -1;  // Library not found (no NVIDIA driver)
    }

    // Resolve all function pointers
    bool ok = true;

    // Functions that DON'T have _v2 variants
    ok = resolve(g_cuda_lib, "cuInit", (void**)&cu_cuInit) && ok;
    ok = resolve(g_cuda_lib, "cuDeviceGetCount", (void**)&cu_cuDeviceGetCount) && ok;
    ok = resolve(g_cuda_lib, "cuDeviceGet", (void**)&cu_cuDeviceGet) && ok;
    ok = resolve(g_cuda_lib, "cuDeviceGetName", (void**)&cu_cuDeviceGetName) && ok;
    ok = resolve(g_cuda_lib, "cuDeviceGetAttribute", (void**)&cu_cuDeviceGetAttribute) && ok;
    ok = resolve(g_cuda_lib, "cuModuleLoadDataEx", (void**)&cu_cuModuleLoadDataEx) && ok;
    ok = resolve(g_cuda_lib, "cuModuleLoadData", (void**)&cu_cuModuleLoadData) && ok;
    ok = resolve(g_cuda_lib, "cuModuleGetFunction", (void**)&cu_cuModuleGetFunction) && ok;
    ok = resolve(g_cuda_lib, "cuModuleUnload", (void**)&cu_cuModuleUnload) && ok;
    ok = resolve(g_cuda_lib, "cuLaunchKernel", (void**)&cu_cuLaunchKernel) && ok;
    ok = resolve(g_cuda_lib, "cuCtxSynchronize", (void**)&cu_cuCtxSynchronize) && ok;

    // Functions that HAVE _v2 variants (CUDA 3.2+ memory/context APIs)
    ok = resolve_v2(g_cuda_lib, "cuDeviceTotalMem", (void**)&cu_cuDeviceTotalMem) && ok;
    ok = resolve_v2(g_cuda_lib, "cuCtxCreate", (void**)&cu_cuCtxCreate) && ok;
    ok = resolve_v2(g_cuda_lib, "cuCtxDestroy", (void**)&cu_cuCtxDestroy) && ok;
    ok = resolve_v2(g_cuda_lib, "cuCtxSetCurrent", (void**)&cu_cuCtxSetCurrent) && ok;
    ok = resolve_v2(g_cuda_lib, "cuMemAlloc", (void**)&cu_cuMemAlloc) && ok;
    ok = resolve_v2(g_cuda_lib, "cuMemFree", (void**)&cu_cuMemFree) && ok;
    ok = resolve_v2(g_cuda_lib, "cuMemcpyHtoD", (void**)&cu_cuMemcpyHtoD) && ok;
    ok = resolve_v2(g_cuda_lib, "cuMemcpyDtoH", (void**)&cu_cuMemcpyDtoH) && ok;

    if (!ok) {
        cuda_unload();
        return -2;  // Missing symbols
    }

    // Initialize the driver
    CUresult res = cu_cuInit(0);
    if (res != CUDA_SUCCESS) {
        cuda_unload();
        return -3;  // Driver initialization failed
    }

    g_loaded = 1;
    return 0;
}

void cuda_unload(void) {
    if (g_cuda_lib) {
        LIB_CLOSE(g_cuda_lib);
        g_cuda_lib = nullptr;
    }

    cu_cuInit = nullptr;
    cu_cuDeviceGetCount = nullptr;
    cu_cuDeviceGet = nullptr;
    cu_cuDeviceGetName = nullptr;
    cu_cuDeviceTotalMem = nullptr;
    cu_cuDeviceGetAttribute = nullptr;
    cu_cuCtxCreate = nullptr;
    cu_cuCtxDestroy = nullptr;
    cu_cuCtxSetCurrent = nullptr;
    cu_cuModuleLoadDataEx = nullptr;
    cu_cuModuleLoadData = nullptr;
    cu_cuModuleGetFunction = nullptr;
    cu_cuModuleUnload = nullptr;
    cu_cuMemAlloc = nullptr;
    cu_cuMemFree = nullptr;
    cu_cuMemcpyHtoD = nullptr;
    cu_cuMemcpyDtoH = nullptr;
    cu_cuLaunchKernel = nullptr;
    cu_cuCtxSynchronize = nullptr;

    g_loaded = 0;
}

int cuda_is_loaded(void) {
    return g_loaded;
}
