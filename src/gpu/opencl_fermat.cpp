// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * OpenCL Fermat Primality Test Implementation
 *
 * In memory of Jonnie Frey (1989-2017), creator of Gapcoin.
 *
 * Uses dynamic OpenCL loading — no SDK required at build time.
 * OpenCL.dll / libOpenCL.so is loaded at runtime if available.
 *
 * Supports multi-GPU: each device gets its own context, queue, and kernels.
 * The legacy single-device API (opencl_fermat_init/batch/cleanup) operates
 * on device 0 for backward compatibility.
 */

#include "opencl_fermat.h"
#include "opencl_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_GPU_DEVICES 16

/* Per-device OpenCL state */
struct OCLDeviceState {
    cl_platform_id platform;
    cl_device_id device;
    cl_context context;
    cl_command_queue queue;
    cl_program program;
    cl_kernel kernel_320;
    cl_kernel kernel_352;
    int initialized;
    char device_name[256];
    size_t device_memory;
};

static OCLDeviceState g_devices[MAX_GPU_DEVICES] = {};
static int g_num_initialized = 0;

/* Embedded kernel source (generated from fermat.cl) */
static const char* kernel_source =
#include "fermat_cl_source.h"
;

/* Alternative: Load kernel from file at runtime */
static char* load_kernel_file(const char* filename, size_t* size) {
    FILE* f = fopen(filename, "rb");
    if (!f) return nullptr;

    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* source = (char*)malloc(*size + 1);
    if (!source) {
        fclose(f);
        return nullptr;
    }

    fread(source, 1, *size, f);
    source[*size] = '\0';
    fclose(f);

    return source;
}

/* Initialize a single device. Thread-safe for different device_ids. */
static int init_single_device(int device_id) {
    cl_int err;
    cl_uint num_platforms, num_devices;
    OCLDeviceState* dev = &g_devices[device_id];

    if (dev->initialized) return 0;

    /* Ensure OpenCL is dynamically loaded */
    if (!opencl_is_loaded()) {
        if (opencl_load() != 0) {
            return -2;
        }
    }

    /* Get platform */
    err = ocl_clGetPlatformIDs(1, &dev->platform, &num_platforms);
    if (err != CL_SUCCESS || num_platforms == 0) {
        return -2;
    }

    /* Get device */
    err = ocl_clGetDeviceIDs(dev->platform, CL_DEVICE_TYPE_GPU, 0, nullptr, &num_devices);
    if (err != CL_SUCCESS || num_devices == 0) {
        return -2;
    }

    cl_device_id* devices = (cl_device_id*)malloc(sizeof(cl_device_id) * num_devices);
    err = ocl_clGetDeviceIDs(dev->platform, CL_DEVICE_TYPE_GPU, num_devices, devices, nullptr);
    if (err != CL_SUCCESS) {
        free(devices);
        return -1;
    }

    if ((cl_uint)device_id >= num_devices) {
        free(devices);
        return -2;  /* Device index out of range */
    }
    dev->device = devices[device_id];
    free(devices);

    /* Get device info */
    ocl_clGetDeviceInfo(dev->device, CL_DEVICE_NAME, sizeof(dev->device_name), dev->device_name, nullptr);
    ocl_clGetDeviceInfo(dev->device, CL_DEVICE_GLOBAL_MEM_SIZE, sizeof(dev->device_memory), &dev->device_memory, nullptr);

    /* Create context */
    dev->context = ocl_clCreateContext(nullptr, 1, &dev->device, nullptr, nullptr, &err);
    if (err != CL_SUCCESS) {
        return -1;
    }

    /* Create command queue */
    dev->queue = ocl_clCreateCommandQueue(dev->context, dev->device, 0, &err);
    if (err != CL_SUCCESS) {
        ocl_clReleaseContext(dev->context);
        return -1;
    }

    /* Try to load kernel from file first, fall back to embedded */
    size_t kernel_size;
    char* source = load_kernel_file("fermat.cl", &kernel_size);
    if (!source) {
        source = (char*)kernel_source;
        kernel_size = strlen(kernel_source);
    }

    /* Create program */
    dev->program = ocl_clCreateProgramWithSource(dev->context, 1, (const char**)&source, &kernel_size, &err);
    if (source != kernel_source) {
        free(source);
    }
    if (err != CL_SUCCESS) {
        ocl_clReleaseCommandQueue(dev->queue);
        ocl_clReleaseContext(dev->context);
        return -1;
    }

    /* Build program */
    err = ocl_clBuildProgram(dev->program, 1, &dev->device, "", nullptr, nullptr);
    if (err != CL_SUCCESS) {
        size_t log_size;
        ocl_clGetProgramBuildInfo(dev->program, dev->device, CL_PROGRAM_BUILD_LOG, 0, nullptr, &log_size);
        char* log = (char*)malloc(log_size + 1);
        ocl_clGetProgramBuildInfo(dev->program, dev->device, CL_PROGRAM_BUILD_LOG, log_size, log, nullptr);
        fprintf(stderr, "OpenCL build error (device %d):\n%s\n", device_id, log);
        free(log);

        ocl_clReleaseProgram(dev->program);
        ocl_clReleaseCommandQueue(dev->queue);
        ocl_clReleaseContext(dev->context);
        return -1;
    }

    /* Create kernels */
    dev->kernel_320 = ocl_clCreateKernel(dev->program, "fermat_kernel_320", &err);
    if (err != CL_SUCCESS) {
        ocl_clReleaseProgram(dev->program);
        ocl_clReleaseCommandQueue(dev->queue);
        ocl_clReleaseContext(dev->context);
        return -1;
    }

    dev->kernel_352 = ocl_clCreateKernel(dev->program, "fermat_kernel_352", &err);
    if (err != CL_SUCCESS) {
        ocl_clReleaseKernel(dev->kernel_320);
        ocl_clReleaseProgram(dev->program);
        ocl_clReleaseCommandQueue(dev->queue);
        ocl_clReleaseContext(dev->context);
        return -1;
    }

    dev->initialized = 1;
    return 0;
}

static void cleanup_single_device(int device_id) {
    if (device_id < 0 || device_id >= MAX_GPU_DEVICES) return;
    OCLDeviceState* dev = &g_devices[device_id];
    if (!dev->initialized) return;

    if (dev->kernel_320) ocl_clReleaseKernel(dev->kernel_320);
    if (dev->kernel_352) ocl_clReleaseKernel(dev->kernel_352);
    if (dev->program) ocl_clReleaseProgram(dev->program);
    if (dev->queue) ocl_clReleaseCommandQueue(dev->queue);
    if (dev->context) ocl_clReleaseContext(dev->context);

    memset(dev, 0, sizeof(*dev));
}

// ============================================================================
// Legacy single-device API (backward compatible, operates on device 0)
// ============================================================================

int opencl_fermat_init(int device_id) {
    int rc = init_single_device(device_id);
    if (rc == 0 && device_id == 0) {
        g_num_initialized = 1;
    }
    return rc;
}

void opencl_fermat_cleanup(void) {
    cleanup_single_device(0);
    g_num_initialized = 0;
}

int opencl_fermat_batch(uint8_t *h_results, const uint32_t *h_primes,
                        uint32_t count, int bits) {
    return opencl_fermat_batch_device(0, h_results, h_primes, count, bits);
}

// ============================================================================
// Multi-GPU API
// ============================================================================

int opencl_fermat_init_all(int* num_initialized) {
    int count = opencl_get_device_count();
    if (count <= 0) return -2;
    if (count > MAX_GPU_DEVICES) count = MAX_GPU_DEVICES;

    int ok = 0;
    for (int i = 0; i < count; i++) {
        if (init_single_device(i) == 0) {
            ok++;
        }
    }
    g_num_initialized = ok;
    if (num_initialized) *num_initialized = ok;
    return ok > 0 ? 0 : -1;
}

int opencl_fermat_init_device(int device_id) {
    if (device_id < 0 || device_id >= MAX_GPU_DEVICES) return -1;
    int rc = init_single_device(device_id);
    if (rc == 0) {
        /* Recount initialized devices */
        int n = 0;
        for (int i = 0; i < MAX_GPU_DEVICES; i++) {
            if (g_devices[i].initialized) n++;
        }
        g_num_initialized = n;
    }
    return rc;
}

void opencl_fermat_cleanup_device(int device_id) {
    cleanup_single_device(device_id);
    int n = 0;
    for (int i = 0; i < MAX_GPU_DEVICES; i++) {
        if (g_devices[i].initialized) n++;
    }
    g_num_initialized = n;
}

void opencl_fermat_cleanup_all(void) {
    for (int i = 0; i < MAX_GPU_DEVICES; i++) {
        cleanup_single_device(i);
    }
    g_num_initialized = 0;
}

int opencl_fermat_batch_device(int device_id, uint8_t *h_results,
                               const uint32_t *h_primes,
                               uint32_t count, int bits) {
    if (device_id < 0 || device_id >= MAX_GPU_DEVICES) return -1;
    OCLDeviceState* dev = &g_devices[device_id];
    if (!dev->initialized) return -1;
    if (count == 0) return 0;

    cl_int err;
    int limbs = (bits <= 320) ? 10 : 11;
    size_t primes_size = count * limbs * sizeof(uint32_t);
    size_t results_size = count * sizeof(uint8_t);

    /* Create buffers */
    cl_mem d_primes = ocl_clCreateBuffer(dev->context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                         primes_size, (void*)h_primes, &err);
    if (err != CL_SUCCESS) return -1;

    cl_mem d_results = ocl_clCreateBuffer(dev->context, CL_MEM_WRITE_ONLY,
                                          results_size, nullptr, &err);
    if (err != CL_SUCCESS) {
        ocl_clReleaseMemObject(d_primes);
        return -1;
    }

    /* Select kernel */
    cl_kernel kernel = (bits <= 320) ? dev->kernel_320 : dev->kernel_352;

    /* Set kernel arguments */
    ocl_clSetKernelArg(kernel, 0, sizeof(cl_mem), &d_results);
    ocl_clSetKernelArg(kernel, 1, sizeof(cl_mem), &d_primes);
    ocl_clSetKernelArg(kernel, 2, sizeof(uint32_t), &count);

    /* Execute kernel */
    size_t global_size = ((count + 63) / 64) * 64;
    size_t local_size = 64;
    err = ocl_clEnqueueNDRangeKernel(dev->queue, kernel, 1, nullptr, &global_size, &local_size, 0, nullptr, nullptr);
    if (err != CL_SUCCESS) {
        ocl_clReleaseMemObject(d_results);
        ocl_clReleaseMemObject(d_primes);
        return -1;
    }

    /* Read results */
    err = ocl_clEnqueueReadBuffer(dev->queue, d_results, CL_TRUE, 0, results_size, h_results, 0, nullptr, nullptr);
    if (err != CL_SUCCESS) {
        ocl_clReleaseMemObject(d_results);
        ocl_clReleaseMemObject(d_primes);
        return -1;
    }

    /* Cleanup */
    ocl_clReleaseMemObject(d_results);
    ocl_clReleaseMemObject(d_primes);

    return 0;
}

int opencl_get_num_initialized(void) {
    return g_num_initialized;
}

// ============================================================================
// Query functions (unchanged)
// ============================================================================

int opencl_get_device_count(void) {
    if (!opencl_is_loaded()) {
        if (opencl_load() != 0) return 0;
    }

    cl_platform_id platform;
    cl_uint num_platforms, num_devices;

    if (ocl_clGetPlatformIDs(1, &platform, &num_platforms) != CL_SUCCESS || num_platforms == 0) {
        return 0;
    }

    if (ocl_clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 0, nullptr, &num_devices) != CL_SUCCESS) {
        return 0;
    }

    return (int)num_devices;
}

const char* opencl_get_device_name(int device_id) {
    static char name[256];
    if (!opencl_is_loaded()) {
        if (opencl_load() != 0) return "N/A";
    }

    /* If device is initialized, return cached name */
    if (device_id >= 0 && device_id < MAX_GPU_DEVICES && g_devices[device_id].initialized) {
        return g_devices[device_id].device_name;
    }

    cl_platform_id platform;
    cl_uint num_platforms, num_devices;

    if (ocl_clGetPlatformIDs(1, &platform, &num_platforms) != CL_SUCCESS) {
        return "Unknown";
    }

    cl_device_id* devices = (cl_device_id*)malloc(sizeof(cl_device_id) * 16);
    if (ocl_clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 16, devices, &num_devices) != CL_SUCCESS) {
        free(devices);
        return "Unknown";
    }

    if ((cl_uint)device_id >= num_devices) {
        device_id = 0;
    }

    ocl_clGetDeviceInfo(devices[device_id], CL_DEVICE_NAME, sizeof(name), name, nullptr);
    free(devices);

    return name;
}

size_t opencl_get_device_memory(int device_id) {
    if (!opencl_is_loaded()) {
        if (opencl_load() != 0) return 0;
    }

    /* If device is initialized, return cached value */
    if (device_id >= 0 && device_id < MAX_GPU_DEVICES && g_devices[device_id].initialized) {
        return g_devices[device_id].device_memory;
    }

    cl_platform_id platform;
    cl_uint num_platforms, num_devices;
    size_t memory = 0;

    if (ocl_clGetPlatformIDs(1, &platform, &num_platforms) != CL_SUCCESS) {
        return 0;
    }

    cl_device_id* devices = (cl_device_id*)malloc(sizeof(cl_device_id) * 16);
    if (ocl_clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 16, devices, &num_devices) != CL_SUCCESS) {
        free(devices);
        return 0;
    }

    if ((cl_uint)device_id >= num_devices) {
        device_id = 0;
    }

    ocl_clGetDeviceInfo(devices[device_id], CL_DEVICE_GLOBAL_MEM_SIZE, sizeof(memory), &memory, nullptr);
    free(devices);

    return memory;
}

int opencl_is_available(void) {
    return opencl_get_device_count() > 0 ? 1 : 0;
}
