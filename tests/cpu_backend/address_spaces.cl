// RUN: rm -rf %t.cache
// RUN: env POCL_CACHE_DIR=%t.cache POCL_CPU_NUM_WORKERS=1 %ocldbg --dry-run --break-at 28 \
// RUN:   --break-for 1 %test_host_runner %s addr_spaces | %FileCheck %s
// FLAGS: -k addr_spaces -g_size 16 -l_size 4 --arg-buf-ramp 16 1.0 --arg-buf-val 4 2.0 --arg-local 4 --arg-buf 16
/**
 * address_spaces.cl — Tests that kernel pointer arguments report the address
 * space they were declared with.
 *
 * pocl lowers the kernel to the host target before emitting the object, which
 * flattens every pointer to address space 0 and leaves no DW_AT_address_class,
 * so the three pointers below share one DWARF type. The address spaces come
 * from the !kernel_arg_addr_space metadata on the cached program bitcode
 * instead. This test fails if that is ignored and pointers are all reported as
 * __global, which is what a type-name guess produces.
 *
 * __global coverage lives in variable_inspection.cl. Scalars carry no address
 * space qualifier and are expected to stay unannotated.
 */

__kernel void addr_spaces(__global const float *src,
                          __constant float     *table,
                          __local float        *scratch,
                          __global float       *dst)
{
    int gx = get_global_id(0);
    int lx = get_local_id(0);

    scratch[lx] = src[gx] * table[0];
    dst[gx] = scratch[lx];
}

// CHECK: Breakpoint hit at line 28 for WI({{[0-9]+}},0,0) grp({{[0-9]+}},0,0) (hit 1):
// CHECK-DAG: table (float * __constant) = 0x{{[0-9a-fA-F]+}}
// CHECK-DAG: scratch (float * __local) = 0x{{[0-9a-fA-F]+}}
// CHECK-DAG: lx (int) =
