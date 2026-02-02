// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * OpenCL Fermat Primality Test Implementation
 *
 * In memory of Jonnie Frey (1989-2017), creator of Gapcoin.
 */

#ifdef HAVE_OPENCL

#include "opencl_fermat.h"

#ifdef __APPLE__
#include <OpenCL/opencl.h>
#else
#include <CL/cl.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Global OpenCL state */
static cl_platform_id g_platform = NULL;
static cl_device_id g_device = NULL;
static cl_context g_context = NULL;
static cl_command_queue g_queue = NULL;
static cl_program g_program = NULL;
static cl_kernel g_kernel_320 = NULL;
static cl_kernel g_kernel_352 = NULL;
static int g_initialized = 0;
static char g_device_name[256] = {0};
static size_t g_device_memory = 0;

/* Embedded kernel source (generated from fermat.cl) */
static const char* kernel_source =
#include "fermat_cl_source.h"
;

/* Alternative: Load kernel from file at runtime */
static char* load_kernel_file(const char* filename, size_t* size) {
    FILE* f = fopen(filename, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* source = (char*)malloc(*size + 1);
    if (!source) {
        fclose(f);
        return NULL;
    }

    fread(source, 1, *size, f);
    source[*size] = '\0';
    fclose(f);

    return source;
}

int opencl_fermat_init(int device_id) {
    cl_int err;
    cl_uint num_platforms, num_devices;

    if (g_initialized) return 0;

    /* Get platform */
    err = clGetPlatformIDs(1, &g_platform, &num_platforms);
    if (err != CL_SUCCESS || num_platforms == 0) {
        return -2;  /* No OpenCL available */
    }

    /* Get device */
    err = clGetDeviceIDs(g_platform, CL_DEVICE_TYPE_GPU, 0, NULL, &num_devices);
    if (err != CL_SUCCESS || num_devices == 0) {
        return -2;  /* No GPU devices */
    }

    cl_device_id* devices = (cl_device_id*)malloc(sizeof(cl_device_id) * num_devices);
    err = clGetDeviceIDs(g_platform, CL_DEVICE_TYPE_GPU, num_devices, devices, NULL);
    if (err != CL_SUCCESS) {
        free(devices);
        return -1;
    }

    if ((cl_uint)device_id >= num_devices) {
        device_id = 0;
    }
    g_device = devices[device_id];
    free(devices);

    /* Get device info */
    clGetDeviceInfo(g_device, CL_DEVICE_NAME, sizeof(g_device_name), g_device_name, NULL);
    clGetDeviceInfo(g_device, CL_DEVICE_GLOBAL_MEM_SIZE, sizeof(g_device_memory), &g_device_memory, NULL);

    /* Create context */
    g_context = clCreateContext(NULL, 1, &g_device, NULL, NULL, &err);
    if (err != CL_SUCCESS) {
        return -1;
    }

    /* Create command queue */
#ifdef CL_VERSION_2_0
    g_queue = clCreateCommandQueueWithProperties(g_context, g_device, NULL, &err);
#else
    g_queue = clCreateCommandQueue(g_context, g_device, 0, &err);
#endif
    if (err != CL_SUCCESS) {
        clReleaseContext(g_context);
        return -1;
    }

    /* Try to load kernel from file first, fall back to embedded */
    size_t kernel_size;
    char* source = load_kernel_file("fermat.cl", &kernel_size);
    if (!source) {
        /* Use embedded kernel source */
        source = (char*)kernel_source;
        kernel_size = strlen(kernel_source);
    }

    /* Create program */
    g_program = clCreateProgramWithSource(g_context, 1, (const char**)&source, &kernel_size, &err);
    if (source != kernel_source) {
        free(source);
    }
    if (err != CL_SUCCESS) {
        clReleaseCommandQueue(g_queue);
        clReleaseContext(g_context);
        return -1;
    }

    /* Build program */
    err = clBuildProgram(g_program, 1, &g_device, "-cl-fast-relaxed-math", NULL, NULL);
    if (err != CL_SUCCESS) {
        /* Get build log */
        size_t log_size;
        clGetProgramBuildInfo(g_program, g_device, CL_PROGRAM_BUILD_LOG, 0, NULL, &log_size);
        char* log = (char*)malloc(log_size + 1);
        clGetProgramBuildInfo(g_program, g_device, CL_PROGRAM_BUILD_LOG, log_size, log, NULL);
        fprintf(stderr, "OpenCL build error:\n%s\n", log);
        free(log);

        clReleaseProgram(g_program);
        clReleaseCommandQueue(g_queue);
        clReleaseContext(g_context);
        return -1;
    }

    /* Create kernels */
    g_kernel_320 = clCreateKernel(g_program, "fermat_kernel_320", &err);
    if (err != CL_SUCCESS) {
        clReleaseProgram(g_program);
        clReleaseCommandQueue(g_queue);
        clReleaseContext(g_context);
        return -1;
    }

    g_kernel_352 = clCreateKernel(g_program, "fermat_kernel_352", &err);
    if (err != CL_SUCCESS) {
        clReleaseKernel(g_kernel_320);
        clReleaseProgram(g_program);
        clReleaseCommandQueue(g_queue);
        clReleaseContext(g_context);
        return -1;
    }

    g_initialized = 1;
    return 0;
}

void opencl_fermat_cleanup(void) {
    if (!g_initialized) return;

    if (g_kernel_320) clReleaseKernel(g_kernel_320);
    if (g_kernel_352) clReleaseKernel(g_kernel_352);
    if (g_program) clReleaseProgram(g_program);
    if (g_queue) clReleaseCommandQueue(g_queue);
    if (g_context) clReleaseContext(g_context);

    g_kernel_320 = NULL;
    g_kernel_352 = NULL;
    g_program = NULL;
    g_queue = NULL;
    g_context = NULL;
    g_initialized = 0;
}

int opencl_fermat_batch(uint8_t *h_results, const uint32_t *h_primes,
                        uint32_t count, int bits) {
    if (!g_initialized) return -1;
    if (count == 0) return 0;

    cl_int err;
    int limbs = (bits <= 320) ? 10 : 11;
    size_t primes_size = count * limbs * sizeof(uint32_t);
    size_t results_size = count * sizeof(uint8_t);

    /* Create buffers */
    cl_mem d_primes = clCreateBuffer(g_context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                     primes_size, (void*)h_primes, &err);
    if (err != CL_SUCCESS) return -1;

    cl_mem d_results = clCreateBuffer(g_context, CL_MEM_WRITE_ONLY,
                                      results_size, NULL, &err);
    if (err != CL_SUCCESS) {
        clReleaseMemObject(d_primes);
        return -1;
    }

    /* Select kernel */
    cl_kernel kernel = (bits <= 320) ? g_kernel_320 : g_kernel_352;

    /* Set kernel arguments */
    clSetKernelArg(kernel, 0, sizeof(cl_mem), &d_results);
    clSetKernelArg(kernel, 1, sizeof(cl_mem), &d_primes);
    clSetKernelArg(kernel, 2, sizeof(uint32_t), &count);

    /* Execute kernel */
    size_t global_size = ((count + 63) / 64) * 64;  /* Round up to workgroup size */
    size_t local_size = 64;
    err = clEnqueueNDRangeKernel(g_queue, kernel, 1, NULL, &global_size, &local_size, 0, NULL, NULL);
    if (err != CL_SUCCESS) {
        clReleaseMemObject(d_results);
        clReleaseMemObject(d_primes);
        return -1;
    }

    /* Read results */
    err = clEnqueueReadBuffer(g_queue, d_results, CL_TRUE, 0, results_size, h_results, 0, NULL, NULL);
    if (err != CL_SUCCESS) {
        clReleaseMemObject(d_results);
        clReleaseMemObject(d_primes);
        return -1;
    }

    /* Cleanup */
    clReleaseMemObject(d_results);
    clReleaseMemObject(d_primes);

    return 0;
}

int opencl_get_device_count(void) {
    cl_platform_id platform;
    cl_uint num_platforms, num_devices;

    if (clGetPlatformIDs(1, &platform, &num_platforms) != CL_SUCCESS || num_platforms == 0) {
        return 0;
    }

    if (clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 0, NULL, &num_devices) != CL_SUCCESS) {
        return 0;
    }

    return (int)num_devices;
}

const char* opencl_get_device_name(int device_id) {
    static char name[256];
    cl_platform_id platform;
    cl_uint num_platforms, num_devices;

    if (clGetPlatformIDs(1, &platform, &num_platforms) != CL_SUCCESS) {
        return "Unknown";
    }

    cl_device_id* devices = (cl_device_id*)malloc(sizeof(cl_device_id) * 16);
    if (clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 16, devices, &num_devices) != CL_SUCCESS) {
        free(devices);
        return "Unknown";
    }

    if ((cl_uint)device_id >= num_devices) {
        device_id = 0;
    }

    clGetDeviceInfo(devices[device_id], CL_DEVICE_NAME, sizeof(name), name, NULL);
    free(devices);

    return name;
}

size_t opencl_get_device_memory(int device_id) {
    cl_platform_id platform;
    cl_uint num_platforms, num_devices;
    size_t memory = 0;

    if (clGetPlatformIDs(1, &platform, &num_platforms) != CL_SUCCESS) {
        return 0;
    }

    cl_device_id* devices = (cl_device_id*)malloc(sizeof(cl_device_id) * 16);
    if (clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 16, devices, &num_devices) != CL_SUCCESS) {
        free(devices);
        return 0;
    }

    if ((cl_uint)device_id >= num_devices) {
        device_id = 0;
    }

    clGetDeviceInfo(devices[device_id], CL_DEVICE_GLOBAL_MEM_SIZE, sizeof(memory), &memory, NULL);
    free(devices);

    return memory;
}

int opencl_is_available(void) {
    return opencl_get_device_count() > 0 ? 1 : 0;
}

#else /* !HAVE_OPENCL */

/* Stub implementations when OpenCL is not available */
#include "opencl_fermat.h"

int opencl_fermat_init(int device_id) {
    (void)device_id;
    return -2;
}

void opencl_fermat_cleanup(void) {}

int opencl_fermat_batch(uint8_t *h_results, const uint32_t *h_primes,
                        uint32_t count, int bits) {
    (void)h_results; (void)h_primes; (void)count; (void)bits;
    return -1;
}

int opencl_get_device_count(void) { return 0; }
const char* opencl_get_device_name(int device_id) { (void)device_id; return "N/A"; }
size_t opencl_get_device_memory(int device_id) { (void)device_id; return 0; }
int opencl_is_available(void) { return 0; }

#endif /* HAVE_OPENCL */
