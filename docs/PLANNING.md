# segfault26 — Source-Level OpenCL Debugger

> Extending LLDB to give developers genuine source-level debugging of OpenCL kernels.
> The primary execution targets are **software backends** — pocl's CPU device and the
> Oclgrind OpenCL simulator — which model GPU execution semantics without requiring
> vendor driver access. Hardware GPU targets (AMD → Intel → NVIDIA) are stretch goals.
>
> **Central research question:** How can source-level debugging abstractions for massively
> parallel OpenCL execution be mapped onto fundamentally different execution models —
> CPU thread loops, LLVM IR interpreter lanes, and (later) GPU wavefronts — while
> preserving a common work-item / source abstraction?

---

## Table of Contents

1. [Problem & Motivation](#1-problem--motivation)
2. [High-Level Architecture](#2-high-level-architecture)
3. [Component Map](#3-component-map)
4. [Implementation Milestones](#4-implementation-milestones)
5. [GPU Target Priority](#5-gpu-target-priority)
6. [Key Technical Decisions](#6-key-technical-decisions)
7. [Data Flow: Breakpoint Hit](#7-data-flow-breakpoint-hit)
8. [Stretch Goals](#8-stretch-goals)
9. [Open Questions & Risks](#9-open-questions--risks)

---

## 1. Problem & Motivation

OpenCL kernels executing on a GPU produce no source-level debug signal at the driver
boundary. Developers are left with:

- `printf` injection (unreliable, changes execution behavior)
- Vendor debuggers (RenderDoc, Nsight, ROCgdb) — hardware-gated, closed-source hooks
- Offline ISA inspection — meaningless for everyday debugging

**The insight:** pocl lowers OpenCL C → LLVM IR → native host code and executes kernels
on the CPU through its "basic" CPU device. This means:

- LLVM's standard `-g`/`-gdwarf` pipeline can emit DWARF for kernels (subject to
  verifying it survives pocl's work-item lowering transforms — see Milestone 1)
- OS-level halting (ptrace / software breakpoints via LLDB) applies to the host
  execution context
- We can prototype a source-level debugger *without* any proprietary driver API

The key research contribution is not "build an LLDB frontend for OpenCL." It is
establishing a **common source-level debugging model for OpenCL work-items**
(`OCLWorkItem`) and demonstrating that this model can be implemented over fundamentally
different execution engines — pocl's transformed CPU execution and Oclgrind's LLVM IR
interpreter — with hardware GPU backends as future validation. The hardest, vendor-gated
part of GPU debugging (hardware-level halting) is explicitly deferred; the part that
survives across all backends (DWARF source model, work-item abstraction, DAP surface)
is built first.

---

## 2. High-Level Architecture

### 2a. Execution Model

A critical clarification about pocl's CPU driver:

> pocl does **not** create one OS thread per OpenCL work-item.

pocl executes work-**groups** using host threads, with the number of threads controlled
by `POCL_CPU_MAX_CU_COUNT`. Within each host thread, **multiple work-items** are
executed depending on the selected work-group execution strategy:

| pocl Strategy | Description |
|---------------|-------------|
| `loops`       | Work-item loop generated in IR; multiple WIs per thread |
| `loopvec`     | Loop + LLVM vectorization; WIs potentially vectorized together |
| `repl`        | Work-items replicated/chained |
| `cbs`         | Continuation-based synchronization |

The correct execution model is therefore:

```
OpenCL NDRange
    |
    v
Work-group
    |
    v
pocl CPU execution context (one or more host threads)
    |
    v
Multiple work-items executed within that context
  (via loop, vectorization, or cooperative scheduling)
```

This is a fundamentally harder debugger problem than CPU debugging: work-items are
**logical execution entities**, not directly OS threads.

### 2b. The OCL Debug Model

The central abstraction of this project is `OCLWorkItem` — a logical entity that
the debugger can inspect regardless of how the underlying hardware executes it:

```
OpenCL Source
      |
  DWARF / source locations
      |
  +---+---+
  |       |
  OCL     Execution
  Debug   Model
  Model   |
  |       +-------+-------+-------+
  |       |       |       |       |
  |      CPU    AMD    Intel  NVIDIA
  |      host   wave/  SIMD/  CUDA
  |      exec   lane   EU     thread
  |       |       |       |       |
  +-------+-------+-------+-------+
            |
      OCLWorkItem
        .global_id   (gx, gy, gz)
        .local_id    (lx, ly, lz)
        .group_id
        .exec_context  -> backend-specific handle
```

### 2c. DWARF Layering

The document formerly claimed "the DWARF resolver is reused unchanged across CPU and
GPU backends." That is incorrect. The **source-level** parts of DWARF are shared; the
**machine-level** parts are target-specific.

```
Shared (target-independent)           Target-specific
-------------------------------        --------------------------------
Source file / line table               Register numbering
Lexical scopes                         Register files (CPU regs vs. VGPRs)
Variable names and types               Lane selection
Location expression evaluation         Address spaces (AS 1/3/4 on AMDGPU)
Inlined function info                  Target-specific DW_OP extensions
```

Implementation:

```
OCLVariableResolver
    |
    +-- source_model (shared)
    |     +-- DWARF line table
    |     +-- DW_TAG_variable, DW_TAG_formal_parameter
    |     +-- DW_AT_type, scopes
    |
    +-- location_backend (per-target)
          +-- CPU:    LLDB SBFrame / SBValue (ptrace + DWARF exprs on x86 regs)
          +-- AMD:    ROCdbgapi wave register read + AMDGPU DWARF reg map
          +-- Intel:  zeDebugReadRegisters + Intel EU DWARF reg map
          +-- NVIDIA: cuda-gdb MI register read + PTX DWARF reg map
```

### 2d. System Diagram

```
+------------------------------------------------------------------+
|   Developer Workflow                                             |
|   kernel.cl  ->  host binary (pocl linked)  ->  run via ocldbg  |
+------------------------------------------------------------------+
                              |
              +---------------v----------------+
              |        ocldbg                  |
              |   (LLDB plugin + DAP server)   |
              +------+----------+--------------+
                     |          |
          +----------v--+   +---v-----------------+
          |  LLDB Core  |   |  OCL Debug Model    |
          |  (process   |   |  OCLWorkItem        |
          |   control)  |   |  OCLBreakpoint      |
          +----------+--+   |  OCLVariableResolver|
                     |      +---+--------+--------+
                     |          |        |
                     |    DWARF /   Location
                     |    source    Backend
                     |    model     (per-target)
                     |
              +-------v-----------------------+
              |   Execution Backend           |
              +------+-------+-------+-------+
                     |       |       |
               pocl CPU    AMD    Intel  NVIDIA
               (host      ROCdb-  L0    cuda-gdb
                exec)     gapi    Debug  MI
```

---

## 3. Component Map

```
segfault26/
+-- docs/
|   +-- PLANNING.md
+-- ocldbg/
|   +-- main.cpp                         <- CLI entry, DAP bootstrap
|   +-- ocl_debug_model/
|   |   +-- OCLWorkItem.h/cpp            <- central work-item abstraction
|   |   +-- OCLBreakpoint.h/cpp          <- source-location -> backend BP
|   |   +-- OCLVariableResolver.h/cpp    <- shared source/DWARF model
|   |   +-- OCLWorkItemSelector.h/cpp    <- "wi 3 0 0" command
|   +-- plugin/
|   |   +-- OCLDebugPlugin.cpp           <- LLDB SBPlugin registration
|   |   +-- OCLBreakpointResolver.cpp    <- source loc -> one or more PCs
|   +-- backends/
|   |   +-- Backend.h                    <- pure-virtual interface
|   |   +-- cpu/
|   |   |   +-- PoclCPUBackend.cpp       <- host thread execution, LLDB frames
|   |   |   +-- WorkGroupTracker.cpp     <- work-group -> host thread mapping
|   |   |   +-- WIContextExtractor.cpp   <- extract per-WI state from WG context
|   |   +-- amd/
|   |   |   +-- ROCdbgapiBackend.cpp     <- ROCdbgapi process/wave/breakpoint
|   |   |   +-- AMDLocationBackend.cpp   <- AMDGPU DWARF reg map, lane selection
|   |   +-- intel/
|   |   |   +-- L0Backend.cpp            <- zeDebug* API
|   |   |   +-- IntelLocationBackend.cpp <- Intel EU DWARF reg map, SIMD channels
|   |   +-- nvidia/
|   |       +-- CUDAGDBBridge.cpp        <- cuda-gdb MI protocol wrapper
|   |       +-- OCLCUDAMapping.cpp       <- OpenCL WI <-> CUDA thread mapping
|   +-- dap/
|   |   +-- DAPServer.cpp                <- Debug Adapter Protocol server
|   +-- dwarf/
|       +-- DWARFSourceModel.cpp         <- shared: line tables, scopes, types
|       +-- LocationExprEval.cpp         <- DW_OP_* evaluation (target-aware)
+-- runtime_inject/
|   +-- libocldbg_rt.so
|       +-- wg_dispatch_hook.cpp         <- intercept work-group dispatch
|       +-- wi_state_export.cpp          <- export WG->host-thread map via shmem
+-- tests/
|   +-- kernels/
|   |   +-- hello_kernel.cl
|   |   +-- reduction_bug.cl             <- intentional off-by-one in reduction
|   |   +-- matrix_mul.cl
|   +-- integration/
|       +-- test_breakpoint.py
+-- scripts/
    +-- build.sh
    +-- demo.sh
```

---

## 4. Implementation Milestones

The project is organized around **risk reduction** rather than calendar phases.
The hardest problems are:

1. Establishing that DWARF survives pocl's kernel transformations
2. Reliably identifying and isolating per-work-item state
3. GPU breakpoint semantics and execution-model mapping

### Milestone 1 — pocl + DWARF Baseline

**Goal:** Prove the DWARF premise before writing any debugger code.

Tasks:
- [x] Install pocl from source (`-DENABLE_LLVM=ON`)
- [x] Run a kernel through pocl's CPU driver using **pocl's own compilation pipeline**.
  The correct way to test this is with pocl's documented debug environment variables:

```bash
# Tell pocl to add debug info and disable optimization through its own pipeline
export POCL_EXTRA_BUILD_FLAGS="-g -cl-opt-disable"
# Preserve all intermediate compilation files (bitcode, object, etc.)
export POCL_LEAVE_KERNEL_COMPILER_TEMP_FILES=1
# Run the host program normally — pocl compiles the kernel internally
./host_program
# Inspect the intermediate bitcode pocl actually used
llvm-dis /tmp/pocl-*/kernel*.bc -o kernel_pocl.ll
grep '!dbg' kernel_pocl.ll | head -20
dwarfdump /tmp/pocl-*/kernel*.o
```

  This is strictly better than compiling with standalone Clang: it examines what pocl
  *actually* generates, not an approximation. Standalone Clang can serve as a sanity
  check (does the source produce DWARF at all?), but it does not reflect pocl's
  work-item lowering passes.

- [x] Verify: does pocl's `loops` work-group function still carry `!dbg` metadata
  after work-item outlining? `-O0` is used as the initial controlled baseline because
  optimization, vectorization, and work-item transformations make source-level variable
  locations substantially harder to recover. LLVM has mechanisms for preserving variable
  locations through optimization (instruction-referencing debug info), but these are
  deferred until the `-O0` baseline is proven.
- [x] Establish: which generated function(s) correspond to which kernel source lines?
  (pocl may generate wrapper or outline functions around the kernel body)
- [x] Determine: can LLDB associate a stopped host PC back to `kernel.cl:42`?

**Note on pocl environment variables:**
`POCL_DEBUG=all` enables pocl diagnostic output. `POCL_DEBUG_LLVM_PASSES=1` enables
LLVM pass diagnostics. `POCL_EXTRA_BUILD_FLAGS` injects compiler flags into pocl's
kernel compilation. `POCL_LEAVE_KERNEL_COMPILER_TEMP_FILES=1` preserves intermediates.
All are documented by pocl. Use them to observe and test — not replace — pocl's pipeline.

---

### Milestone 2 — Work-Item State Identification

**Goal:** Reliably determine which work-item's state is visible at a stopped host PC.

This is the core research problem for the CPU backend. Because pocl does **not** create
one OS thread per work-item, we cannot simply map `pthread_t → (gx, gy, gz)`.

#### Resolution of Work-Group Tracking Mechanism (Option A: Breakpoint-Assisted / LLDB-Native)

Experimental inspection of pocl 7.1 (`libpocl-devices-pthread.so`) revealed that:
1. Pocl dispatches work-groups inside an internal `work_group_scheduler` loop by directly invoking
   the kernel work-group function via an internal function pointer (`call *0xb8(%r14)`), with no
   stable exported symbol called between work-groups. A standard symbol-interposition `LD_PRELOAD`
   shim cannot hook individual work-group dispatches without modifying pocl source.
2. In the System V AMD64 ABI, pocl passes work-group coordinates directly in registers upon entry to
   `_pocl_kernel_<name>_workgroup`:
   - `%rdx` = `group_id_x`
   - `%rcx` = `group_id_y`
   - `%r8`  = `group_id_z`
3. Therefore, **Option A (Breakpoint-Assisted / LLDB-Native Tracking)** is adopted:
   - When a kernel is loaded/enqueued, LLDB sets an internal breakpoint on `_pocl_kernel_<name>_workgroup`.
   - On hit, `WorkGroupTracker` inspects the registers on the hitting thread (`thread.GetThreadID()`),
     records `thread_to_wg_[tid] = {group_id_x, group_id_y, group_id_z}`, and resumes execution.
   - This requires **zero modifications to pocl** and eliminates the need for an external shared-memory shim.

At a stopped PC inside the host thread, `WorkGroupTracker` maps the host thread to its
current work-group. Extracting the **current work-item** within that work-group then
depends on the execution strategy:

| pocl Strategy | Work-item identification at stopped PC |
|---------------|----------------------------------------|
| `loops`       | Loop induction variable in register/frame (e.g. `%rsi` or `%r10` / DWARF local var) |
| `loopvec`     | Harder — SIMD lane; may need LLDB vector register inspection |
| `cbs`         | Continuation state; requires understanding the CBS data structures |

**For the initial prototype, explicitly target `loops` strategy only** with
`-O0`/`-cl-opt-disable`. Document this constraint.

```
OCLWorkItem
  .global_id  = group_offset + loop_induction_var
  .local_id   = loop_induction_var
  .group_id   = from WorkGroupTracker (recorded on WG function entry)
  .exec_ctx   = host_tid + frame
```

**Deliverable:** Given a stopped host thread inside pocl's kernel execution, reliably
recover `(gx, gy, gz)` for the current work-item. Validated on `reduction_bug.cl`.

---

### Milestone 3 — Variable Inspection

**Goal:** Given a stopped work-item, resolve source-level variable names and values.

The variable resolver has two layers:

**Layer 1: Source/DWARF model (shared)**

```cpp
class DWARFSourceModel {
    // Source file/line table lookup
    SourceLocation pc_to_source(uint64_t pc);
    // Variable discovery in scope at a given PC
    vector<VarInfo> variables_in_scope(uint64_t pc, uint64_t sp);
    // Type resolution
    TypeInfo resolve_type(DWARFDie die);
};
```

**Layer 2: Location backend (CPU-specific for now)**

```cpp
class CPULocationBackend : public LocationBackend {
    // Evaluate a DW_OP_* location expression in the context of
    // an LLDB SBFrame for the stopped host thread
    Value evaluate_location(DWARFExpr expr, SBFrame frame);
};
```

OCL-specific augmentation (applicable to all backends):
- Annotate pointer variables with address space (`__global`, `__local`, `__private`)
  derived from DWARF address-class attributes
- Display vector types (`float4`, `int8`) as structured values
- Show work-item coordinates alongside locals

```
(ocldbg) wi 3 0 0
Selected work-item (gx=3, gy=0, gz=0) in work-group (0,0,0)
[kernel.cl:42]

(ocldbg) vars
  int    local_id  = 3          [__private]
  float  acc       = 6.28       [__private]
  float* src       = 0x7f3a...  [__global, N=1024]

(ocldbg) mem src 32
0x7f3a...: 1.0 2.0 3.0 4.0 5.0 6.0 7.0 8.0
```

**Note on breakpoint→PC mapping:**
A single source line may map to multiple PCs (loop preamble, loop body, etc.), and
inlining/optimization can further complicate this. The breakpoint resolver must handle
one-to-many source-line → PC mappings:

```
kernel.cl:42
    |
    v
DWARF line program
    |
    v
{PC_a, PC_b, ...}   (one or more addresses)
    |
    v
Backend breakpoint(s) set at each address
    |
    v
On stop: identify dispatch / work-group / work-item
```

**Deliverable:** `vars` command produces correct values for `reduction_bug.cl`. The
off-by-one bug is findable by inspecting `acc` at the right work-item.

---

### Milestone 4 — Stepping & Breakpoint UX

**Goal:** `next`, `step`, `continue` work correctly from the work-item's perspective.

Stepping maps to LLDB thread-level operations on the host thread executing the work-group.
Key caveats:
- `next` steps over one source line in the host thread — which may correspond to
  executing multiple work-items' worth of loop iterations if the strategy is `loops`
- The exact semantics depend on the work-group execution strategy chosen in M2
- Do **not** claim "all other work-items keep running" — in `loops` mode, other WIs in
  the same work-group are simply iterations of the same loop on the same thread and are
  not running concurrently

Document the actual observed stepping semantics experimentally before publishing claims.

**Deliverable:** `next`/`step`/`continue` produce predictable, documented behavior.
The demo script reproduces the `reduction_bug.cl` debugging session end-to-end.

---

### Milestone 5 — DAP Server & IDE Integration

**Goal:** VS Code / Neovim debugger UI works out of the box via the Debug Adapter Protocol.

| DAP Request            | Implementation                                      |
|------------------------|-----------------------------------------------------|
| `initialize`           | Advertise capabilities                              |
| `launch`               | Start host binary under LLDB control                |
| `setBreakpoints`       | `OCLBreakpointResolver` → backend BP(s)             |
| `threads`              | Return **currently representable stop contexts** (see below) |
| `stackTrace`           | LLDB frames for selected WI's host thread           |
| `scopes/variables`     | `OCLVariableResolver` (M3) output                   |
| `continue/next/stepIn` | LLDB thread stepping (M4)                           |

#### DAP `threads` — not every work-item

Do **not** expose every work-item in the NDRange as a DAP thread. A 1024×1024 NDRange
would produce over a million entries. More importantly, a backend may only have state for
currently stopped or recently executed work-items. Introduce `OCLStopContext` to represent
what is actually inspectable at any given halt:

```cpp
struct OCLStopContext {
    OCLWorkItem stopped_wi;       // the WI at which execution halted
    vector<OCLWorkItem> visible;  // other WIs whose state is accessible
                                  // (e.g. same work-group on CPU, same wave on AMD)
};
```

The DAP `threads` response returns `stopped_wi` plus any `visible` entries — not the
entire NDRange. The user can request a different WI with `wi <gx> <gy> <gz>`, which
prompts the backend to make that WI's state available if possible.

VS Code `launch.json` template:

```json
{
  "type": "ocldbg",
  "request": "launch",
  "program": "${workspaceFolder}/host_binary",
  "kernel": "${workspaceFolder}/kernel.cl",
  "poclStrategy": "loops",
  "ndrange": [1024, 1, 1]
}
```

**Deliverable:** Full debug session from VS Code — gutter breakpoint, Threads panel
showing the stopped WI and its visible peers, variable hover, stepping buttons.

---

### Milestone 6 — Oclgrind Emulator Backend

**Goal:** Source-level breakpoints and variable inspection running on the Oclgrind
OpenCL device simulator — no GPU hardware required.

#### Why Oclgrind (recap)

Oclgrind is an OpenCL 1.2 ICD-compatible device simulator built on an LLVM IR
interpreter. Any OpenCL host program can use it by setting `OCLGRIND_DEVICE=1` (or
pointing the ICD loader at Oclgrind's `.icd` file) — no source changes required.

Its plugin API exposes C++ hooks that fire at interpreter execution events:

```cpp
// Oclgrind Plugin.h — hooks available to our plugin
class Plugin {
    virtual void instructionExecuted(
        const WorkItem* wi, const llvm::Instruction* inst, const TypedValue& result);
    virtual void memoryAtomicLoad(const Memory* mem, const WorkItem* wi, ...);
    virtual void memoryAtomicStore(const Memory* mem, const WorkItem* wi, ...);
    virtual void workGroupBarrier(const WorkGroup* wg, uint32_t flags);
    virtual void workItemBegin(const WorkItem* wi);
    virtual void workItemComplete(const WorkItem* wi);
};
```

This is a far simpler hook surface than any GPU debug API: we intercept at the LLVM IR
instruction level, where `!dbg` metadata is directly accessible and variable locations
are SSA values rather than physical register files.

#### Execution Model in Oclgrind

Oclgrind schedules work-items cooperatively within a work-group. The execution order
is deterministic and sequential (one WI runs to a synchronization point, then the next).
This is **different** from real GPU parallel execution, but it is faithful to the
OpenCL memory model and makes halting semantics straightforward:

```
OCLBreakpointResolver
  +-- source line 42 -> LLVM IR instruction with matching !dbg location
  +-- record target instruction pointer

OclgrindDebugPlugin::instructionExecuted(wi, inst, ...)
  +-- if inst == target_instruction:
  |     +-- read wi->globalID -> (gx, gy, gz)
  |     +-- if matches selected work-item (or any):
  |           +-- suspend execution (set halt flag)
  |           +-- notify ocldbg: "Stopped at kernel.cl:42 [WI(gx,gy,gz)]"
```

#### Oclgrind Already Has an Interactive Debugger — Extend It

A key finding that makes this milestone more tractable: **Oclgrind ships an existing
interactive debugger** with breakpoints, stepping, variable printing, memory inspection,
and work-item switching. This debugger was documented in Codeplay's SYCL debugging work
and is part of the mainline Oclgrind codebase.

The implementation strategy for this milestone is therefore **not** to build a debugger
from scratch using raw plugin callbacks, but to:

1. Study Oclgrind's existing interactive debugger implementation to understand how it
   accesses work-item execution state and variable values.
2. Expose that state through the `OCLWorkItem` / `LocationBackend` interfaces.
3. Wrap the result in the DAP server to give IDE-native access.

This positions the research contribution correctly:

```
Existing Oclgrind interactive debugger
              |
              v
    OclgrindBackend (our adapter)
              |
              v
       OCLWorkItem model  <-- shared with CPU backend
              |
        DAP server  <-- shared with CPU backend
              |
        VS Code / Neovim / CLion
```

#### Location Backend for Oclgrind (to be established from Oclgrind source)

The exact mechanism for retrieving source-level variable values from Oclgrind's
interpreter **must be established by reading Oclgrind's debugger source**, not assumed
in advance. The plugin API provides:

```cpp
// Known from Plugin.h — these hooks exist
virtual void instructionExecuted(
    const WorkItem* wi, const llvm::Instruction* inst, const TypedValue& result);
virtual void workItemBegin(const WorkItem* wi);
virtual void workItemComplete(const WorkItem* wi);
```

The `TypedValue& result` from `instructionExecuted` gives the result of each executed
instruction. Whether this is sufficient to reconstruct named variable values (vs.
requiring access to Oclgrind's internal memory/state model) is to be determined by
examining `src/core/WorkItem.{h,cpp}` and the existing interactive debugger
implementation. Do not assume a `WorkItem::getVariable()` API or a direct
`DW_OP_regN → SSA` mapping until verified.

#### Integration Path

```
+---------------------+         +------------------------+
| ocldbg DAP server   |         | Oclgrind process       |
|                     |  IPC /  |                        |
|  OCLBreakpoint   ---+--pipe-->|  OclgrindBackend       |
|  OCLWorkItem        |         |  (adapts existing      |
|  OCLVariableResolver|<--------+   Oclgrind debugger    |
|  DAPServer          |  halt   |   + plugin hooks)      |
+---------------------+  notify +------------------------+
```

Options for ocldbg ↔ Oclgrind IPC:
- **In-process (start here):** Our backend runs as an Oclgrind plugin in the same
  process. Simplest for prototyping.
- **Out-of-process:** Plugin writes halt events to a Unix socket; ocldbg reads and
  sends DAP `stopped` events. Needed for full DAP server separation.

#### Open Questions Specific to Oclgrind (also in §9)

- Does `instructionExecuted` fire at IR instruction granularity?
- How does Oclgrind's existing interactive debugger retrieve named variable values?
  (Read `src/plugins/InteractiveDebugger.cpp`)
- Can execution be suspended mid-work-group, or only at work-item boundaries?
- Is the sequential WI scheduling order stable across Oclgrind versions?

**Deliverable:** End-to-end demo — `reduction_bug.cl` runs on Oclgrind, breakpoint at
the reduction line halts at WI (3,0,0), `acc` inspected and correct, stepping works.
All without any GPU hardware. The `OclgrindBackend` implements the same `Backend`
interface as `CPUBackend`, proving the common abstraction works across two different
execution engines.


## 5. Execution Target Priority

### Primary Targets (in-scope)

| Priority | Target | Mechanism | Why |
|----------|--------|-----------|-----|
| 1 | **pocl CPU device** | Host pthreads + LLDB/ptrace | Foundation; DWARF from LLVM; no hardware needed |
| 2 | **Oclgrind** (emulator) | LLVM IR interpreter + plugin API | Native OpenCL ICD; plugin hooks; no hardware needed |

### Hardware GPU Targets (stretch / future)

| Priority | Vendor | Mechanism | Key Risk |
|----------|--------|-----------|----------|
| 3 | **AMD** | ROCdbgapi (`librocm-dbgapi`) | Consumer GPU availability; driver support level |
| 4 | **Intel** | Level Zero `zeDebug*` API | Debug driver build requirement |
| 5 | **NVIDIA** | cuda-gdb MI bridge | OpenCL WI ↔ CUDA thread mapping gap |

### Why Oclgrind over MGPUSim or gem5?

**Oclgrind** is the clear choice for a second emulator target:

- **Native OpenCL ICD:** Oclgrind is an OpenCL 1.2 ICD-compatible device. Any OpenCL
  program runs on it without modification — no ROCm toolchain, no HSACO compilation,
  no OS boot.
- **LLVM IR interpreter:** Kernels execute at the LLVM IR level, so DWARF metadata is
  naturally available at instruction granularity. Variable locations are directly readable
  without register-file translation.
- **Built-in plugin API:** Oclgrind exposes a C++ plugin interface with hooks for
  instruction execution, memory access, barrier crossing, and work-item scheduling.
  An interactive debug mode already exists as a reference.
- **Active maintenance (LLVM 18+):** Packaged in Ubuntu 24.04 LTS; updated for modern
  LLVM/Clang toolchains.
- **Faithful GPU semantics:** Correctly models NDRange, work-groups, work-items,
  `__local`/`__global`/`__private` memory — the semantics we care about.

**MGPUSim** requires the ROCm toolchain, a Go-based host, and only targets AMD GCN3.
**gem5 (GPUFS)** requires booting a full Linux kernel; cycle-accurate simulation is
ordered-of-magnitude slower and oriented toward performance research, not debugger
development.

Oclgrind's LLVM IR interpreter also means the **location backend for Oclgrind is far
simpler** than any real GPU backend: variable locations are LLVM IR SSA values, not
physical register files, so the `DWARFSourceModel` maps almost directly.

---

## 6. Key Technical Decisions

### 6.1 OCLWorkItem as the Central Abstraction

Rather than mapping work-items directly to LLDB `SBThread` objects (which only makes
sense on the CPU backend), introduce `OCLWorkItem` as the debugger's primary object:

```cpp
struct OCLWorkItem {
    size3 global_id;   // (gx, gy, gz)
    size3 local_id;    // (lx, ly, lz)
    size3 group_id;
    ExecutionContext exec_ctx;  // backend-specific: host frame, wave+lane, EU+channel, etc.
};
```

The backend interface:

```cpp
class Backend {
    virtual OCLWorkItem select_work_item(size3 global_id) = 0;
    virtual Value read_variable(OCLWorkItem wi, VarInfo var) = 0;
    virtual void set_breakpoint(SourceLocation loc) = 0;
    virtual void step(OCLWorkItem wi) = 0;
};
```

Command dispatch:

```
(ocldbg) wi 3 0 0
    |
    v
OCLWorkItemSelector::select({3,0,0})
    |
    v
backend->select_work_item({3,0,0})
    |
    +-- CPU:       locate loop induction var in LLDB frame
    +-- Oclgrind:  plugin-provided WI ID from interpreter state
    +-- AMD:       find wave containing WI, set lane  [stretch]
    +-- Intel:     find EU thread + SIMD channel      [stretch]
    +-- NVIDIA:    (blockIdx, threadIdx) via pocl CUDA [stretch]
```

### 6.2 LLDB as the Host Process Control Engine

LLDB's `liblldb` handles process launch, ptrace, symbol loading, and DWARF parsing on
the CPU backend. On GPU backends, LLDB is still used for host-side process control
(e.g., the host OpenCL runtime process) while GPU-side control goes through the
respective GPU debug API.

### 6.3 Two-Layer Variable Resolution

The `OCLVariableResolver` has a **shared DWARF source layer** and a
**target-specific location backend**. They are not the same component. The source layer
is written once; the location backend is written per target. See §2c.

### 6.4 pocl Strategy Constraint (Phase 1)

Phase 1 explicitly targets pocl's `loops` work-group execution strategy at `-O0`.
Higher optimization levels and other strategies (loopvec, cbs) are deferred — they
introduce vectorization and cooperative scheduling that complicate both DWARF survival
and work-item identification. State this constraint explicitly in the demo.

### 6.5 DAP as the Primary UI Protocol

Exposing the debugger as a DAP server means VS Code, Neovim (nvim-dap), Emacs
(dap-mode), and CLion all work without IDE-specific code.

---

## 7. Data Flow: Breakpoint Hit (CPU Backend)

```
User: breakpoint set --file kernel.cl --line 42
                |
                v
OCLBreakpointResolver
  +-- DWARF line program for kernel.cl
  +-- line 42  ->  {PC_a, PC_b}  (one or more addresses)
  +-- backend->set_breakpoint(PC_a), set_breakpoint(PC_b)
                |
                v
pocl executes kernel (work-groups on host threads, loops strategy)
  host_thread_7 executing WG(1,0,0), loop iter i=3
  hits PC_a  ->  SIGTRAP  ->  LLDB stops host_thread_7
                |
                v
CPUBackend::on_stop(host_thread_7)
  +-- shim lookup: host_thread_7  ->  WG(1,0,0)
  +-- read loop induction var from LLDB frame  ->  local_id=3
  +-- compute global_id  =  group_offset + local_id  =  (3,0,0)
  +-- emit: "Stopped at kernel.cl:42 [WI(3,0,0) in WG(1,0,0)]"
                |
                v
User: wi 3 0 0; vars
  +-- OCLWorkItemSelector  ->  OCLWorkItem{(3,0,0), (3,0,0), (1,0,0), frame}
  +-- DWARFSourceModel: variables in scope at PC_a
  +-- CPULocationBackend: evaluate DW_OP_* exprs in LLDB SBFrame
  +-- OCL augmentation: annotate address spaces
                |
                v
Output:
  float  acc = 6.28  [__private]
  int    i   = 3     [__private]
  float* src = 0x7f... [__global]
```

---

## 8. Stretch Goals

### 8a. Optimized Kernel Debugging
Support `-O1`/`-O2` kernels. Key challenge: LLVM's debug info tracks variable locations
through optimization, but locations can disappear or become partially available.
Requires LLVM's instruction-referencing debug info (`-Xclang -femit-debug-entry-values`).

### 8b. Instrumentation-Based Data Race Detection
At a barrier, log all memory accesses `(address, WI, PC, operation)` via IR
instrumentation (not just passive debugger state inspection). Detect conflicting writes
to the same address from different work-items. This is **instrumentation-based**, not
a passive debugger extension.

### 8c. Divergence Visualization
At a conditional branch, display which work-items took the `if` path and which took
`else`. On CPU (`loops`), this requires stepping each loop iteration individually.
On AMD wavefronts, read the execution mask register.

### 8d. Conditional Breakpoints Per Work-Item

```
breakpoint set --file k.cl --line 42 --condition "gx == 5 && acc > 10.0"
```

Evaluated in the work-item's context; skips halt for non-matching work-items.

### 8e. AMD Hardware Backend (ROCdbgapi)
Integrate with `librocm-dbgapi` for process/wave/breakpoint/register control on AMD GPUs.
Requires ROCm stack and a compatible AMD GPU. The `AMDLocationBackend` must handle
AMDGPU DWARF register numbering and lane-qualified VGPR reads.

### 8f. Intel Hardware Backend (Level Zero)
Integrate with `zeDebug*` APIs for Intel Xe/Arc GPUs. May require a special debug build
of the Intel `compute-runtime` (NEO driver). Analogous `IntelLocationBackend` needed.

### 8g. NVIDIA Backend (cuda-gdb MI bridge)
Feasibility-first: establish if pocl's CUDA backend produces a CUDA-debuggable execution
model and whether the OpenCL WI ↔ CUDA `(blockIdx, threadIdx)` mapping is reliable.

### 8h. Watchpoints on Global Buffers
Halt the first work-item that writes to a specific `__global` address. On pocl CPU:
LLDB hardware watchpoints. On Oclgrind: plugin memory-write hook. On AMD: ROCdbgapi
watchpoint API.

---

## 9. Open Questions & Risks

| # | Question | Impact | How to resolve |
|---|----------|--------|----------------|
| 1 | Does pocl's `loops` work-group function retain `!dbg` LLVM metadata after WI outlining? | Core M1 blocker | **Resolved (Verified):** `-g -cl-opt-disable` retains `!dbg` subprograms, variables (`#dbg_value`), and line tables in both `program.bc` and `parallel.bc`. |
| 2 | Is the loop induction variable for work-item ID reliably readable via LLDB at `-O0`? | M2 core assumption | **Resolved (Verified):** The induction variable is readable in `%rsi` (simple loops) or `%r10` / stack context array (`reduce_sum`), stepping line-by-line accurately. |
| 3 | **Can pocl's CPU driver be instrumented at WG execution boundaries without modifying pocl source?** | M2 core architecture | **Resolved:** Dispatch loop is internal (`call *0xb8(%r14)`); resolved via **Option A** (LLDB internal breakpoint on `_pocl_kernel_*_workgroup` reading `%rdx`, `%rcx`, `%r8`), avoiding shim complexity or pocl source patches. |
| 4 | Does pocl emit correct `DW_AT_address_class` for OpenCL address spaces? | Variable annotation | Inspect DWARF output from pocl pipeline |
| 5 | Does Oclgrind's `instructionExecuted` callback fire at IR instruction granularity? | M6 breakpoint precision | Read `src/core/Plugin.h` and `KernelInvocation.cpp` |
| 6 | How does Oclgrind's existing interactive debugger retrieve named variable values? | M6 location backend | Read `src/plugins/InteractiveDebugger.cpp` |
| 7 | Can the Oclgrind plugin API suspend/resume WI execution, or only observe passively? | M6 halt semantics | Review interactive debug plugin as reference |
| 8 | Is Oclgrind's sequential WI execution order stable across versions? | M6 stepping | Empirical |
| 9 | [Stretch] Is ROCdbgapi available on consumer AMD GPUs (RX 7000 series)? | 8e scope | ROCm compatibility matrix |
| 10 | [Stretch] Does Intel `zeDebug*` require a special NEO driver build? | 8f scope | Intel docs; build `compute-runtime` with debug flags |
---

## References

### Primary targets
- [pocl source](https://github.com/pocl/pocl) — `lib/CL/devices/basic/`
- [pocl CPU driver documentation](https://pocl.sourceforge.net/docs/html/using.html) — work-group execution strategies
- [pocl CHANGES](https://github.com/pocl/pocl/blob/main/CHANGES) — history of CPU driver debug improvements
- [Oclgrind](https://github.com/jrprice/Oclgrind) — OpenCL device simulator, plugin API, interactive debug reference
- [Oclgrind Plugin.h](https://github.com/jrprice/Oclgrind/blob/master/src/core/Plugin.h) — plugin hook interface
- [Oclgrind KernelInvocation](https://github.com/jrprice/Oclgrind/blob/master/src/core/KernelInvocation.cpp) — LLVM IR interpreter internals

### Toolchain
- [LLDB SB API](https://lldb.llvm.org/python_api.html)
- [LLVM InstrRef Debug Info](https://github.com/llvm/llvm-project/blob/main/llvm/docs/InstrRefDebugInfo.md)
- [Debug Adapter Protocol spec](https://microsoft.github.io/debug-adapter-protocol/)

### Stretch / hardware targets
- [ROCdbgapi](https://github.com/ROCm/ROCdbgapi) — AMD Debugger API (`librocm-dbgapi`)
- [ROCdbgapi wave group docs](https://rocm.docs.amd.com/projects/ROCdbgapi/en/docs-6.0.0/doxygen/docBin/html/group__wave__group.html)
- [ROCr Debug Agent](https://rocm.docs.amd.com/projects/rocr_debug_agent/en/latest/how-to/user-guide.html) — diagnostic agent (not a control API)
- [LLVM DWARF for AMDGPU](https://llvm.org/docs/AMDGPUUsage.html#dwarf)
- [Intel Level Zero Debug API](https://spec.oneapi.io/level-zero/latest/tools/PROG.html#debug-api)
- [cuda-gdb documentation](https://docs.nvidia.com/cuda/cuda-gdb/index.html)
