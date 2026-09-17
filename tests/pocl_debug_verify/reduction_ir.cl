// RUN: rm -rf %t.cache
// RUN: env POCL_CACHE_DIR=%t.cache POCL_LEAVE_KERNEL_COMPILER_TEMP_FILES=1 POCL_EXTRA_BUILD_FLAGS="-g -cl-opt-disable" %test_host_runner %s
// RUN: %llvm-dis $(find %t.cache -name "program.bc" | head -n 1) -o - | %FileCheck %s --check-prefix=CHECK-PROGRAM-IR
// RUN: %llvm-dis $(find %t.cache -name "parallel.bc" | head -n 1) -o - | %FileCheck %s --check-prefix=CHECK-PARALLEL-IR
// FLAGS: -k reduce_sum -g_size 16 -l_size 4 --arg-buf 80 --arg-buf 16 --arg-int 4
/**
 * reduction_ir.cl — LIT test demonstrating pocl's IR lowering for loops and reductions.
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

// -----------------------------------------------------------------------------
// Section 1: Initial LLVM IR checks (Clang OpenCL front-end output)
// -----------------------------------------------------------------------------
// 1a. OpenCL ID pseudo-globals:
// CHECK-PROGRAM-IR-DAG: @_global_id_x = external constant i64
// CHECK-PROGRAM-IR-DAG: @_local_id_x = external constant i64

// 1b. Kernel signature:
// CHECK-PROGRAM-IR: define dso_local spir_kernel void @reduce_sum(ptr {{.*}}%src, ptr {{.*}}%dst, i32 {{.*}}%n) {{.*}}!dbg ![[SP_PROG:[0-9]+]]

// 1c. Entry allocas and debug declarations:
// CHECK-PROGRAM-IR: entry:
// CHECK-PROGRAM-IR:   %src.addr = alloca ptr
// CHECK-PROGRAM-IR:   %dst.addr = alloca ptr
// CHECK-PROGRAM-IR:   %n.addr = alloca i32
// CHECK-PROGRAM-IR:   %gx = alloca i32
// CHECK-PROGRAM-IR:   %acc = alloca float
// CHECK-PROGRAM-IR:   %i = alloca i32
// CHECK-PROGRAM-IR:   #dbg_declare(ptr %src.addr, ![[VAR_SRC:[0-9]+]], !DIExpression(), !{{[0-9]+}})
// CHECK-PROGRAM-IR:   #dbg_declare(ptr %dst.addr, ![[VAR_DST:[0-9]+]], !DIExpression(), !{{[0-9]+}})
// CHECK-PROGRAM-IR:   #dbg_declare(ptr %n.addr, ![[VAR_N:[0-9]+]], !DIExpression(), !{{[0-9]+}})
// CHECK-PROGRAM-IR:   #dbg_declare(ptr %gx, ![[VAR_GX:[0-9]+]], !DIExpression(), !{{[0-9]+}})

// 1d. get_global_id call and initialization:
// CHECK-PROGRAM-IR:   call i64 @_Z13get_global_idj(i32 {{.*}}0)
// CHECK-PROGRAM-IR:   store i32 %{{.*}}, ptr %gx
// CHECK-PROGRAM-IR:   #dbg_declare(ptr %acc, ![[VAR_ACC:[0-9]+]], !DIExpression(), !{{[0-9]+}})
// CHECK-PROGRAM-IR:   store float 0.000000e+00, ptr %acc
// CHECK-PROGRAM-IR:   #dbg_declare(ptr %i, ![[VAR_I:[0-9]+]], !DIExpression(), !{{[0-9]+}})
// CHECK-PROGRAM-IR:   store i32 0, ptr %i
// CHECK-PROGRAM-IR:   br label %for.cond

// 1e. Loop condition (shows <= off-by-one comparison):
// CHECK-PROGRAM-IR: for.cond:
// CHECK-PROGRAM-IR:   %[[I_VAL:[0-9]+]] = load i32, ptr %i
// CHECK-PROGRAM-IR:   %[[N_VAL:[0-9]+]] = load i32, ptr %n.addr
// CHECK-PROGRAM-IR:   %cmp = icmp sle i32 %[[I_VAL]], %[[N_VAL]]
// CHECK-PROGRAM-IR:   br i1 %cmp, label %for.body, label %for.end

// 1f. Loop body arithmetic & accumulation:
// CHECK-PROGRAM-IR: for.body:
// CHECK-PROGRAM-IR:   %mul = mul nsw i32 %{{.*}}, %{{.*}}
// CHECK-PROGRAM-IR:   %add = add nsw i32 %mul, %{{.*}}
// CHECK-PROGRAM-IR:   getelementptr inbounds float, ptr %{{.*}}, i64
// CHECK-PROGRAM-IR:   fadd float
// CHECK-PROGRAM-IR:   store float %{{.*}}, ptr %acc
// CHECK-PROGRAM-IR:   br label %for.inc

// 1g. Loop latch & exit:
// CHECK-PROGRAM-IR: for.inc:
// CHECK-PROGRAM-IR:   add nsw i32 %{{.*}}, 1
// CHECK-PROGRAM-IR:   store i32 %{{.*}}, ptr %i
// CHECK-PROGRAM-IR:   br label %for.cond
// CHECK-PROGRAM-IR: for.end:
// CHECK-PROGRAM-IR:   store float %{{.*}}, ptr %{{.*}}
// CHECK-PROGRAM-IR:   ret void

// 1h. DWARF metadata descriptors:
// CHECK-PROGRAM-IR: ![[SP_PROG]] = distinct !DISubprogram(name: "reduce_sum"
// CHECK-PROGRAM-IR: ![[VAR_SRC]] = !DILocalVariable(name: "src", arg: 1
// CHECK-PROGRAM-IR: ![[VAR_DST]] = !DILocalVariable(name: "dst", arg: 2
// CHECK-PROGRAM-IR: ![[VAR_N]] = !DILocalVariable(name: "n", arg: 3
// CHECK-PROGRAM-IR: ![[VAR_GX]] = !DILocalVariable(name: "gx"
// CHECK-PROGRAM-IR: ![[VAR_ACC]] = !DILocalVariable(name: "acc"
// CHECK-PROGRAM-IR: ![[VAR_I]] = !DILocalVariable(name: "i"

// -----------------------------------------------------------------------------
// Section 2: Transformed LLVM IR checks (workgroup loops & context allocas)
// -----------------------------------------------------------------------------
// 2a. Workgroup kernel function:
// CHECK-PARALLEL-IR: define void @_pocl_kernel_reduce_sum_workgroup(ptr %0, ptr %1, i64 %2, i64 %3, i64 %4) {{.*}}!dbg ![[SP_WG:[0-9]+]]

// 2b. Context allocas for intermediate per-work-item state:
// CHECK-PARALLEL-IR: %.acc.0.ex_phi.pocl_context{{.*}} = alloca [1 x [1 x [{{[0-9]+}} x float]]]
// CHECK-PARALLEL-IR: %.i.0.ex_phi.pocl_context{{.*}} = alloca [1 x [1 x [{{[0-9]+}} x i32]]]

// 2c. Argument unmarshalling:
// CHECK-PARALLEL-IR: #dbg_value(ptr %{{.*}}, ![[VAR_SRC_PAR:[0-9]+]], !DIExpression(), ![[LOC_INL:[0-9]+]])
// CHECK-PARALLEL-IR: #dbg_value(ptr %{{.*}}, ![[VAR_DST_PAR:[0-9]+]], !DIExpression(), ![[LOC_INL]])
// CHECK-PARALLEL-IR: #dbg_value(i32 %{{.*}}, ![[VAR_N_PAR:[0-9]+]], !DIExpression(), ![[LOC_INL]])

// 2d. Work-item loop induction variables:
// CHECK-PARALLEL-IR: %_local_id_x.i.0 = phi i64 [ 0, %{{.*}} ], [ %{{.*}}, %{{.*}} ]
// CHECK-PARALLEL-IR: %_global_id_x.i.0 = phi i64 [ %{{.*}}, %{{.*}} ], [ %{{.*}}, %{{.*}} ]

// 2e. Per-work-item variable tracking inside loop:
// CHECK-PARALLEL-IR: #dbg_value(i32 %conv.i.i, ![[VAR_GX_PAR:[0-9]+]], !DIExpression(), ![[LOC_INL]])
// CHECK-PARALLEL-IR: #dbg_value(float 0.000000e+00, ![[VAR_ACC_PAR:[0-9]+]], !DIExpression(), ![[LOC_INL]])
// CHECK-PARALLEL-IR: #dbg_value(i32 0, ![[VAR_I_PAR:[0-9]+]], !DIExpression(), !{{[0-9]+}})

// 2f. Indexed store into context array using local ID:
// CHECK-PARALLEL-IR: getelementptr [1 x [1 x [{{[0-9]+}} x float]]], ptr %.acc.0.ex_phi.pocl_context{{.*}}, i64 0, i64 0, i64 0, i64 %_local_id_x.i.0
// CHECK-PARALLEL-IR: store float 0.000000e+00

// 2g. DWARF metadata & inlining hierarchy:
// CHECK-PARALLEL-IR: ![[SP_WG]] = distinct !DISubprogram(name: "_pocl_kernel_reduce_sum_workgroup"
// CHECK-PARALLEL-IR: ![[VAR_SRC_PAR]] = !DILocalVariable(name: "src"
// CHECK-PARALLEL-IR: distinct !DISubprogram(name: "reduce_sum"
// CHECK-PARALLEL-IR: ![[VAR_DST_PAR]] = !DILocalVariable(name: "dst"
// CHECK-PARALLEL-IR: ![[VAR_N_PAR]] = !DILocalVariable(name: "n"
// CHECK-PARALLEL-IR: ![[VAR_GX_PAR]] = !DILocalVariable(name: "gx"
// CHECK-PARALLEL-IR: ![[VAR_ACC_PAR]] = !DILocalVariable(name: "acc"
// CHECK-PARALLEL-IR: ![[VAR_I_PAR]] = !DILocalVariable(name: "i"
