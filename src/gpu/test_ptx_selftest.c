/**
 * Standalone PTX self-test — loads PTX via CUDA Driver API (same as Freycoin).
 * Compile with any C compiler (no nvcc needed):
 *   cl test_ptx_selftest.c /link nvcuda.lib
 *   gcc test_ptx_selftest.c -lnvcuda -o test_ptx_selftest
 *
 * Or just run from Freycoin's debug log by adding verbose selftest output.
 */
#include <stdio.h>
#include <stdint.h>
#include <windows.h>

/* Minimal CUDA Driver API types */
typedef int CUresult;
typedef int CUdevice;
typedef struct CUctx_st* CUcontext;
typedef struct CUmod_st* CUmodule;
typedef struct CUfunc_st* CUfunction;
typedef unsigned long long CUdeviceptr;

#define CUDA_SUCCESS 0

/* Function pointer types */
typedef CUresult (*pfn_cuInit)(unsigned int);
typedef CUresult (*pfn_cuDeviceGet)(CUdevice*, int);
typedef CUresult (*pfn_cuDeviceGetCount)(int*);
typedef CUresult (*pfn_cuCtxCreate)(CUcontext*, unsigned int, CUdevice);
typedef CUresult (*pfn_cuCtxDestroy)(CUcontext);
typedef CUresult (*pfn_cuModuleLoadData)(CUmodule*, const void*);
typedef CUresult (*pfn_cuModuleGetFunction)(CUfunction*, CUmodule, const char*);
typedef CUresult (*pfn_cuMemAlloc)(CUdeviceptr*, size_t);
typedef CUresult (*pfn_cuMemFree)(CUdeviceptr);
typedef CUresult (*pfn_cuMemcpyHtoD)(CUdeviceptr, const void*, size_t);
typedef CUresult (*pfn_cuMemcpyDtoH)(void*, CUdeviceptr, size_t);
typedef CUresult (*pfn_cuLaunchKernel)(CUfunction, unsigned, unsigned, unsigned,
                                        unsigned, unsigned, unsigned,
                                        unsigned, void*, void**, void**);
typedef CUresult (*pfn_cuCtxSynchronize)(void);
typedef CUresult (*pfn_cuDeviceGetName)(char*, int, CUdevice);

/* Embedded PTX */
static const char* ptx_source =
#include "fermat_ptx_source.h"
;

int main() {
    HMODULE lib = LoadLibraryA("nvcuda.dll");
    if (!lib) { printf("Failed to load nvcuda.dll\n"); return 1; }
    printf("Loaded nvcuda.dll\n");

    pfn_cuInit cuInit = (pfn_cuInit)GetProcAddress(lib, "cuInit");
    pfn_cuDeviceGet cuDeviceGet = (pfn_cuDeviceGet)GetProcAddress(lib, "cuDeviceGet");
    pfn_cuDeviceGetCount cuDeviceGetCount = (pfn_cuDeviceGetCount)GetProcAddress(lib, "cuDeviceGetCount");
    pfn_cuCtxCreate cuCtxCreate = (pfn_cuCtxCreate)GetProcAddress(lib, "cuCtxCreate_v2");
    pfn_cuCtxDestroy cuCtxDestroy = (pfn_cuCtxDestroy)GetProcAddress(lib, "cuCtxDestroy_v2");
    pfn_cuModuleLoadData cuModuleLoadData = (pfn_cuModuleLoadData)GetProcAddress(lib, "cuModuleLoadData");
    pfn_cuModuleGetFunction cuModuleGetFunction = (pfn_cuModuleGetFunction)GetProcAddress(lib, "cuModuleGetFunction");
    pfn_cuMemAlloc cuMemAlloc = (pfn_cuMemAlloc)GetProcAddress(lib, "cuMemAlloc_v2");
    pfn_cuMemFree cuMemFree = (pfn_cuMemFree)GetProcAddress(lib, "cuMemFree_v2");
    pfn_cuMemcpyHtoD cuMemcpyHtoD = (pfn_cuMemcpyHtoD)GetProcAddress(lib, "cuMemcpyHtoD_v2");
    pfn_cuMemcpyDtoH cuMemcpyDtoH = (pfn_cuMemcpyDtoH)GetProcAddress(lib, "cuMemcpyDtoH_v2");
    pfn_cuLaunchKernel cuLaunchKernel = (pfn_cuLaunchKernel)GetProcAddress(lib, "cuLaunchKernel");
    pfn_cuCtxSynchronize cuCtxSynchronize = (pfn_cuCtxSynchronize)GetProcAddress(lib, "cuCtxSynchronize");
    pfn_cuDeviceGetName cuDeviceGetName = (pfn_cuDeviceGetName)GetProcAddress(lib, "cuDeviceGetName");

    if (!cuInit || !cuDeviceGet || !cuCtxCreate || !cuModuleLoadData ||
        !cuModuleGetFunction || !cuMemAlloc || !cuLaunchKernel || !cuCtxSynchronize) {
        printf("Failed to resolve CUDA Driver API symbols\n"); return 1;
    }

    CUresult res;
    res = cuInit(0);
    if (res != CUDA_SUCCESS) { printf("cuInit failed: %d\n", res); return 1; }

    int count;
    cuDeviceGetCount(&count);
    printf("CUDA devices: %d\n", count);

    CUdevice dev;
    cuDeviceGet(&dev, 0);

    char name[256];
    cuDeviceGetName(name, 256, dev);
    printf("Device: %s\n", name);

    CUcontext ctx;
    res = cuCtxCreate(&ctx, 0, dev);
    if (res != CUDA_SUCCESS) { printf("cuCtxCreate failed: %d\n", res); return 1; }

    CUmodule mod;
    res = cuModuleLoadData(&mod, ptx_source);
    if (res != CUDA_SUCCESS) { printf("cuModuleLoadData failed: %d\n", res); return 1; }
    printf("PTX loaded and JIT-compiled OK\n");

    CUfunction selftest_fn;
    res = cuModuleGetFunction(&selftest_fn, mod, "fermat_selftest");
    if (res != CUDA_SUCCESS) { printf("fermat_selftest not found: %d\n", res); return 1; }

    /* Allocate and zero 4 bytes on device */
    CUdeviceptr d_results;
    cuMemAlloc(&d_results, 4);
    uint8_t zeros[4] = {0};
    cuMemcpyHtoD(d_results, zeros, 4);

    /* Launch: 1 block, 1 thread */
    void* params[] = { &d_results };
    res = cuLaunchKernel(selftest_fn, 1,1,1, 1,1,1, 0, NULL, params, NULL);
    if (res != CUDA_SUCCESS) { printf("Launch failed: %d\n", res); return 1; }

    res = cuCtxSynchronize();
    if (res != CUDA_SUCCESS) { printf("Sync failed: %d\n", res); return 1; }

    uint8_t h[4];
    cuMemcpyDtoH(h, d_results, 4);

    printf("\nSelf-test results:\n");
    printf("  secp256k1 prime (expect 1): %d\n", h[0]);
    printf("  Mersenne M127   (expect 1): %d\n", h[1]);
    printf("  Composite 15    (expect 0): %d\n", h[2]);
    printf("  Sentinel        (expect AA): %02X\n", h[3]);

    int pass = (h[0] == 1 && h[1] == 1 && h[2] == 0 && h[3] == 0xAA);
    printf("\n%s\n", pass ? "ALL TESTS PASSED" : "TESTS FAILED");

    /* Also test batch kernel */
    printf("\n=== Batch kernel test ===\n");
    CUfunction k320;
    res = cuModuleGetFunction(&k320, mod, "fermat_kernel_320");
    if (res != CUDA_SUCCESS) { printf("fermat_kernel_320 not found: %d\n", res); return 1; }

    uint32_t primes[30] = {
        0xFFFFFC2Fu, 0xFFFFFFFEu, 0xFFFFFFFFu, 0xFFFFFFFFu,
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
        0x00000000u, 0x00000000u,
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x7FFFFFFFu,
        0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
        0x00000000u, 0x00000000u,
        0x0000000Fu, 0x00000000u, 0x00000000u, 0x00000000u,
        0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
        0x00000000u, 0x00000000u
    };

    CUdeviceptr d_primes, d_batch;
    cuMemAlloc(&d_primes, sizeof(primes));
    cuMemAlloc(&d_batch, 3);
    cuMemcpyHtoD(d_primes, primes, sizeof(primes));
    uint8_t bz[3] = {0};
    cuMemcpyHtoD(d_batch, bz, 3);

    uint32_t cnt = 3;
    void* bparams[] = { &d_batch, &d_primes, &cnt };
    res = cuLaunchKernel(k320, 1,1,1, 64,1,1, 0, NULL, bparams, NULL);
    if (res != CUDA_SUCCESS) { printf("Batch launch failed: %d\n", res); return 1; }
    cuCtxSynchronize();

    uint8_t br[3];
    cuMemcpyDtoH(br, d_batch, 3);
    printf("  secp256k1 (expect 1): %d\n", br[0]);
    printf("  M127      (expect 1): %d\n", br[1]);
    printf("  15        (expect 0): %d\n", br[2]);

    cuMemFree(d_results);
    cuMemFree(d_primes);
    cuMemFree(d_batch);
    cuCtxDestroy(ctx);

    return pass ? 0 : 1;
}
