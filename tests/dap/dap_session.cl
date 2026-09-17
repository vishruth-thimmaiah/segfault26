// RUN: rm -rf %t.kcache && env POCL_CACHE_DIR=%t.kcache POCL_CPU_MAX_CU_COUNT=1 POCL_CPU_NUM_WORKERS=1 python3 %S/dap_driver.py %ocldbg %test_host_runner %s reduce_sum | %FileCheck %s --check-prefixes=CHECK%if x86_64 %{,CHECK-PARAM%}
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

// CHECK: [dap] initialized
// CHECK: [dap] launched
// CHECK: [dap] breakpoints set: line 13
// CHECK: [dap] stopped at breakpoint
// CHECK: [dap] threads: WI(0,0,0) grp(0,0,0)
// CHECK: [dap] stackTrace: line 13
// CHECK: [dap] scopes: __local
// CHECK: [dap] var: acc = 0.000000
// pocl gives the kernel's parameters no debug info on AArch64, so n can only
// be read on x86_64. Everything else here works on both.
// CHECK-PARAM: [dap] var: n = 4
// CHECK: [dap] selectWorkItem: WI(0,0,0) grp(0,0,0)
// CHECK: [dap] continue
// CHECK: [dap] stopped at breakpoint
// CHECK: [dap] var: acc = 1.000000
// CHECK: [dap] disconnected
