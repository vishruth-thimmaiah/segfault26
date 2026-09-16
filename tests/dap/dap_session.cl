// RUN: rm -rf %t.kcache && env POCL_CACHE_DIR=%t.kcache POCL_CPU_MAX_CU_COUNT=1 POCL_CPU_NUM_WORKERS=1 \
// RUN:   python3 %S/dap_driver.py %ocldbg %test_host_runner %s reduce_sum | %FileCheck %s

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

// CHECK: [dap] initialized
// CHECK: [dap] launched
// CHECK: [dap] breakpoints set: line 13
// CHECK: [dap] stopped at breakpoint
// CHECK: [dap] threads: WI(0,0,0) grp(0,0,0)
// CHECK: [dap] stackTrace: line 13
// CHECK: [dap] scopes: Locals
// CHECK: [dap] var: acc = 0.000000
// CHECK: [dap] var: n = 4
// CHECK: [dap] selectWorkItem: WI(0,0,0) grp(0,0,0)
// CHECK: [dap] continue
// CHECK: [dap] stopped at breakpoint
// CHECK: [dap] var: acc = 1.000000
// CHECK: [dap] disconnected
