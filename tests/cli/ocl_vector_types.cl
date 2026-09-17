// RUN: rm -rf %t.kcache && env POCL_CACHE_DIR=%t.kcache POCL_CPU_MAX_CU_COUNT=1 POCL_CPU_NUM_WORKERS=1 %ocldbg -b -o "ocl break ocl_vector_types.cl:10" -o "run" -o "ocl p vec" -o "ocl p small" -o "ocl vars" -o "c" %test_host_runner %s vec_kernel | %FileCheck %s
// FLAGS: -k vec_kernel -g_size 4 -l_size 4 --arg-buf 4
__kernel void vec_kernel(__global float *dst)
{
    int gx = get_global_id(0);
    volatile float8 vec = (float8)(1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f);
    volatile uchar4 small = (uchar4)(10, 20, 30, 40);
    float offset = (float)gx;

    dst[gx] = vec.x + (float)small.x + offset;
}

// CHECK: (ocldbg) ocl break ocl_vector_types.cl:10
// CHECK: [ocldbg] Breakpoint #1 (pending) set at ocl_vector_types.cl:10
// CHECK: (ocldbg) run
// CHECK: [ocldbg] Breakpoint #1: resolved at address 0x{{[0-9a-fA-F]+}} (ocl_vector_types.cl:10)
// CHECK: Process {{[0-9]+}} stopped
// CHECK: stop reason = breakpoint
// CHECK: at tempfile_{{[0-9a-zA-Z]+}}.cl:10
// CHECK: (ocldbg) ocl p vec
// CHECK: (float8) vec = (1.000000, 2.000000, 3.000000, 4.000000, 5.000000, 6.000000, 7.000000, 8.000000)
// CHECK: (ocldbg) ocl p small
// CHECK: (uchar4) small = (10, 20, 30, 40)
// CHECK: (ocldbg) ocl vars
// CHECK-DAG: vec (float8) = (1.000000, 2.000000, 3.000000, 4.000000, 5.000000, 6.000000, 7.000000, 8.000000)
// CHECK-DAG: small (uchar4) = (10, 20, 30, 40)
// CHECK: (ocldbg) c
// CHECK: Process {{[0-9]+}} resuming

// Kept to x86_64: other CPU-backend tests in this directory (see ocl_break.cl)
// note pocl emits a different subset of a kernel's variables per target;
// not verified on aarch64.
// REQUIRES: x86_64
