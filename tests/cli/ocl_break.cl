// RUN: env POCL_CACHE_DIR=%t.kcache POCL_CPU_MAX_CU_COUNT=1 POCL_CPU_NUM_WORKERS=1 %ocldbg -b -o "ocl break ocl_break.cl:11" -o "ocl break list" -o "run" -o "ocl p acc" -o "ocl print n" -o "ocl vars" -o "c" -o "ocl p acc" -o "c" -o "c" -o "c" -o "c" -o "ocl p acc" -o "ocl break delete 1" -o "c" %test_host_runner %s reduce_sum | %FileCheck %s
// FLAGS: -k reduce_sum -g_size 16 -l_size 4 --arg-buf 80 --arg-buf 16 --arg-int 4

__kernel void reduce_sum(__global const float *src,
                         __global float       *dst,
                         int                   n)
{
    int gx = get_global_id(0);

    float acc = 0.0f;
    for (int i = 0; i <= n; i++) {
        acc += src[gx * n + i];
    }

    dst[gx] = acc;
}

// CHECK: (ocldbg) ocl break ocl_break.cl:11
// CHECK: [ocldbg] Breakpoint #1 (pending) set at ocl_break.cl:11
// CHECK: (ocldbg) ocl break list
// CHECK: [ocldbg] OpenCL Breakpoints:
// CHECK:   #1: ocl_break.cl:11 (pending)
// CHECK: (ocldbg) run
// CHECK: [ocldbg] Breakpoint #1: resolved at address 0x{{[0-9a-fA-F]+}} (ocl_break.cl:11)
// CHECK: Process {{[0-9]+}} stopped
// CHECK: stop reason = breakpoint
// CHECK: at tempfile_{{[0-9a-zA-Z]+}}.cl:11
// CHECK: (ocldbg) ocl p acc
// CHECK: (float) acc = 0.000000
// CHECK: (ocldbg) ocl print n
// CHECK: (int) n = 4
// CHECK: (ocldbg) ocl vars
// CHECK-DAG: src (const float * __global) = 0x{{[0-9a-fA-F]+}}
// CHECK-DAG: dst (float * __global) = 0x{{[0-9a-fA-F]+}}
// CHECK-DAG: n (int) = 4
// CHECK-DAG: acc (float) = 0.000000
// CHECK: (ocldbg) c
// CHECK: Process {{[0-9]+}} stopped
// CHECK: (ocldbg) ocl p acc
// CHECK: (float) acc = 1.000000
// CHECK: (ocldbg) c
// CHECK: (ocldbg) c
// CHECK: (ocldbg) c
// CHECK: (ocldbg) c
// CHECK: (ocldbg) ocl p acc
// CHECK: (float) acc = 2.000000
// CHECK: (ocldbg) ocl break delete 1
// CHECK: [ocldbg] Deleted OpenCL breakpoint #1
// CHECK: (ocldbg) c
// CHECK: Process {{[0-9]+}} resuming
// CHECK: Process {{[0-9]+}} exited with status = 0
