// RUN: %ocldbg --dry-run --show-wg-bounds %test_host_runner %s reduce_sum | %FileCheck %s --check-prefix=CHECK-REDUCE
// RUN: %ocldbg --dry-run --show-wg-bounds %test_host_runner %s vec_add | %FileCheck %s --check-prefix=CHECK-VECADD
// RUN: %ocldbg --dry-run --show-wg-bounds %test_host_runner %s volume_acc | %FileCheck %s --check-prefix=CHECK-3D

/**
 * workgroup_bounds.cl — Tests dynamic NDRange inference from kernel call sites
 * and verifies WorkGroupTracker bounds and WIContextExtractor mappings.
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

__kernel void vec_add(__global const float *a,
                      __global const float *b,
                      __global float       *c)
{
    int gid = get_global_id(0);
    c[gid] = a[gid] + b[gid];
}

__kernel void volume_acc(__global const float *src,
                         __global float       *dst)
{
    int gx = get_global_id(0);
    int gy = get_global_id(1);
    int gz = get_global_id(2);
    int idx = (gz * 8 * 8) + (gy * 8) + gx;
    dst[idx] = src[idx] + 1.0f;
}

// =============================================================================
// Inferred NDRange and Work-Group Bounds for reduce_sum (global=16, local=4)
// =============================================================================
// CHECK-REDUCE: [ocldbg] Inferred NDRange from call site:
// CHECK-REDUCE-NEXT:   Global Size: (16,1,1)
// CHECK-REDUCE-NEXT:   Local Size:  (4,1,1)
// CHECK-REDUCE-NEXT:   Work-Groups: (4,1,1)
// CHECK-REDUCE: [ocldbg] Work-Group Bounds:
// CHECK-REDUCE-NEXT:   WG (0,0,0): global range [(0,0,0) - (3,0,0)] (4 work-items)
// CHECK-REDUCE-NEXT:   WG (1,0,0): global range [(4,0,0) - (7,0,0)] (4 work-items)
// CHECK-REDUCE-NEXT:   WG (2,0,0): global range [(8,0,0) - (11,0,0)] (4 work-items)
// CHECK-REDUCE-NEXT:   WG (3,0,0): global range [(12,0,0) - (15,0,0)] (4 work-items)
// CHECK-REDUCE: [ocldbg] Work-Item Context Mapping:
// CHECK-REDUCE-NEXT:   WI (0,0,0) -> WG (0,0,0) Local (0,0,0)
// CHECK-REDUCE-NEXT:   WI (3,0,0) -> WG (0,0,0) Local (3,0,0)
// CHECK-REDUCE-NEXT:   WI (4,0,0) -> WG (1,0,0) Local (0,0,0)
// CHECK-REDUCE-NEXT:   WI (15,0,0) -> WG (3,0,0) Local (3,0,0)
// CHECK-REDUCE: [ocldbg] Dry run completed successfully.

// =============================================================================
// Inferred NDRange and Work-Group Bounds for vec_add (global=64, local=16)
// =============================================================================
// CHECK-VECADD: [ocldbg] Inferred NDRange from call site:
// CHECK-VECADD-NEXT:   Global Size: (64,1,1)
// CHECK-VECADD-NEXT:   Local Size:  (16,1,1)
// CHECK-VECADD-NEXT:   Work-Groups: (4,1,1)
// CHECK-VECADD: [ocldbg] Work-Group Bounds:
// CHECK-VECADD-NEXT:   WG (0,0,0): global range [(0,0,0) - (15,0,0)] (16 work-items)
// CHECK-VECADD-NEXT:   WG (1,0,0): global range [(16,0,0) - (31,0,0)] (16 work-items)
// CHECK-VECADD-NEXT:   WG (2,0,0): global range [(32,0,0) - (47,0,0)] (16 work-items)
// CHECK-VECADD-NEXT:   WG (3,0,0): global range [(48,0,0) - (63,0,0)] (16 work-items)
// CHECK-VECADD: [ocldbg] Work-Item Context Mapping:
// CHECK-VECADD-NEXT:   WI (0,0,0) -> WG (0,0,0) Local (0,0,0)
// CHECK-VECADD-NEXT:   WI (15,0,0) -> WG (0,0,0) Local (15,0,0)
// CHECK-VECADD-NEXT:   WI (16,0,0) -> WG (1,0,0) Local (0,0,0)
// CHECK-VECADD-NEXT:   WI (63,0,0) -> WG (3,0,0) Local (15,0,0)
// CHECK-VECADD: [ocldbg] Dry run completed successfully.

// =============================================================================
// Inferred NDRange and Work-Group Bounds for volume_acc (global=[8,8,4], local=[2,2,2])
// =============================================================================
// CHECK-3D: [ocldbg] Inferred NDRange from call site:
// CHECK-3D-NEXT:   Global Size: (8,8,4)
// CHECK-3D-NEXT:   Local Size:  (2,2,2)
// CHECK-3D-NEXT:   Work-Groups: (4,4,2)
// CHECK-3D: [ocldbg] Work-Group Bounds:
// CHECK-3D-NEXT:   WG (0,0,0): global range [(0,0,0) - (1,1,1)] (8 work-items)
// CHECK-3D:        WG (3,3,1): global range [(6,6,2) - (7,7,3)] (8 work-items)
// CHECK-3D: [ocldbg] Work-Item Context Mapping:
// CHECK-3D-NEXT:   WI (0,0,0) -> WG (0,0,0) Local (0,0,0)
// CHECK-3D-NEXT:   WI (1,0,0) -> WG (0,0,0) Local (1,0,0)
// CHECK-3D-NEXT:   WI (2,0,0) -> WG (1,0,0) Local (0,0,0)
// CHECK-3D-NEXT:   WI (7,0,0) -> WG (3,0,0) Local (1,0,0)
// CHECK-3D-NEXT:   WI (0,1,0) -> WG (0,0,0) Local (0,1,0)
// CHECK-3D-NEXT:   WI (0,0,1) -> WG (0,0,0) Local (0,0,1)
// CHECK-3D: [ocldbg] Dry run completed successfully.
