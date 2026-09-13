// RUN: %ocldbg -b -o "ocl break reduction_bug.cl:23" -o "ocl break list" -o "run" -o "ocl break delete 1" -o "c" %test_host_runner %s reduce_sum | %FileCheck %s

/**
 * ocl_break.cl — Verifies that 'ocl break' sets pending breakpoints on OpenCL
 * source lines, resolves them upon kernel dispatch, stops worker threads,
 * and allows deleting breakpoints and resuming execution.
 */

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

// CHECK: (ocldbg) ocl break reduction_bug.cl:23
// CHECK: [ocldbg] Breakpoint #1 (pending) set at reduction_bug.cl:23
// CHECK: (ocldbg) ocl break list
// CHECK: [ocldbg] OpenCL Breakpoints:
// CHECK:   #1: reduction_bug.cl:23 (pending)
// CHECK: (ocldbg) run
// CHECK: [ocldbg] Breakpoint #1: resolved at address 0x{{[0-9a-fA-F]+}} (reduction_bug.cl:23)
// CHECK: Process {{[0-9]+}} stopped
// CHECK: stop reason = breakpoint
// CHECK: at tempfile_{{[0-9a-zA-Z]+}}.cl:23
// CHECK: (ocldbg) ocl break delete 1
// CHECK: [ocldbg] Deleted OpenCL breakpoint #1
// CHECK: (ocldbg) c
// CHECK: Process {{[0-9]+}} resuming
// CHECK: Process {{[0-9]+}} exited with status = 0
