// RUN: %ocldbg --dry-run --track-wg --inspect-vars %test_host_runner %s reduce_sum | %FileCheck %s --check-prefix=CHECK-INSPECT
// RUN: env POCL_CPU_NUM_WORKERS=1 %ocldbg --dry-run --break-at 23 --break-for 8 %test_host_runner %s reduce_sum | %FileCheck %s --check-prefix=CHECK-BREAK

/**
 * variable_inspection.cl — Tests live DWARF variable inspection at work-group dispatch.
 *
 * Verifies that:
 * 1. ocldbg halts at the first work-group dispatch.
 * 2. DWARFSourceModel identifies in-scope kernel parameters and local variables.
 * 3. OpenCL pointer types are annotated with __global address space.
 * 4. CPULocationBackend evaluates target register values via LLDB.
 * 5. --break-at and --break-for let work-items execute multiple iterations, verifying variable values change.
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

// =============================================================================
// Source-Line Breakpoint and Variable Inspection (--break-at / --break-for)
// =============================================================================
// Pass 1: Work-items start accumulating
// CHECK-BREAK: [ocldbg] Breakpoint hit at line 23 for WI({{[0-9]+}},{{[0-9]+}},{{[0-9]+}}) grp({{[0-9]+}},{{[0-9]+}},{{[0-9]+}}) (hit 1):
// CHECK-BREAK-DAG: src (const float * __global) = 0x{{[0-9a-fA-F]+}}
// CHECK-BREAK-DAG: dst (float * __global) = 0x{{[0-9a-fA-F]+}}
// CHECK-BREAK-DAG: n (int) = 4
// CHECK-BREAK-DAG: acc (float) = 0.000000
// CHECK-BREAK: [ocldbg] Breakpoint hit at line 23 for WI({{[0-9]+}},{{[0-9]+}},{{[0-9]+}}) grp({{[0-9]+}},{{[0-9]+}},{{[0-9]+}}) (hit 2):
// CHECK-BREAK: [ocldbg] Breakpoint hit at line 23 for WI({{[0-9]+}},{{[0-9]+}},{{[0-9]+}}) grp({{[0-9]+}},{{[0-9]+}},{{[0-9]+}}) (hit 3):
// CHECK-BREAK: [ocldbg] Breakpoint hit at line 23 for WI({{[0-9]+}},{{[0-9]+}},{{[0-9]+}}) grp({{[0-9]+}},{{[0-9]+}},{{[0-9]+}}) (hit 4):
// Pass 2: Subsequent iterations continue, with acc visibly incremented
// CHECK-BREAK: [ocldbg] Breakpoint hit at line 23 for WI({{[0-9]+}},{{[0-9]+}},{{[0-9]+}}) grp({{[0-9]+}},{{[0-9]+}},{{[0-9]+}}) (hit 5):
// CHECK-BREAK-DAG: src (const float * __global) = 0x{{[0-9a-fA-F]+}}
// CHECK-BREAK-DAG: dst (float * __global) = 0x{{[0-9a-fA-F]+}}
// CHECK-BREAK-DAG: n (int) = 4
// CHECK-BREAK-DAG: acc (float) = {{[0-9]+\.[0-9]+}}
// CHECK-BREAK: [ocldbg] Breakpoint hit at line 23 for WI({{[0-9]+}},{{[0-9]+}},{{[0-9]+}}) grp({{[0-9]+}},{{[0-9]+}},{{[0-9]+}}) (hit 6):
// CHECK-BREAK: [ocldbg] Breakpoint hit at line 23 for WI({{[0-9]+}},{{[0-9]+}},{{[0-9]+}}) grp({{[0-9]+}},{{[0-9]+}},{{[0-9]+}}) (hit 7):
// CHECK-BREAK: [ocldbg] Breakpoint hit at line 23 for WI({{[0-9]+}},{{[0-9]+}},{{[0-9]+}}) grp({{[0-9]+}},{{[0-9]+}},{{[0-9]+}}) (hit 8):
// CHECK-BREAK-DAG: acc (float) = {{[1-9][0-9]*\.[0-9]+}}
// CHECK-BREAK-NOT: [ocldbg] Breakpoint hit at line 23
// CHECK-BREAK: [ocldbg] Dry run completed successfully.
