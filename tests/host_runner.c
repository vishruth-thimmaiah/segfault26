/**
 * host_runner.c — Minimal OpenCL host program for testing debugger attach and breakpoints.
 *
 * Owner: Person F
 *
 * Usage:
 *   ./test_host_runner [path_to_kernel.cl]
 */

#include <stdio.h>
#include <stdlib.h>
#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>

int main(int argc, char **argv) {
    const char *kernel_file = (argc > 1) ? argv[1] : "tests/kernels/hello_kernel.cl";
    printf("[host_runner] Loading kernel from %s\n", kernel_file);

    cl_platform_id platform = NULL;
    cl_uint num_platforms = 0;
    cl_int err = clGetPlatformIDs(1, &platform, &num_platforms);
    if (err != CL_SUCCESS || num_platforms == 0) {
        fprintf(stderr, "[host_runner] Failed to find OpenCL platform (err=%d)\n", err);
        return 1;
    }

    char plat_name[128];
    clGetPlatformInfo(platform, CL_PLATFORM_NAME, sizeof(plat_name), plat_name, NULL);
    printf("[host_runner] Using platform: %s\n", plat_name);

    cl_device_id device = NULL;
    cl_uint num_devices = 0;
    err = clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 1, &device, &num_devices);
    if (err != CL_SUCCESS || num_devices == 0) {
        fprintf(stderr, "[host_runner] Failed to find OpenCL device (err=%d)\n", err);
        return 1;
    }

    char dev_name[128];
    clGetDeviceInfo(device, CL_DEVICE_NAME, sizeof(dev_name), dev_name, NULL);
    printf("[host_runner] Using device: %s\n", dev_name);

    cl_context context = clCreateContext(NULL, 1, &device, NULL, NULL, &err);
    if (!context) {
        fprintf(stderr, "[host_runner] Failed to create context (err=%d)\n", err);
        return 1;
    }

    printf("[host_runner] OpenCL initialized successfully. Ready for debugger attachment.\n");
    clReleaseContext(context);
    return 0;
}
