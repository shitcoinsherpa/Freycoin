// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * Dynamic CUDA Driver API Loader
 *
 * Loads nvcuda.dll / libcuda.so at runtime via LoadLibrary/dlopen.
 * No CUDA Toolkit required at build time. Pre-compiled binaries work
 * on any system with NVIDIA GPU drivers installed.
 *
 * The CUDA Driver API ships with every NVIDIA display driver — unlike
 * the CUDA Runtime API (cudart), which requires the CUDA Toolkit.
 * This means any user with an NVIDIA GPU and up-to-date drivers can
 * use CUDA acceleration without installing any SDK.
 *
 * Pattern mirrors opencl_loader.h for consistency.
 */

#ifndef FREYCOIN_GPU_CUDA_LOADER_H
#define FREYCOIN_GPU_CUDA_LOADER_H

#include <cstddef>
#include <cstdint>

// ============================================================================
// CUDA Driver API Type Definitions (from cuda.h)
// Defined inline so we never need the CUDA Toolkit headers.
// ============================================================================

typedef int CUresult;
typedef int CUdevice;
typedef struct CUctx_st*  CUcontext;
typedef struct CUmod_st*  CUmodule;
typedef struct CUfunc_st* CUfunction;
typedef struct CUstream_st* CUstream;

// 64-bit device pointer (CUDA 3.2+)
typedef unsigned long long CUdeviceptr;

// Device attributes
typedef enum CUdevice_attribute_enum {
    CU_DEVICE_ATTRIBUTE_MAX_THREADS_PER_BLOCK = 1,
    CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_X = 2,
    CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_X = 5,
    CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT = 16,
    CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR = 75,
    CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR = 76,
} CUdevice_attribute;

// JIT compiler options
typedef enum CUjit_option_enum {
    CU_JIT_MAX_REGISTERS = 0,
    CU_JIT_THREADS_PER_BLOCK = 1,
    CU_JIT_INFO_LOG_BUFFER = 3,
    CU_JIT_INFO_LOG_BUFFER_SIZE_BYTES = 4,
    CU_JIT_ERROR_LOG_BUFFER = 5,
    CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES = 6,
    CU_JIT_TARGET = 9,
} CUjit_option;

// ============================================================================
// CUDA Constants
// ============================================================================

#define CUDA_SUCCESS 0

// ============================================================================
// CUDA Driver API Function Pointer Typedefs
// ============================================================================

// Initialization
typedef CUresult (*pfn_cuInit)(unsigned int Flags);

// Device management
typedef CUresult (*pfn_cuDeviceGetCount)(int* count);
typedef CUresult (*pfn_cuDeviceGet)(CUdevice* device, int ordinal);
typedef CUresult (*pfn_cuDeviceGetName)(char* name, int len, CUdevice dev);
typedef CUresult (*pfn_cuDeviceTotalMem)(size_t* bytes, CUdevice dev);
typedef CUresult (*pfn_cuDeviceGetAttribute)(int* pi, CUdevice_attribute attrib, CUdevice dev);

// Context management
typedef CUresult (*pfn_cuCtxCreate)(CUcontext* pctx, unsigned int flags, CUdevice dev);
typedef CUresult (*pfn_cuCtxDestroy)(CUcontext ctx);
typedef CUresult (*pfn_cuCtxSetCurrent)(CUcontext ctx);

// Module management (PTX loading)
typedef CUresult (*pfn_cuModuleLoadDataEx)(CUmodule* module, const void* image,
                                           unsigned int numOptions, CUjit_option* options,
                                           void** optionValues);
typedef CUresult (*pfn_cuModuleLoadData)(CUmodule* module, const void* image);
typedef CUresult (*pfn_cuModuleGetFunction)(CUfunction* hfunc, CUmodule hmod, const char* name);
typedef CUresult (*pfn_cuModuleUnload)(CUmodule hmod);

// Memory management (v2 — 64-bit device pointers, CUDA 3.2+)
typedef CUresult (*pfn_cuMemAlloc)(CUdeviceptr* dptr, size_t bytesize);
typedef CUresult (*pfn_cuMemFree)(CUdeviceptr dptr);
typedef CUresult (*pfn_cuMemcpyHtoD)(CUdeviceptr dstDevice, const void* srcHost, size_t ByteCount);
typedef CUresult (*pfn_cuMemcpyDtoH)(void* dstHost, CUdeviceptr srcDevice, size_t ByteCount);

// Kernel launch
typedef CUresult (*pfn_cuLaunchKernel)(CUfunction f,
                                        unsigned int gridDimX, unsigned int gridDimY, unsigned int gridDimZ,
                                        unsigned int blockDimX, unsigned int blockDimY, unsigned int blockDimZ,
                                        unsigned int sharedMemBytes, CUstream hStream,
                                        void** kernelParams, void** extra);

// Synchronization
typedef CUresult (*pfn_cuCtxSynchronize)(void);

// ============================================================================
// Function Pointer Globals (defined in cuda_loader.cpp)
// ============================================================================

extern pfn_cuInit               cu_cuInit;
extern pfn_cuDeviceGetCount     cu_cuDeviceGetCount;
extern pfn_cuDeviceGet          cu_cuDeviceGet;
extern pfn_cuDeviceGetName      cu_cuDeviceGetName;
extern pfn_cuDeviceTotalMem     cu_cuDeviceTotalMem;
extern pfn_cuDeviceGetAttribute cu_cuDeviceGetAttribute;
extern pfn_cuCtxCreate          cu_cuCtxCreate;
extern pfn_cuCtxDestroy         cu_cuCtxDestroy;
extern pfn_cuCtxSetCurrent      cu_cuCtxSetCurrent;
extern pfn_cuModuleLoadDataEx   cu_cuModuleLoadDataEx;
extern pfn_cuModuleLoadData     cu_cuModuleLoadData;
extern pfn_cuModuleGetFunction  cu_cuModuleGetFunction;
extern pfn_cuModuleUnload       cu_cuModuleUnload;
extern pfn_cuMemAlloc           cu_cuMemAlloc;
extern pfn_cuMemFree            cu_cuMemFree;
extern pfn_cuMemcpyHtoD         cu_cuMemcpyHtoD;
extern pfn_cuMemcpyDtoH         cu_cuMemcpyDtoH;
extern pfn_cuLaunchKernel       cu_cuLaunchKernel;
extern pfn_cuCtxSynchronize     cu_cuCtxSynchronize;

// ============================================================================
// Loader API
// ============================================================================

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Attempt to load CUDA Driver API library at runtime.
 * @return 0 on success, -1 if library not found, -2 if symbols missing
 */
int cuda_load(void);

/**
 * Unload CUDA library and clear function pointers.
 */
void cuda_unload(void);

/**
 * Check if CUDA Driver API was successfully loaded.
 * @return 1 if loaded, 0 if not
 */
int cuda_is_loaded(void);

#ifdef __cplusplus
}
#endif

#endif // FREYCOIN_GPU_CUDA_LOADER_H
