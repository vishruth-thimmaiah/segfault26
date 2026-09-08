// RUN: rm -rf %t.cache
// RUN: env POCL_CACHE_DIR=%t.cache POCL_LEAVE_KERNEL_COMPILER_TEMP_FILES=1 POCL_EXTRA_BUILD_FLAGS="-g -cl-opt-disable" %test_host_runner %s
// RUN: %llvm-dis $(find %t.cache -name "program.bc" | head -n 1) -o - | %FileCheck %s --check-prefix=CHECK-PROGRAM-IR
// RUN: %llvm-dis $(find %t.cache -name "parallel.bc" | head -n 1) -o - | %FileCheck %s --check-prefix=CHECK-PARALLEL-IR

/**
 * pocl_ir.cl — LIT test demonstrating that pocl uses LLVM IR as its internal
 * representation and applies IR transformation passes to lower OpenCL kernels
 * into native workgroup loops while preserving DWARF debug metadata.
 */

__kernel void vec_add(__global const float *a,
                      __global const float *b,
                      __global float       *c)
{
    int gx = get_global_id(0);
    c[gx] = a[gx] + b[gx];
}

// -----------------------------------------------------------------------------
// Section 1: Verification that pocl compiles OpenCL C into initial LLVM IR
// -----------------------------------------------------------------------------
// CHECK-PROGRAM-IR-DAG: @_global_id_x = external constant i64
// CHECK-PROGRAM-IR-DAG: @_local_id_x = external constant i64
// CHECK-PROGRAM-IR: define dso_local spir_kernel void @vec_add(ptr {{.*}}%a, ptr {{.*}}%b, ptr {{.*}}%c) {{.*}}!dbg ![[SP_PROG:[0-9]+]]
// CHECK-PROGRAM-IR: ![[SP_PROG]] = distinct !DISubprogram(name: "vec_add"
// CHECK-PROGRAM-IR: !DILocalVariable(name: "a", arg: 1
// CHECK-PROGRAM-IR: !DILocalVariable(name: "b", arg: 2
// CHECK-PROGRAM-IR: !DILocalVariable(name: "c", arg: 3
// CHECK-PROGRAM-IR: !DILocalVariable(name: "gx"

// -----------------------------------------------------------------------------
// Section 2: Verification that pocl transforms LLVM IR into workgroup loops
// -----------------------------------------------------------------------------
// CHECK-PARALLEL-IR: define void @_pocl_kernel_vec_add_workgroup(ptr %0, ptr %1, i64 %2, i64 %3, i64 %4) {{.*}}!dbg ![[SP_WG:[0-9]+]]
// CHECK-PARALLEL-IR: #dbg_value(ptr %{{.*}}, ![[VAR_A:[0-9]+]], !DIExpression(), ![[LOC_INL:[0-9]+]])
// CHECK-PARALLEL-IR: phi i64 [ 0, %{{.*}} ], [ %{{.*}}, %{{.*}} ]
// CHECK-PARALLEL-IR: fadd float
// CHECK-PARALLEL-IR: store float
// CHECK-PARALLEL-IR: ![[SP_WG]] = distinct !DISubprogram(name: "_pocl_kernel_vec_add_workgroup"
// CHECK-PARALLEL-IR: ![[VAR_A]] = !DILocalVariable(name: "a"
// CHECK-PARALLEL-IR: distinct !DISubprogram(name: "vec_add"
