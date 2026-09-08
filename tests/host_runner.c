/**
 * host_runner.c — Minimal OpenCL host program for testing debugger attach and breakpoints.
 *
 * Owner: Person F
 *
 * Usage:
 *   ./test_host_runner [path_to_kernel.cl] [kernel_name]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>

static char *read_file(const char *filename, size_t *out_size) {
    FILE *f = fopen(filename, "rb");
    if (!f) {
        perror("[host_runner] fopen");
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) {
        fclose(f);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t read_bytes = fread(buf, 1, (size_t)len, f);
    buf[read_bytes] = '\0';
    fclose(f);
    if (out_size) {
        *out_size = read_bytes;
    }
    return buf;
}

static cl_platform_id find_pocl_or_default_platform(void) {
    cl_uint num_platforms = 0;
    cl_int err = clGetPlatformIDs(0, NULL, &num_platforms);
    if (err != CL_SUCCESS || num_platforms == 0) {
        return NULL;
    }

    cl_platform_id *platforms = (cl_platform_id *)malloc(sizeof(cl_platform_id) * num_platforms);
    clGetPlatformIDs(num_platforms, platforms, NULL);

    cl_platform_id selected = platforms[0];
    for (cl_uint i = 0; i < num_platforms; ++i) {
        char name[128];
        clGetPlatformInfo(platforms[i], CL_PLATFORM_NAME, sizeof(name), name, NULL);
        if (strstr(name, "Portable") || strstr(name, "pocl") || strstr(name, "POCL")) {
            selected = platforms[i];
            break;
        }
    }
    free(platforms);
    return selected;
}

int main(int argc, char **argv) {
    const char *kernel_file = (argc > 1) ? argv[1] : "tests/kernels/hello_kernel.cl";
    const char *kernel_name = (argc > 2) ? argv[2] : NULL;

    if (!kernel_name) {
        if (strstr(kernel_file, "reduction")) {
            kernel_name = "reduce_sum";
        } else if (strstr(kernel_file, "matrix")) {
            kernel_name = "matmul";
        } else {
            kernel_name = "vec_add";
        }
    }

    printf("[host_runner] Loading kernel file: %s (kernel name: %s)\n", kernel_file, kernel_name);
    size_t src_size = 0;
    char *kernel_src = read_file(kernel_file, &src_size);
    if (!kernel_src) {
        fprintf(stderr, "[host_runner] Could not read file %s\n", kernel_file);
        return 1;
    }

    cl_platform_id platform = find_pocl_or_default_platform();
    if (!platform) {
        fprintf(stderr, "[host_runner] Failed to find OpenCL platform\n");
        free(kernel_src);
        return 1;
    }

    char plat_name[128];
    clGetPlatformInfo(platform, CL_PLATFORM_NAME, sizeof(plat_name), plat_name, NULL);
    printf("[host_runner] Using platform: %s\n", plat_name);

    cl_device_id device = NULL;
    cl_uint num_devices = 0;
    cl_int err = clGetDeviceIDs(platform, CL_DEVICE_TYPE_CPU, 1, &device, &num_devices);
    if (err != CL_SUCCESS || num_devices == 0) {
        err = clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 1, &device, &num_devices);
        if (err != CL_SUCCESS || num_devices == 0) {
            fprintf(stderr, "[host_runner] Failed to find OpenCL device (err=%d)\n", err);
            free(kernel_src);
            return 1;
        }
    }

    char dev_name[128];
    clGetDeviceInfo(device, CL_DEVICE_NAME, sizeof(dev_name), dev_name, NULL);
    printf("[host_runner] Using device: %s\n", dev_name);

    cl_context context = clCreateContext(NULL, 1, &device, NULL, NULL, &err);
    if (!context || err != CL_SUCCESS) {
        fprintf(stderr, "[host_runner] Failed to create context (err=%d)\n", err);
        free(kernel_src);
        return 1;
    }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    cl_command_queue queue = clCreateCommandQueue(context, device, 0, &err);
#pragma GCC diagnostic pop
    if (!queue || err != CL_SUCCESS) {
        fprintf(stderr, "[host_runner] Failed to create command queue (err=%d)\n", err);
        clReleaseContext(context);
        free(kernel_src);
        return 1;
    }

    const char *sources[] = {kernel_src};
    const size_t lengths[] = {src_size};
    cl_program program = clCreateProgramWithSource(context, 1, sources, lengths, &err);
    if (!program || err != CL_SUCCESS) {
        fprintf(stderr, "[host_runner] Failed to create program (err=%d)\n", err);
        clReleaseCommandQueue(queue);
        clReleaseContext(context);
        free(kernel_src);
        return 1;
    }

    /* Compile with debug flags to preserve DWARF info and disable optimizations */
    const char *build_options = "-g -cl-opt-disable";
    err = clBuildProgram(program, 1, &device, build_options, NULL, NULL);
    if (err != CL_SUCCESS) {
        size_t log_size = 0;
        clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, 0, NULL, &log_size);
        char *log = (char *)malloc(log_size + 1);
        if (log) {
            clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, log_size, log, NULL);
            log[log_size] = '\0';
            fprintf(stderr, "[host_runner] Build failed:\n%s\n", log);
            free(log);
        }
        clReleaseProgram(program);
        clReleaseCommandQueue(queue);
        clReleaseContext(context);
        free(kernel_src);
        return 1;
    }
    printf("[host_runner] Program built successfully with flags: %s\n", build_options);

    cl_kernel kernel = clCreateKernel(program, kernel_name, &err);
    if (!kernel || err != CL_SUCCESS) {
        fprintf(stderr, "[host_runner] Failed to create kernel '%s' (err=%d)\n", kernel_name, err);
        clReleaseProgram(program);
        clReleaseCommandQueue(queue);
        clReleaseContext(context);
        free(kernel_src);
        return 1;
    }

    if (strcmp(kernel_name, "vec_add") == 0) {
        const size_t n = 64;
        float h_a[64], h_b[64], h_c[64];
        for (size_t i = 0; i < n; ++i) {
            h_a[i] = (float)i;
            h_b[i] = (float)(i * 2);
            h_c[i] = 0.0f;
        }

        cl_mem d_a = clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                    sizeof(float) * n, h_a, &err);
        cl_mem d_b = clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                    sizeof(float) * n, h_b, &err);
        cl_mem d_c = clCreateBuffer(context, CL_MEM_WRITE_ONLY, sizeof(float) * n, NULL, &err);

        clSetKernelArg(kernel, 0, sizeof(cl_mem), &d_a);
        clSetKernelArg(kernel, 1, sizeof(cl_mem), &d_b);
        clSetKernelArg(kernel, 2, sizeof(cl_mem), &d_c);

        size_t global_size = n;
        size_t local_size = 16;
        printf("[host_runner] Enqueueing kernel %s (global=%zu, local=%zu)...\n", kernel_name,
               global_size, local_size);
        err = clEnqueueNDRangeKernel(queue, kernel, 1, NULL, &global_size, &local_size, 0, NULL,
                                     NULL);
        if (err != CL_SUCCESS) {
            fprintf(stderr, "[host_runner] Failed to enqueue kernel (err=%d)\n", err);
        }

        clFinish(queue);
        clEnqueueReadBuffer(queue, d_c, CL_TRUE, 0, sizeof(float) * n, h_c, 0, NULL, NULL);
        printf("[host_runner] Result sample: c[0]=%.1f, c[1]=%.1f, c[63]=%.1f\n", h_c[0], h_c[1],
               h_c[63]);

        clReleaseMemObject(d_a);
        clReleaseMemObject(d_b);
        clReleaseMemObject(d_c);
    } else if (strcmp(kernel_name, "reduce_sum") == 0) {
        const int n_items = 4;
        const size_t num_wi = 16;
        const size_t total_floats = num_wi * (size_t)n_items + 16;
        float *h_src = (float *)malloc(sizeof(float) * total_floats);
        float h_dst[16] = {0};
        for (size_t i = 0; i < total_floats; ++i) {
            h_src[i] = 1.0f;
        }

        cl_mem d_src = clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                      sizeof(float) * total_floats, h_src, &err);
        cl_mem d_dst =
            clCreateBuffer(context, CL_MEM_WRITE_ONLY, sizeof(float) * num_wi, NULL, &err);
        cl_int n_arg = n_items;

        clSetKernelArg(kernel, 0, sizeof(cl_mem), &d_src);
        clSetKernelArg(kernel, 1, sizeof(cl_mem), &d_dst);
        clSetKernelArg(kernel, 2, sizeof(cl_int), &n_arg);

        size_t global_size = num_wi;
        size_t local_size = 4;
        printf("[host_runner] Enqueueing kernel %s (global=%zu, local=%zu)...\n", kernel_name,
               global_size, local_size);
        err = clEnqueueNDRangeKernel(queue, kernel, 1, NULL, &global_size, &local_size, 0, NULL,
                                     NULL);
        if (err != CL_SUCCESS) {
            fprintf(stderr, "[host_runner] Failed to enqueue kernel (err=%d)\n", err);
        }

        clFinish(queue);
        clEnqueueReadBuffer(queue, d_dst, CL_TRUE, 0, sizeof(float) * num_wi, h_dst, 0, NULL, NULL);
        printf("[host_runner] Result sample: dst[0]=%.1f, dst[3]=%.1f\n", h_dst[0], h_dst[3]);

        clReleaseMemObject(d_src);
        clReleaseMemObject(d_dst);
        free(h_src);
    }

    clReleaseKernel(kernel);
    clReleaseProgram(program);
    clReleaseCommandQueue(queue);
    clReleaseContext(context);
    free(kernel_src);

    printf("[host_runner] Kernel execution finished successfully.\n");
    return 0;
}
