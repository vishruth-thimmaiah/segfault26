// RUN: %ocldbg --dry-run --track-wg --inspect-vars %test_host_runner %s reduce_sum | %FileCheck %s --check-prefix=CHECK-INSPECT

/**
 * variable_inspection.cl — Tests live DWARF variable inspection at work-group dispatch.
 *
 * Verifies that:
 * 1. ocldbg halts at the first work-group dispatch.
 * 2. DWARFSourceModel identifies in-scope kernel parameters and local variables.
 * 3. OpenCL pointer types are annotated with __global address space.
 * 4. CPULocationBackend evaluates target register values via LLDB.
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

// =============================================================================
// Live Variable Inspection at Work-Group Entry for reduce_sum
// =============================================================================
// CHECK-INSPECT: [ocldbg] Inspecting variables for stopped WI({{[0-9]+}},{{[0-9]+}},{{[0-9]+}}) grp({{[0-9]+}},{{[0-9]+}},{{[0-9]+}}):
// CHECK-INSPECT-DAG: src (const float * __global) = 0x{{[0-9a-fA-F]+}}
// CHECK-INSPECT-DAG: dst (float * __global) = 0x{{[0-9a-fA-F]+}}
// CHECK-INSPECT-DAG: n (int) = 4
// CHECK-INSPECT-DAG: acc (float) = 0.000000
// CHECK-INSPECT: [ocldbg] Work-Group Dispatch Tracking:
// CHECK-INSPECT-NEXT:   Dispatches: 4
// CHECK-INSPECT-NEXT:   Expected:   4
// CHECK-INSPECT: [ocldbg] Dry run completed successfully.
