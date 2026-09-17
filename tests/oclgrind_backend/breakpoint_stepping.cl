// REQUIRES: oclgrind
// RUN: env OCLDBG_BUILD_OPTIONS=-cl-opt-disable %ocldbg --backend oclgrind --dry-run \
// RUN:   --break-at 24 --break-for 3 --print gx --print sum %test_host_runner %s \
// RUN:   | %FileCheck %s
// FLAGS: -k vec_add -g_size 64 -l_size 16 --arg-buf-ramp 64 1.0 --arg-buf-ramp 64 2.0 --arg-buf 64
/**
 * breakpoint_stepping.cl — Tests source-level debugging on the Oclgrind simulator.
 *
 * Verifies that:
 * 1. The debugger injects its plugin and halts the interpreter on a kernel line.
 * 2. Each halt reports the coordinates of one work-item, in a reproducible order.
 * 3. OclgrindLocationBackend reads named kernel variables at the halt.
 *
 * Oclgrind always compiles with debug info and rejects -g, hence the build
 * options override on the RUN line.
 */

__kernel void vec_add(__global const float *a,
                      __global const float *b,
                      __global float       *c)
{
    int gx = get_global_id(0);
    float sum = a[gx] + b[gx];
    c[gx] = sum;
}

// Work-items halt one at a time, in ascending global ID order.
// CHECK: Breakpoint hit at line 24 for WI(0,0,0) grp(0,0,0) (hit 1):
// CHECK-NEXT: gx = 0
// CHECK-NEXT: sum = 0
// CHECK: Breakpoint hit at line 24 for WI(1,0,0) grp(0,0,0) (hit 2):
// CHECK-NEXT: gx = 1
// CHECK-NEXT: sum = 3
// CHECK: Breakpoint hit at line 24 for WI(2,0,0) grp(0,0,0) (hit 3):
// CHECK-NEXT: gx = 2
// CHECK-NEXT: sum = 6
// CHECK: Oclgrind session completed with 3 breakpoint hit(s).
