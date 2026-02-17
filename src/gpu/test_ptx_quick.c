/**
 * Quick PTX self-test — loads PTX from file at runtime.
 * Compile: gcc -O0 -o test_ptx_quick.exe test_ptx_quick.c
 * Run: test_ptx_quick.exe fermat.ptx
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <windows.h>

typedef int CUresult;
typedef int CUdevice;
typedef struct CUctx_st* CUcontext;
typedef struct CUmod_st* CUmodule;
typedef struct CUfunc_st* CUfunction;
typedef unsigned long long CUdeviceptr;
#define CUDA_SUCCESS 0

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

int main(int argc, char **argv) {
    if (argc < 2) { printf("Usage: %s <fermat.ptx>\n", argv[0]); return 1; }

    /* Read PTX file */
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("Cannot open %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *ptx = (char*)malloc(sz + 1);
    fread(ptx, 1, sz, f);
    ptx[sz] = 0;
    fclose(f);
    printf("Read %ld bytes of PTX\n", sz);

    HMODULE lib = LoadLibraryA("nvcuda.dll");
    if (!lib) { printf("Failed to load nvcuda.dll\n"); return 1; }

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
    res = cuModuleLoadData(&mod, ptx);
    if (res != CUDA_SUCCESS) { printf("cuModuleLoadData (JIT) failed: %d\n", res); return 1; }
    printf("PTX JIT-compiled OK\n");

    /* Self-test kernel */
    CUfunction selftest_fn;
    res = cuModuleGetFunction(&selftest_fn, mod, "fermat_selftest");
    if (res != CUDA_SUCCESS) { printf("fermat_selftest not found: %d\n", res); return 1; }

    CUdeviceptr d_results;
    cuMemAlloc(&d_results, 4);
    uint8_t zeros[4] = {0};
    cuMemcpyHtoD(d_results, zeros, 4);

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

    cuMemFree(d_results);
    cuCtxDestroy(ctx);
    free(ptx);
    return pass ? 0 : 1;
}
