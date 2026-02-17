/**
 * Standalone CUDA self-test for Fermat kernel Montgomery math.
 * Compile: nvcc -o test_cuda_selftest test_cuda_selftest.cu -arch=sm_75
 * Run: ./test_cuda_selftest
 */
#include <stdio.h>
#include <stdint.h>
#include <cuda_runtime.h>

/* Import the kernels from fermat.cu by including it directly */
#include "fermat.cu"

int main() {
    cudaSetDevice(0);

    cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, 0);
    printf("GPU: %s (compute %d.%d)\n", prop.name, prop.major, prop.minor);

    /* Test 1: Self-test kernel */
    printf("\n=== Self-test kernel ===\n");
    uint8_t *d_results;
    cudaMalloc(&d_results, 4);
    cudaMemset(d_results, 0, 4);

    fermat_selftest<<<1, 1>>>(d_results);
    cudaDeviceSynchronize();

    cudaError_t err = cudaGetLastError();
    if (err != cudaSuccess) {
        printf("Kernel launch error: %s\n", cudaGetErrorString(err));
        return 1;
    }

    uint8_t h_results[4];
    cudaMemcpy(h_results, d_results, 4, cudaMemcpyDeviceToHost);

    printf("  secp256k1 prime (expect 1): %d\n", h_results[0]);
    printf("  Mersenne M127   (expect 1): %d\n", h_results[1]);
    printf("  Composite 15    (expect 0): %d\n", h_results[2]);
    printf("  Sentinel        (expect AA): %02X\n", h_results[3]);

    int pass = 1;
    if (h_results[0] != 1) { printf("  FAIL: secp256k1\n"); pass = 0; }
    if (h_results[1] != 1) { printf("  FAIL: Mersenne M127\n"); pass = 0; }
    if (h_results[2] != 0) { printf("  FAIL: Composite 15\n"); pass = 0; }
    if (h_results[3] != 0xAA) { printf("  FAIL: Sentinel\n"); pass = 0; }

    if (pass) printf("\nALL TESTS PASSED\n");
    else printf("\nTESTS FAILED\n");

    /* Test 2: Batch kernel with same numbers */
    printf("\n=== Batch kernel (fermat_kernel_320) ===\n");

    /* secp256k1 prime, little-endian 10x uint32_t */
    uint32_t primes[30] = {
        /* secp256k1 */
        0xFFFFFC2Fu, 0xFFFFFFFEu, 0xFFFFFFFFu, 0xFFFFFFFFu,
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
        0x00000000u, 0x00000000u,
        /* M127 */
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x7FFFFFFFu,
        0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
        0x00000000u, 0x00000000u,
        /* 15 (composite) */
        0x0000000Fu, 0x00000000u, 0x00000000u, 0x00000000u,
        0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
        0x00000000u, 0x00000000u
    };

    uint32_t *d_primes;
    uint8_t *d_batch_results;
    cudaMalloc(&d_primes, sizeof(primes));
    cudaMalloc(&d_batch_results, 3);
    cudaMemcpy(d_primes, primes, sizeof(primes), cudaMemcpyHostToDevice);

    fermat_kernel_320<<<1, 64>>>(d_batch_results, d_primes, 3);
    cudaDeviceSynchronize();

    err = cudaGetLastError();
    if (err != cudaSuccess) {
        printf("Batch kernel error: %s\n", cudaGetErrorString(err));
    }

    uint8_t batch_results[3];
    cudaMemcpy(batch_results, d_batch_results, 3, cudaMemcpyDeviceToHost);

    printf("  secp256k1 prime (expect 1): %d\n", batch_results[0]);
    printf("  Mersenne M127   (expect 1): %d\n", batch_results[1]);
    printf("  Composite 15    (expect 0): %d\n", batch_results[2]);

    cudaFree(d_results);
    cudaFree(d_primes);
    cudaFree(d_batch_results);

    return pass ? 0 : 1;
}
