// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * Dynamic OpenCL Loader
 *
 * Loads OpenCL.dll / libOpenCL.so at runtime via LoadLibrary/dlopen.
 * No OpenCL SDK required at build time. Pre-compiled binaries work
 * on any system with GPU drivers installed.
 *
 * Pattern inspired by CLEW (OpenCL Extension Wrangler) and XMRig's OclLib.
 */

#ifndef FREYCOIN_GPU_OPENCL_LOADER_H
#define FREYCOIN_GPU_OPENCL_LOADER_H

#include <cstddef>
#include <cstdint>

// ============================================================================
// OpenCL Type Definitions (from Khronos cl.h / cl_platform.h)
// Defined inline so we never need the OpenCL SDK headers.
// ============================================================================

typedef int32_t  cl_int;
typedef uint32_t cl_uint;
typedef uint64_t cl_ulong;
typedef cl_uint  cl_bool;
typedef cl_ulong cl_bitfield;
typedef cl_bitfield cl_device_type;
typedef cl_uint  cl_device_info;
typedef cl_bitfield cl_mem_flags;
typedef cl_uint  cl_program_build_info;
typedef cl_bitfield cl_command_queue_properties;
typedef intptr_t cl_context_properties;

// Opaque handles
typedef struct _cl_platform_id*    cl_platform_id;
typedef struct _cl_device_id*      cl_device_id;
typedef struct _cl_context*        cl_context;
typedef struct _cl_command_queue*   cl_command_queue;
typedef struct _cl_program*        cl_program;
typedef struct _cl_kernel*         cl_kernel;
typedef struct _cl_mem*            cl_mem;
typedef struct _cl_event*          cl_event;

// ============================================================================
// OpenCL Constants
// ============================================================================

#define CL_SUCCESS                  0
#define CL_DEVICE_TYPE_GPU          (1 << 2)
#define CL_DEVICE_NAME              0x102B
#define CL_DEVICE_GLOBAL_MEM_SIZE   0x101F
#define CL_MEM_READ_ONLY            (1 << 2)
#define CL_MEM_WRITE_ONLY           (1 << 1)
#define CL_MEM_COPY_HOST_PTR        (1 << 5)
#define CL_PROGRAM_BUILD_LOG        0x1183
#define CL_TRUE                     1
#define CL_FALSE                    0

// ============================================================================
// OpenCL Function Pointer Typedefs
// ============================================================================

typedef cl_int (*pfn_clGetPlatformIDs)(
    cl_uint num_entries, cl_platform_id* platforms, cl_uint* num_platforms);

typedef cl_int (*pfn_clGetDeviceIDs)(
    cl_platform_id platform, cl_device_type device_type,
    cl_uint num_entries, cl_device_id* devices, cl_uint* num_devices);

typedef cl_int (*pfn_clGetDeviceInfo)(
    cl_device_id device, cl_device_info param_name,
    size_t param_value_size, void* param_value, size_t* param_value_size_ret);

typedef cl_context (*pfn_clCreateContext)(
    const cl_context_properties* properties, cl_uint num_devices,
    const cl_device_id* devices,
    void (*pfn_notify)(const char*, const void*, size_t, void*),
    void* user_data, cl_int* errcode_ret);

typedef cl_command_queue (*pfn_clCreateCommandQueue)(
    cl_context context, cl_device_id device,
    cl_command_queue_properties properties, cl_int* errcode_ret);

typedef cl_program (*pfn_clCreateProgramWithSource)(
    cl_context context, cl_uint count, const char** strings,
    const size_t* lengths, cl_int* errcode_ret);

typedef cl_int (*pfn_clBuildProgram)(
    cl_program program, cl_uint num_devices, const cl_device_id* device_list,
    const char* options,
    void (*pfn_notify)(cl_program, void*), void* user_data);

typedef cl_int (*pfn_clGetProgramBuildInfo)(
    cl_program program, cl_device_id device,
    cl_program_build_info param_name,
    size_t param_value_size, void* param_value, size_t* param_value_size_ret);

typedef cl_kernel (*pfn_clCreateKernel)(
    cl_program program, const char* kernel_name, cl_int* errcode_ret);

typedef cl_int (*pfn_clSetKernelArg)(
    cl_kernel kernel, cl_uint arg_index, size_t arg_size, const void* arg_value);

typedef cl_int (*pfn_clEnqueueNDRangeKernel)(
    cl_command_queue command_queue, cl_kernel kernel,
    cl_uint work_dim, const size_t* global_work_offset,
    const size_t* global_work_size, const size_t* local_work_size,
    cl_uint num_events_in_wait_list, const cl_event* event_wait_list,
    cl_event* event);

typedef cl_int (*pfn_clEnqueueReadBuffer)(
    cl_command_queue command_queue, cl_mem buffer, cl_bool blocking_read,
    size_t offset, size_t size, void* ptr,
    cl_uint num_events_in_wait_list, const cl_event* event_wait_list,
    cl_event* event);

typedef cl_mem (*pfn_clCreateBuffer)(
    cl_context context, cl_mem_flags flags, size_t size,
    void* host_ptr, cl_int* errcode_ret);

typedef cl_int (*pfn_clReleaseMemObject)(cl_mem memobj);
typedef cl_int (*pfn_clReleaseKernel)(cl_kernel kernel);
typedef cl_int (*pfn_clReleaseProgram)(cl_program program);
typedef cl_int (*pfn_clReleaseCommandQueue)(cl_command_queue command_queue);
typedef cl_int (*pfn_clReleaseContext)(cl_context context);

// ============================================================================
// Function Pointer Globals (defined in opencl_loader.cpp)
// ============================================================================

extern pfn_clGetPlatformIDs          ocl_clGetPlatformIDs;
extern pfn_clGetDeviceIDs            ocl_clGetDeviceIDs;
extern pfn_clGetDeviceInfo           ocl_clGetDeviceInfo;
extern pfn_clCreateContext           ocl_clCreateContext;
extern pfn_clCreateCommandQueue      ocl_clCreateCommandQueue;
extern pfn_clCreateProgramWithSource ocl_clCreateProgramWithSource;
extern pfn_clBuildProgram            ocl_clBuildProgram;
extern pfn_clGetProgramBuildInfo     ocl_clGetProgramBuildInfo;
extern pfn_clCreateKernel            ocl_clCreateKernel;
extern pfn_clSetKernelArg            ocl_clSetKernelArg;
extern pfn_clEnqueueNDRangeKernel    ocl_clEnqueueNDRangeKernel;
extern pfn_clEnqueueReadBuffer       ocl_clEnqueueReadBuffer;
extern pfn_clCreateBuffer            ocl_clCreateBuffer;
extern pfn_clReleaseMemObject        ocl_clReleaseMemObject;
extern pfn_clReleaseKernel           ocl_clReleaseKernel;
extern pfn_clReleaseProgram          ocl_clReleaseProgram;
extern pfn_clReleaseCommandQueue     ocl_clReleaseCommandQueue;
extern pfn_clReleaseContext          ocl_clReleaseContext;

// ============================================================================
// Loader API
// ============================================================================

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Attempt to load OpenCL library at runtime.
 * @return 0 on success, -1 if library not found, -2 if symbols missing
 */
int opencl_load(void);

/**
 * Unload OpenCL library and clear function pointers.
 */
void opencl_unload(void);

/**
 * Check if OpenCL was successfully loaded.
 * @return 1 if loaded, 0 if not
 */
int opencl_is_loaded(void);

#ifdef __cplusplus
}
#endif

#endif // FREYCOIN_GPU_OPENCL_LOADER_H
