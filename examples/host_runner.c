/**
 * host_runner.c — Minimal OpenCL host program for testing debugger attach and breakpoints.
 *
 * Owner: Person F
 *
 * Usage:
 *   ./test_host_runner [path_to_kernel.cl] [kernel_name] [options]
 *
 * Options:
 *   -g_size, -g, --global-size <g0[,g1,g2]>   Global work size (comma-separated for 1D/2D/3D)
 *   -l_size, -l, --local-size <l0[,l1,l2]>     Local work size (comma-separated for 1D/2D/3D)
 *   -k, --kernel <name>                       Kernel function name
 *   -f, --file <path>                         Kernel source file path
 *   --buffers <N>                             Allocate N float buffers sized to total global work
 * items
 *   --arg-buf <size_in_floats>                Add a float buffer argument with specific element
 * count
 *   --arg-int, --scalar-int <value>           Add a 32-bit integer scalar argument
 *   --arg-float <value>                       Add a 32-bit float scalar argument
 *
 * Header comments:
 *   If CLI parameters are not supplied, host_runner will inspect the kernel source file
 *   for a header comment (e.g. `// FLAGS: -k vec_add -g_size 64 -l_size 16 --buffers 3`
 *   or `// FLAGS[kernel_name]: ...`). If neither CLI flags nor header comments are found,
 *   host_runner errors out.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>

enum { MAX_DIMS = 3, MAX_KERNEL_ARGS = 16 };

typedef enum { ARG_TYPE_BUFFER, ARG_TYPE_INT, ARG_TYPE_FLOAT } ArgType;

typedef struct {
    ArgType type;
    size_t buf_elements;
    int int_val;
    float float_val;
} KernelArgConfig;

typedef struct {
    size_t work_dim;
    size_t global_size[MAX_DIMS];
    size_t local_size[MAX_DIMS];
    int has_local_size;

    size_t num_args;
    KernelArgConfig args[MAX_KERNEL_ARGS];
} RunnerConfig;

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

static int parse_sizes(const char *str, size_t *sizes, size_t *dim) {
    char buf[128];
    strncpy(buf, str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *token = strtok(buf, ",x ");
    size_t count = 0;
    while (token && count < MAX_DIMS) {
        sizes[count++] = (size_t)strtoul(token, NULL, 10);
        token = strtok(NULL, ",x ");
    }
    if (count == 0) {
        return 0;
    }
    *dim = count;
    return 1;
}

static char g_kernel_name[128] = {0};
static char g_kernel_file[512] = {0};

static int parse_size_flags(int argc, char **argv, int *index, RunnerConfig *config) {
    int i = *index;
    if (strcmp(argv[i], "-g_size") == 0 || strcmp(argv[i], "-g") == 0 ||
        strcmp(argv[i], "--global-size") == 0) {
        if (i + 1 < argc) {
            parse_sizes(argv[++i], config->global_size, &config->work_dim);
            *index = i;
            return 1;
        }
    } else if (strcmp(argv[i], "-l_size") == 0 || strcmp(argv[i], "-l") == 0 ||
               strcmp(argv[i], "--local-size") == 0) {
        if (i + 1 < argc) {
            size_t ldim = 0;
            parse_sizes(argv[++i], config->local_size, &ldim);
            config->has_local_size = 1;
            *index = i;
            return 1;
        }
    }
    return 0;
}

static int parse_name_flags(int argc, char **argv, int *index, const char **kernel_file,
                            const char **kernel_name) {
    int i = *index;
    if (strcmp(argv[i], "-k") == 0 || strcmp(argv[i], "--kernel") == 0) {
        if (i + 1 < argc) {
            strncpy(g_kernel_name, argv[++i], sizeof(g_kernel_name) - 1);
            g_kernel_name[sizeof(g_kernel_name) - 1] = '\0';
            *kernel_name = g_kernel_name;
            *index = i;
            return 1;
        }
    } else if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--file") == 0) {
        if (i + 1 < argc) {
            strncpy(g_kernel_file, argv[++i], sizeof(g_kernel_file) - 1);
            g_kernel_file[sizeof(g_kernel_file) - 1] = '\0';
            *kernel_file = g_kernel_file;
            *index = i;
            return 1;
        }
    }
    return 0;
}

static int parse_arg_spec_flags(int argc, char **argv, int *index, RunnerConfig *config) {
    int i = *index;
    if (strcmp(argv[i], "--buffers") == 0) {
        if (i + 1 < argc) {
            long count = strtol(argv[++i], NULL, 10);
            for (long b = 0; b < count && config->num_args < MAX_KERNEL_ARGS; ++b) {
                config->args[config->num_args].type = ARG_TYPE_BUFFER;
                config->args[config->num_args].buf_elements = 0;
                config->num_args++;
            }
            *index = i;
            return 1;
        }
    } else if (strcmp(argv[i], "--arg-buf") == 0) {
        if (i + 1 < argc && config->num_args < MAX_KERNEL_ARGS) {
            config->args[config->num_args].type = ARG_TYPE_BUFFER;
            config->args[config->num_args].buf_elements = (size_t)strtoul(argv[++i], NULL, 10);
            config->num_args++;
            *index = i;
            return 1;
        }
    } else if (strcmp(argv[i], "--arg-int") == 0 || strcmp(argv[i], "--scalar-int") == 0) {
        if (i + 1 < argc && config->num_args < MAX_KERNEL_ARGS) {
            config->args[config->num_args].type = ARG_TYPE_INT;
            config->args[config->num_args].int_val = (int)strtol(argv[++i], NULL, 10);
            config->num_args++;
            *index = i;
            return 1;
        }
    } else if (strcmp(argv[i], "--arg-float") == 0) {
        if (i + 1 < argc && config->num_args < MAX_KERNEL_ARGS) {
            config->args[config->num_args].type = ARG_TYPE_FLOAT;
            config->args[config->num_args].float_val = strtof(argv[++i], NULL);
            config->num_args++;
            *index = i;
            return 1;
        }
    }
    return 0;
}

static void parse_arg_tokens(int argc, char **argv, RunnerConfig *config, const char **kernel_file,
                             const char **kernel_name) {
    for (int i = 0; i < argc; ++i) {
        if (parse_size_flags(argc, argv, &i, config) ||
            parse_name_flags(argc, argv, &i, kernel_file, kernel_name) ||
            parse_arg_spec_flags(argc, argv, &i, config)) {
            continue;
        }
        if (argv[i][0] != '-') {
            if (kernel_file && !*kernel_file) {
                strncpy(g_kernel_file, argv[i], sizeof(g_kernel_file) - 1);
                g_kernel_file[sizeof(g_kernel_file) - 1] = '\0';
                *kernel_file = g_kernel_file;
            } else if (kernel_name && !*kernel_name) {
                strncpy(g_kernel_name, argv[i], sizeof(g_kernel_name) - 1);
                g_kernel_name[sizeof(g_kernel_name) - 1] = '\0';
                *kernel_name = g_kernel_name;
            }
        }
    }
}

static char *extract_flag_directive(char *line_buf, const char *filter_kernel) {
    if (filter_kernel && filter_kernel[0] != '\0') {
        char key[128];
        snprintf(key, sizeof(key), "FLAGS[%s]:", filter_kernel);
        char *match = strstr(line_buf, key);
        if (match) {
            return match + strlen(key);
        }
    }
    const char *patterns[] = {"FLAGS:", "RUNNER_ARGS:", "CONFIG:", NULL};
    for (int p = 0; patterns[p]; ++p) {
        char *match = strstr(line_buf, patterns[p]);
        if (match) {
            return match + strlen(patterns[p]);
        }
    }
    return NULL;
}

static void parse_flags_from_source(const char *src, const char *filter_kernel,
                                    RunnerConfig *config, const char **kernel_name) {
    const char *line = src;
    while (line && *line) {
        while (*line == ' ' || *line == '\t' || *line == '\r' || *line == '\n') {
            line++;
        }
        if (strncmp(line, "//", 2) != 0) {
            break;
        }
        const char *eol = strchr(line, '\n');
        size_t line_len = eol ? (size_t)(eol - line) : strlen(line);

        char line_buf[512];
        if (line_len >= sizeof(line_buf)) {
            line_len = sizeof(line_buf) - 1;
        }
        strncpy(line_buf, line, line_len);
        line_buf[line_len] = '\0';

        char *flag_start = extract_flag_directive(line_buf, filter_kernel);
        if (flag_start) {
            char *tokens[32];
            int token_count = 0;
            char *tok = strtok(flag_start, " \t\r\n");
            while (tok && token_count < 32) {
                tokens[token_count++] = tok;
                tok = strtok(NULL, " \t\r\n");
            }
            const char *dummy_file = NULL;
            parse_arg_tokens(token_count, tokens, config, &dummy_file, kernel_name);
            break;
        }

        line = eol ? eol + 1 : NULL;
    }
}

static void print_enqueue_message(const char *kernel_name, const RunnerConfig *config) {
    if (config->work_dim == 1) {
        printf("[host_runner] Enqueueing kernel %s (global=%zu, local=%zu)...\n", kernel_name,
               config->global_size[0], config->has_local_size ? config->local_size[0] : 0);
    } else if (config->work_dim == 2) {
        printf("[host_runner] Enqueueing kernel %s (global=[%zu,%zu], local=[%zu,%zu])...\n",
               kernel_name, config->global_size[0], config->global_size[1],
               config->has_local_size ? config->local_size[0] : 0,
               config->has_local_size ? config->local_size[1] : 0);
    } else if (config->work_dim == 3) {
        printf("[host_runner] Enqueueing kernel %s (global=[%zu,%zu,%zu], "
               "local=[%zu,%zu,%zu])...\n",
               kernel_name, config->global_size[0], config->global_size[1], config->global_size[2],
               config->has_local_size ? config->local_size[0] : 0,
               config->has_local_size ? config->local_size[1] : 0,
               config->has_local_size ? config->local_size[2] : 0);
    }
}

static int setup_kernel_args(cl_context context, cl_kernel kernel, const RunnerConfig *config,
                             size_t total_elements, cl_mem *d_buffers, float **h_buffers,
                             size_t *buf_counts, int *num_buffers) {
    *num_buffers = 0;
    cl_int err = CL_SUCCESS;

    for (size_t i = 0; i < config->num_args; ++i) {
        if (config->args[i].type == ARG_TYPE_BUFFER) {
            size_t count =
                config->args[i].buf_elements ? config->args[i].buf_elements : total_elements;
            float *h_buf = (float *)malloc(sizeof(float) * count);
            if (!h_buf) {
                return 0;
            }
            float init_val = 0.0F;
            if (i == 0) {
                init_val = 1.0F;
            } else if (i == 1 && config->num_args > 2) {
                init_val = 2.0F;
            }
            for (size_t j = 0; j < count; ++j) {
                h_buf[j] = init_val;
            }
            cl_mem d_buf = clCreateBuffer(context, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
                                          sizeof(float) * count, (void *)h_buf, &err);
            if (err != CL_SUCCESS) {
                fprintf(stderr, "[host_runner] Failed to create buffer %zu (err=%d)\n", i, err);
                free(h_buf);
                return 0;
            }
            clSetKernelArg(kernel, (cl_uint)i, sizeof(cl_mem), (const void *)&d_buf);
            d_buffers[*num_buffers] = d_buf;
            h_buffers[*num_buffers] = h_buf;
            buf_counts[*num_buffers] = count;
            (*num_buffers)++;
        } else if (config->args[i].type == ARG_TYPE_INT) {
            cl_int val = config->args[i].int_val;
            clSetKernelArg(kernel, (cl_uint)i, sizeof(cl_int), (const void *)&val);
        } else if (config->args[i].type == ARG_TYPE_FLOAT) {
            cl_float val = config->args[i].float_val;
            clSetKernelArg(kernel, (cl_uint)i, sizeof(cl_float), (const void *)&val);
        }
    }
    return 1;
}

static int run_generic_kernel(cl_context context, cl_command_queue queue, cl_kernel kernel,
                              const char *kernel_name, const RunnerConfig *config) {
    size_t total_elements = 1;
    for (size_t d = 0; d < config->work_dim; ++d) {
        total_elements *= config->global_size[d];
    }
    if (total_elements == 0) {
        total_elements = 64;
    }

    cl_mem d_buffers[MAX_KERNEL_ARGS];
    float *h_buffers[MAX_KERNEL_ARGS];
    size_t buf_counts[MAX_KERNEL_ARGS];
    int num_allocated_buffers = 0;

    if (!setup_kernel_args(context, kernel, config, total_elements, d_buffers, h_buffers,
                           buf_counts, &num_allocated_buffers)) {
        return 1;
    }

    print_enqueue_message(kernel_name, config);

    cl_int err =
        clEnqueueNDRangeKernel(queue, kernel, (cl_uint)config->work_dim, NULL, config->global_size,
                               config->has_local_size ? config->local_size : NULL, 0, NULL, NULL);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "[host_runner] Failed to enqueue kernel (err=%d)\n", err);
    }

    clFinish(queue);

    if (num_allocated_buffers > 0) {
        int last_idx = num_allocated_buffers - 1;
        clEnqueueReadBuffer(queue, d_buffers[last_idx], CL_TRUE, 0,
                            sizeof(float) * buf_counts[last_idx], (void *)h_buffers[last_idx], 0,
                            NULL, NULL);
        printf("[host_runner] Result sample: dst[0]=%.1f\n", h_buffers[last_idx][0]);
    }

    for (int b = 0; b < num_allocated_buffers; ++b) {
        clReleaseMemObject(d_buffers[b]);
        free(h_buffers[b]);
    }

    return (err == CL_SUCCESS) ? 0 : 1;
}

int main(int argc, char **argv) {
    const char *kernel_file = NULL;
    const char *kernel_name = NULL;

    RunnerConfig config;
    memset(&config, 0, sizeof(config));

    // Parse command line arguments
    parse_arg_tokens(argc - 1, argv + 1, &config, &kernel_file, &kernel_name);

    if (!kernel_file) {
        kernel_file = "examples/kernels/vector_add.cl";
    }

    size_t src_size = 0;
    char *kernel_src = read_file(kernel_file, &src_size);
    if (!kernel_src) {
        fprintf(stderr, "[host_runner] Could not read file %s\n", kernel_file);
        return 1;
    }

    // If work size was not specified on CLI, parse header comments in the kernel file
    if (config.work_dim == 0) {
        parse_flags_from_source(kernel_src, kernel_name, &config, &kernel_name);
    }

    // If still not configured, report error as required
    if (config.work_dim == 0 || !kernel_name || config.num_args == 0) {
        fprintf(stderr,
                "[host_runner] Error: Execution parameters not found for '%s'.\n"
                "Please provide CLI flags (-g_size, -k, --buffers, etc.) or a header comment "
                "in the kernel file (e.g. '// FLAGS: -k <name> -g_size <dim> ...').\n",
                kernel_file);
        free(kernel_src);
        return 1;
    }

    printf("[host_runner] Loading kernel file: %s (kernel name: %s)\n", kernel_file, kernel_name);

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

    run_generic_kernel(context, queue, kernel, kernel_name, &config);

    clReleaseKernel(kernel);
    clReleaseProgram(program);
    clReleaseCommandQueue(queue);
    clReleaseContext(context);
    free(kernel_src);

    printf("[host_runner] Kernel execution finished successfully.\n");
    return 0;
}
