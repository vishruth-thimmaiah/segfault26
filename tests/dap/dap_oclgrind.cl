// REQUIRES: oclgrind
// RUN: rm -rf %t.kcache && env POCL_CACHE_DIR=%t.kcache OCLDBG_DAP_BACKEND=oclgrind OCLDBG_BUILD_OPTIONS=-cl-opt-disable python3 %S/dap_driver.py %ocldbg %test_host_runner %s reduce_sum | %FileCheck %s
// FLAGS: -k reduce_sum -g_size 16 -l_size 4 --arg-buf 80 --arg-buf 16 --arg-int 4
__kernel void reduce_sum(__global const float *src,
                         __global float       *dst,
                         int                   n)
{
    int gx = get_global_id(0);

    float acc = 0.0f;
    // accumulate sum
    for (int i = 0; i <= n; i++) {
        acc += src[gx * n + i];
    }

    dst[gx] = acc;
}

/**
 * dap_oclgrind.cl — Drives the Oclgrind backend through the DAP protocol.
 *
 * The same server and the same requests as dap_session.cl, against the other
 * backend. It stands on the work-item carrying its own source location: while
 * the stop location was read out of the backend's private execution context,
 * this path returned another backend's state reinterpreted as the CPU one.
 *
 * Variables are not checked here. Resolving them needs the DWARF the kernel
 * module carries, and Oclgrind interprets IR with no such module on disk.
 */

// CHECK: [dap] initialized
// CHECK: [dap] launched
// CHECK: [dap] breakpoints set: line 13
// CHECK: [dap] stopped at breakpoint
// CHECK: [dap] threads: Work-Item (0) [Work-Group (0)]
// CHECK: [dap] stackTrace: {{.*}} at line 13
// CHECK: [dap] scopes: __local
// CHECK: [dap] selectWorkItem: Work-Item (0) [Work-Group (0)]
// CHECK: [dap] continue
// CHECK: [dap] stopped at breakpoint
// CHECK: [dap] disconnected
