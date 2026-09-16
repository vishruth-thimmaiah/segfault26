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
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long len = ftell(f);
    if (len < 0) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    char *buf = (char *)calloc((size_t)len + 1, sizeof(char));
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t read_bytes = fread(buf, 1, (size_t)len, f);
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
    if (!platforms) {
        return NULL;
    }
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
    free((void *)platforms);
    return selected;
}

static cl_device_id find_cpu_or_any_device(cl_platform_id platform) {
    cl_device_id device = NULL;
    cl_uint num_devices = 0;
    cl_int err = clGetDeviceIDs(platform, CL_DEVICE_TYPE_CPU, 1, &device, &num_devices);
    if (err != CL_SUCCESS || num_devices == 0) {
        clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 1, &device, &num_devices);
    }
    return device;
}

static const char *determine_kernel_name(const char *kernel_file, const char *arg_name) {
    if (arg_name) {
        return arg_name;
    }
    if (strstr(kernel_file, "reduction")) {
        return "reduce_sum";
    }
    if (strstr(kernel_file, "matrix")) {
        return "matmul";
    }
    return "vec_add";
}

static cl_program build_kernel_program(cl_context context, cl_device_id device,
                                       const char *kernel_src, size_t src_size) {
    cl_int err = CL_SUCCESS;
    const char *sources[] = {kernel_src};
    const size_t lengths[] = {src_size};
    cl_program program = clCreateProgramWithSource(context, 1, sources, lengths, &err);
    if (!program || err != CL_SUCCESS) {
        fprintf(stderr, "[host_runner] Failed to create program (err=%d)\n", err);
        return NULL;
    }

    // Oclgrind always emits debug info and rejects -g, so the flags are
    // overridable rather than fixed.
    const char *build_options = getenv("OCLDBG_BUILD_OPTIONS");
    if (!build_options) {
        build_options = "-g -cl-opt-disable";
    }
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
        return NULL;
    }
    printf("[host_runner] Program built successfully with flags: %s\n", build_options);
    return program;
}

static int run_vec_add(cl_context context, cl_command_queue queue, cl_kernel kernel) {
    const size_t n = 64;
    float h_a[64];
    float h_b[64];
    float h_c[64];

    for (size_t i = 0; i < n; ++i) {
        h_a[i] = (float)i;
        h_b[i] = (float)(i * 2);
        h_c[i] = 0.0F;
    }

    cl_int err = CL_SUCCESS;
    cl_mem d_a = clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, sizeof(float) * n,
                                (void *)h_a, &err);
    cl_mem d_b = clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, sizeof(float) * n,
                                (void *)h_b, &err);
    cl_mem d_c = clCreateBuffer(context, CL_MEM_WRITE_ONLY, sizeof(float) * n, NULL, &err);

    clSetKernelArg(kernel, 0, sizeof(cl_mem), (const void *)&d_a);
    clSetKernelArg(kernel, 1, sizeof(cl_mem), (const void *)&d_b);
    clSetKernelArg(kernel, 2, sizeof(cl_mem), (const void *)&d_c);

    size_t global_size = n;
    size_t local_size = 16;
    printf("[host_runner] Enqueueing kernel vec_add (global=%zu, local=%zu)...\n", global_size,
           local_size);
    err = clEnqueueNDRangeKernel(queue, kernel, 1, NULL, &global_size, &local_size, 0, NULL, NULL);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "[host_runner] Failed to enqueue kernel (err=%d)\n", err);
    }

    clFinish(queue);
    clEnqueueReadBuffer(queue, d_c, CL_TRUE, 0, sizeof(float) * n, (void *)h_c, 0, NULL, NULL);
    printf("[host_runner] Result sample: c[0]=%.1f, c[1]=%.1f, c[63]=%.1f\n", h_c[0], h_c[1],
           h_c[63]);

    clReleaseMemObject(d_a);
    clReleaseMemObject(d_b);
    clReleaseMemObject(d_c);
    return (err == CL_SUCCESS) ? 0 : 1;
}

static int run_reduce_sum(cl_context context, cl_command_queue queue, cl_kernel kernel) {
    const int n_items = 4;
    const size_t num_wi = 16;
    const size_t total_floats = (num_wi * (size_t)n_items) + 16;
    float *h_src = (float *)malloc(sizeof(float) * total_floats);
    if (!h_src) {
        return 1;
    }
    float h_dst[16];
    for (size_t i = 0; i < 16; ++i) {
        h_dst[i] = 0.0F;
    }
    for (size_t i = 0; i < total_floats; ++i) {
        h_src[i] = 1.0F;
    }

    cl_int err = CL_SUCCESS;
    cl_mem d_src = clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                  sizeof(float) * total_floats, (void *)h_src, &err);
    cl_mem d_dst = clCreateBuffer(context, CL_MEM_WRITE_ONLY, sizeof(float) * num_wi, NULL, &err);
    cl_int n_arg = n_items;

    clSetKernelArg(kernel, 0, sizeof(cl_mem), (const void *)&d_src);
    clSetKernelArg(kernel, 1, sizeof(cl_mem), (const void *)&d_dst);
    clSetKernelArg(kernel, 2, sizeof(cl_int), (const void *)&n_arg);

    size_t global_size = num_wi;
    size_t local_size = 4;
    printf("[host_runner] Enqueueing kernel reduce_sum (global=%zu, local=%zu)...\n", global_size,
           local_size);
    err = clEnqueueNDRangeKernel(queue, kernel, 1, NULL, &global_size, &local_size, 0, NULL, NULL);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "[host_runner] Failed to enqueue kernel (err=%d)\n", err);
    }

    clFinish(queue);
    clEnqueueReadBuffer(queue, d_dst, CL_TRUE, 0, sizeof(float) * num_wi, (void *)h_dst, 0, NULL,
                        NULL);
    printf("[host_runner] Result sample: dst[0]=%.1f, dst[3]=%.1f\n", h_dst[0], h_dst[3]);

    clReleaseMemObject(d_src);
    clReleaseMemObject(d_dst);
    free(h_src);
    return (err == CL_SUCCESS) ? 0 : 1;
}

static int run_addr_spaces(cl_context context, cl_command_queue queue, cl_kernel kernel) {
    const size_t num_wi = 16;
    const size_t local_size = 4;
    float h_src[16];
    float h_table[4];
    float h_dst[16];
    for (size_t i = 0; i < num_wi; ++i) {
        h_src[i] = (float)i;
        h_dst[i] = 0.0F;
    }
    for (size_t i = 0; i < 4; ++i) {
        h_table[i] = 2.0F;
    }

    cl_int err = CL_SUCCESS;
    cl_mem d_src = clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, sizeof(h_src),
                                  (void *)h_src, &err);
    cl_mem d_table = clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                    sizeof(h_table), (void *)h_table, &err);
    cl_mem d_dst = clCreateBuffer(context, CL_MEM_WRITE_ONLY, sizeof(h_dst), NULL, &err);

    clSetKernelArg(kernel, 0, sizeof(cl_mem), (const void *)&d_src);
    clSetKernelArg(kernel, 1, sizeof(cl_mem), (const void *)&d_table);
    // A __local argument is sized by the host and has no buffer behind it.
    clSetKernelArg(kernel, 2, sizeof(float) * local_size, NULL);
    clSetKernelArg(kernel, 3, sizeof(cl_mem), (const void *)&d_dst);

    size_t global_size = num_wi;
    printf("[host_runner] Enqueueing kernel addr_spaces (global=%zu, local=%zu)...\n", global_size,
           local_size);
    err = clEnqueueNDRangeKernel(queue, kernel, 1, NULL, &global_size, &local_size, 0, NULL, NULL);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "[host_runner] Failed to enqueue kernel (err=%d)\n", err);
    }

    clFinish(queue);
    clEnqueueReadBuffer(queue, d_dst, CL_TRUE, 0, sizeof(h_dst), (void *)h_dst, 0, NULL, NULL);
    printf("[host_runner] Result sample: dst[0]=%.1f, dst[3]=%.1f\n", h_dst[0], h_dst[3]);

    clReleaseMemObject(d_src);
    clReleaseMemObject(d_table);
    clReleaseMemObject(d_dst);
    return (err == CL_SUCCESS) ? 0 : 1;
}

static int run_volume_acc(cl_context context, cl_command_queue queue, cl_kernel kernel) {
    const size_t gx = 8;
    const size_t gy = 8;
    const size_t gz = 4;
    const size_t total_elements = gx * gy * gz;
    float *h_src = (float *)malloc(sizeof(float) * total_elements);
    float *h_dst = (float *)malloc(sizeof(float) * total_elements);
    if (!h_src || !h_dst) {
        free(h_src);
        free(h_dst);
        return 1;
    }

    for (size_t i = 0; i < total_elements; ++i) {
        h_src[i] = 1.0F;
        h_dst[i] = 0.0F;
    }

    cl_int err = CL_SUCCESS;
    cl_mem d_src = clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                  sizeof(float) * total_elements, (void *)h_src, &err);
    cl_mem d_dst =
        clCreateBuffer(context, CL_MEM_WRITE_ONLY, sizeof(float) * total_elements, NULL, &err);

    clSetKernelArg(kernel, 0, sizeof(cl_mem), (const void *)&d_src);
    clSetKernelArg(kernel, 1, sizeof(cl_mem), (const void *)&d_dst);

    size_t global_size[3] = {gx, gy, gz};
    size_t local_size[3] = {2, 2, 2};
    printf("[host_runner] Enqueueing kernel volume_acc (global=[%zu,%zu,%zu], "
           "local=[%zu,%zu,%zu])...\n",
           global_size[0], global_size[1], global_size[2], local_size[0], local_size[1],
           local_size[2]);
    err = clEnqueueNDRangeKernel(queue, kernel, 3, NULL, global_size, local_size, 0, NULL, NULL);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "[host_runner] Failed to enqueue kernel (err=%d)\n", err);
    }

    clFinish(queue);
    clEnqueueReadBuffer(queue, d_dst, CL_TRUE, 0, sizeof(float) * total_elements, (void *)h_dst, 0,
                        NULL, NULL);
    printf("[host_runner] Result sample: dst[0]=%.1f, dst[255]=%.1f\n", h_dst[0],
           h_dst[total_elements - 1]);

    clReleaseMemObject(d_src);
    clReleaseMemObject(d_dst);
    free(h_src);
    free(h_dst);
    return (err == CL_SUCCESS) ? 0 : 1;
}

int main(int argc, char **argv) {
    const char *kernel_file = (argc > 1) ? argv[1] : "tests/kernels/hello_kernel.cl";
    const char *arg_kernel_name = (argc > 2) ? argv[2] : NULL;
    const char *kernel_name = determine_kernel_name(kernel_file, arg_kernel_name);

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

    cl_device_id device = find_cpu_or_any_device(platform);
    if (!device) {
        fprintf(stderr, "[host_runner] Failed to find OpenCL device\n");
        free(kernel_src);
        return 1;
    }

    char dev_name[128];
    clGetDeviceInfo(device, CL_DEVICE_NAME, sizeof(dev_name), dev_name, NULL);
    printf("[host_runner] Using device: %s\n", dev_name);

    cl_int err = CL_SUCCESS;
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

    cl_program program = build_kernel_program(context, device, kernel_src, src_size);
    if (!program) {
        clReleaseCommandQueue(queue);
        clReleaseContext(context);
        free(kernel_src);
        return 1;
    }

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
        run_vec_add(context, queue, kernel);
    } else if (strcmp(kernel_name, "reduce_sum") == 0) {
        run_reduce_sum(context, queue, kernel);
    } else if (strcmp(kernel_name, "volume_acc") == 0) {
        run_volume_acc(context, queue, kernel);
    } else if (strcmp(kernel_name, "addr_spaces") == 0) {
        run_addr_spaces(context, queue, kernel);
    }

    clReleaseKernel(kernel);
    clReleaseProgram(program);
    clReleaseCommandQueue(queue);
    clReleaseContext(context);
    free(kernel_src);

    printf("[host_runner] Kernel execution finished successfully.\n");
    return 0;
}
