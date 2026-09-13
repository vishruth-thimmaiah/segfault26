// RUN: env POCL_CACHE_DIR=%t.kcache %ocldbg -b -o "ocl break ocl_workitem.cl:12" -o "run" -o "ocl wi list" -o "ocl select 0" -o "ocl p gid" -o "ocl p val" -o "ocl select 3" -o "ocl wi list" -o "ocl p gid" -o "ocl p val" -o "c" -o "ocl wi list" -o "ocl select 1" -o "ocl p acc" -o "ocl break delete 1" -o "c" %test_host_runner %s reduce_sum | %FileCheck %s

__kernel void reduce_sum(__global const float *src,
                         __global float       *dst,
                         int                   n)
{
    volatile int gid = get_global_id(0);
    volatile float val = (float)gid * 2.5f;

    float acc = 0.0f;
    for (int i = 0; i <= n; i++) {
        acc += src[gid * n + i];
    }

    dst[gid] = acc + val;
}

// CHECK: (ocldbg) ocl break ocl_workitem.cl:12
// CHECK: [ocldbg] Breakpoint #1 (pending) set at ocl_workitem.cl:12
// CHECK: (ocldbg) run
// CHECK: [ocldbg] Breakpoint #1: resolved at address 0x{{[0-9a-fA-F]+}} (ocl_workitem.cl:12)
// CHECK: Process {{[0-9]+}} stopped
// CHECK: stop reason = breakpoint
// CHECK: at tempfile_{{[0-9a-zA-Z]+}}.cl:12
// CHECK: (ocldbg) ocl wi list
// CHECK: [ocldbg] Stopped work-items:
// CHECK-DAG: WI(0,0,0) grp(0,0,0)
// CHECK: (ocldbg) ocl select 0
// CHECK: [ocldbg] Selected work-item WI(0,0,0)
// CHECK: (ocldbg) ocl p gid
// CHECK: (int) gid = 3
// CHECK: (ocldbg) ocl p val
// CHECK: (float) val = 7.500000
// CHECK: (ocldbg) ocl select 3
// CHECK: [ocldbg] Selected work-item WI(3,0,0)
// CHECK: (ocldbg) ocl wi list
// CHECK: [ocldbg] Stopped work-items:
// CHECK: * WI({{[0-9]+}},0,0) grp(0,0,0)
// CHECK: (ocldbg) ocl p gid
// CHECK: (int) gid = 3
// CHECK: (ocldbg) ocl p val
// CHECK: (float) val = 7.500000
// CHECK: (ocldbg) c
// CHECK: Process {{[0-9]+}} resuming
// CHECK: Process {{[0-9]+}} stopped
// CHECK: (ocldbg) ocl wi list
// CHECK: [ocldbg] Stopped work-items:
// CHECK: (ocldbg) ocl select 1
// CHECK: [ocldbg] Selected work-item WI(1,0,0)
// CHECK: (ocldbg) ocl p acc
// CHECK: (float) acc = 1.000000
// CHECK: (ocldbg) ocl break delete 1
// CHECK: [ocldbg] Deleted OpenCL breakpoint #1
// CHECK: (ocldbg) c
// CHECK: Process {{[0-9]+}} resuming
// CHECK: Process {{[0-9]+}} exited with status = 0
