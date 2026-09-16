# CODEMAP.md — segfault26 dense repo index

> Purpose: let an agent answer "where is X / what does Y do / is Z implemented"
> from THIS file alone, without re-reading source. Read this file first on any
> new session in this repo. Only open source files for the specific lines you
> need to edit or verify.
>
> **Maintenance rule:** whenever you add/remove/rename a file, change a public
> class's method signatures, implement a TODO stub, or change which component
> owns a piece of logic, update the relevant section below in the same commit/
> session. Stale entries here are worse than none — if unsure, grep to confirm
> before trusting a line number. **This file drifts fast** — main gained ~3300
> lines (DAP server, aarch64 backend, address-space inference) in the ~18
> hours between this file's first version and this refresh. Don't trust a
> stale copy of this file across a big time gap without spot-checking.
>
> Repo root: `/home/shreyas/segfault/segfault26` (git repo, origin =
> github.com/vishruth-thimmaiah/segfault26, **private**). The parent
> `/home/shreyas/segfault` is just a container dir, not itself a repo.
>
> **Branch note (as of this refresh):** this copy of CODEMAP.md lives on
> `amd-gpu-backend`, an unmerged feature branch (PR #25) rebased onto current
> `main`. Sections describing the AMD backend (`backends/amd/`) describe code
> that is NOT YET on `main` — check `gh pr view 25` / `git log main` for
> current merge status before assuming it's there. Everything else in this
> file describes `main` as of commit `7338065` plus this branch's rebase.

## 0. One-liner

`ocldbg` — a source-level debugger for OpenCL/GPU kernels on three targets:
pocl's CPU device (via LLDB/ptrace, x86_64 *and* aarch64 hosts), the Oclgrind
LLVM-IR interpreter (via a custom plugin + socket protocol), and (this
branch, unmerged) real AMD GPU hardware via amd-dbgapi. Exposes both an
LLDB-passthrough CLI and a real Debug Adapter Protocol server (VS Code
extension included). C++20, CMake. Full design rationale lives in
`docs/PLANNING.md` (875 lines, now significantly stale — see §9).

## 1. Implementation status matrix (READ THIS FIRST)

Was **very unevenly implemented** as of ~2026-09-16; a large refactor since
then made the CPU path and DAP server real. Still spot-check before trusting
a line number below.

| Component | Status | Notes |
|---|---|---|
| `DWARFSourceModel` (dwarf/) | ✅ done | Real LLVM DWARFContext parsing: pc↔source, scope vars, type names |
| `CPUABI` base (backends/cpu/CPUABI.h/.cpp) | ✅ done | **New shared base class** (was pure-virtual-only before): `read_enqueue_ndrange`/`read_enqueue_kernel_name`/`read_workgroup_id` now live here, non-virtual, shared by every arch. Subclasses supply only `read_local_id` + `dwarf_register_name` + `arg_registers()` |
| `X86_64CPUABI` (backends/cpu/x86_64) | ✅ done | SysV register map; `dwarf_register_name` covers the x86_64 DWARF reg numbering |
| `AArch64CPUABI` (backends/cpu/aarch64) | ✅ done, **new** | AAPCS64 register map (x0-x4 for args/group id); CI now runs a full second `Build & Test (aarch64)` matrix job |
| `WIContextExtractor` (backends/cpu) | ✅ done | frame+wg_id+local_size → OCLWorkItem |
| `WorkGroupTracker` (backends/cpu) | ✅ done (in-proc only) | `thread_to_wg_` map works; `connect_shm()`/shm path is a no-op stub, unused |
| `OCLAddressSpaces` (ocl_debug_model/) | ✅ done, **new** | Parses pocl's cached program bitcode's `!kernel_arg_addr_space` metadata for real `__global`/`__local`/`__constant`/`__private` annotation of kernel **arguments** (pocl flattens address spaces out of the object's DWARF, so this is the only way to recover them). Does not cover pointers declared inside the kernel body — documented limitation |
| `CPULocationBackend::evaluate` | ✅ done, **substantially grown** | Implemented **inline in `PoclCPUBackend.cpp`**: DW_OP_reg/regx (via `CPUABI::dwarf_register_name`, arch-independent now), DW_OP_breg (incl. `DW_OP_stack_value`), DW_OP_fbreg, DW_OP_lit/consts/constu, DW_OP_implicit_value, LLDB SBValue lookup, a `k_`/`v_`-prefix alias fallback (pocl IR-transform variable renaming) and a static per-name value cache |
| `PoclCPUBackend` (backends/cpu/PoclCPUBackend.cpp) | ✅ done, **flipped from stub** | Used to be ~15 all-TODO methods; now a thin real `Backend` adapter that **wraps an internal `DebuggerContext`** (`impl_->dbg`) and delegates every method to it. This is the piece that made DAP-over-CPU possible |
| `DebuggerContext` (top-level CPU orchestrator) | ✅ done | 1149 lines (was 937). Still the real engine, but now **also reachable through `Backend`** via `PoclCPUBackend`, not just the CLI. New methods: `continue_execution()`, `read_memory()`, `redirect_output_to_stderr()`, `launch(..., stop_at_entry)`. See §4 for the method map |
| `OclgrindBackend` + protocol + plugin | ✅ done | Most complete subsystem, unchanged this cycle. Fork+exec host, Unix socket, line-based protocol |
| `OclgrindLocationBackend` | ✅ done | Sends `print <expr>` over the socket |
| `DebugPlugin` (oclgrind plugin, injected .so) | ✅ done | Full breakpoint/step/select/print/read-memory server loop |
| `OCLVariableResolver` | ✅ done | Constructor now takes `(DWARFSourceModel&, OCLAddressSpaces&)`, was DWARF-only before |
| `OclgrindSession` (`run_oclgrind_session`) | ✅ done | Standalone driver for `--backend oclgrind --dry-run` |
| CLI (`cli.cpp`, `CustomCommands.cpp`, `args.cpp`) | ✅ done | LLDB passthrough + `ocl break/print/vars/select/wi` commands |
| `DAPServer` (dap/) | ✅ done, **flipped from stub** | Was 58 lines that `throw`; now 817+ real lines: `Content-Length`-framed message loop over stdio *and* TCP (socket + fd-swap trick), real handlers for `initialize/launch/setBreakpoints/configurationDone/threads/stackTrace/scopes/variables/continue/next/stepIn/disconnect` + custom `ocldbg/selectWorkItem`, using `llvm::json`. Header (`DAPServer.h`) still carries its original "TODO (Person E)" doc comment — stale comment, not a functional gap |
| `LocationExprEval` (dwarf/) | ❌ **dead stub** | `evaluate()` always returns 0; class is **never instantiated anywhere** (confirmed via grep). Real DW_OP evaluation happens ad hoc in `PoclCPUBackend.cpp` |
| `runtime_inject/` (`libocldbg_rt.so`) | ❌ **dead stub** | Unchanged: no real interposition. **Superseded** by LLDB breakpoints on `_pocl_kernel_*_workgroup` / byte-pattern-scanning `libpocl-devices-pthread.so` |
| `tests/integration/test_breakpoint.py` | ❌ dead stub | The *old* DAP test scaffold, all `skipTest` — **superseded** by `tests/dap/dap_driver.py` + `dap_session.cl`, which is real and passes in CI. `test_breakpoint.py` itself was never updated and should probably be deleted |
| `tests/dap/dap_driver.py` + `dap_session.cl` | ✅ done, **new**, passing | Real DAP smoke test: drives `ocldbg --dap` over a subprocess pipe with the actual Content-Length framing |
| `.vscode/` + `extensions/vscode-ocldbg/` | ✅ present, **new** | A real (if minimal) VS Code extension (`extension.js`) registering `ocldbg` as a DAP debug type, plus `launch.json`/`tasks.json` wiring a "Debug Current Open Kernel" config that builds and launches through it |
| `scripts/demo.sh` | ⚠️ minimal, unchanged | Just builds + runs `ocldbg` with no args |
| `AMDDbgApiSession` (backends/amd, **unmerged PR #25**) | ✅ done, hardware-verified | launch/detach/resume-until-wave-stop/read_memory all real, verified against a live AMD Radeon RX 9060 XT (gfx1200) — this dev sandbox has real ROCm 7.2.1 + amd-dbgapi at `/opt/rocm`, not typical, check before assuming another box has this. Caught a real memory-violation fault from a genuinely buggy kernel end-to-end. Passes `run-clang-tidy` clean (see §5b) |
| `AMDBackend` / `AMDLocationBackend` (backends/amd, **unmerged PR #25**) | ⚠️ partial, real | `launch/detach/resume/on_stop/read_global_memory/location_backend` real. `set_breakpoint/remove_breakpoint/step_over/step_in` **not implemented** (open question — amd-dbgapi's `insert_breakpoint` callback is for the library's own host-side tracking, not a GPU breakpoint API). `select_work_item` not lane-precise. DWARF→register path wired but unverified on hardware |
| `AMDSession` (`run_amd_session`, **unmerged PR #25**) | ✅ done | Standalone driver for `--backend amd --dry-run` |

**Bottom line:** the CPU path now goes through `Backend`/`PoclCPUBackend`
properly (wrapping `DebuggerContext`), the same as Oclgrind always did — this
is what unblocked a real DAP server, which is now also genuinely implemented.
The AMD backend (this branch) is real except targeted breakpoints/stepping,
same caveat as before. `LocationExprEval` and `runtime_inject/` remain dead
code nobody has cleaned up.

**Build caveat carried over from the previous refresh:** LLDB dev headers
are still not installed in whatever sandbox last touched this file (check
`find / -iname 'SBFrame.h' 2>/dev/null` before assuming otherwise) — the
"done" claims above for non-AMD components rest on reading the code plus (for
the AMD backend) real standalone compiles/links/runs against the actual GPU,
not a full `cmake --build`. **This session's CI runs on GitHub Actions DID
successfully build and run the full `ocldbg` binary + LIT suite** (LLVM/LLDB
22 there) — see `.github/workflows/ci.yml`, now a 2×2 matrix (lint job +
`Build & Test` × {x86_64, aarch64}) — so the real build is validated there
even when it can't be validated locally.

## 2. Directory map (file → one-line purpose)

```
segfault26/
├── CMakeLists.txt              build graph — see §6
├── README.md                   build/run instructions, prereqs, usage examples
├── docs/PLANNING.md            design doc / research plan (§9: where it's stale)
├── .github/workflows/ci.yml    2 jobs: lint (format+tidy), build × {x86_64, aarch64} matrix
├── .clang-format / .clang-tidy style configs (WarningsAsErrors: '*' — see §6 pitfalls)
├── .vscode/                    launch.json ("Debug Current Open Kernel (ocldbg)" DAP config
│                                + an "lldb-dap" config for debugging ocldbg itself), tasks.json
├── extensions/vscode-ocldbg/   extension.js + package.json — registers "ocldbg" as a VS Code
│                                DAP debug type (thin: spawns `ocldbg --dap` and pipes stdio)
├── include/ocldbg/             PUBLIC headers — pure interfaces, no backend-specific code
│   ├── Types.h                 Size3, WorkGroupBound, WorkItemMapping,
│   │                           KernelLaunchInfo { kernel_name, work_dim, global_size, local_size,
│   │                             num_groups, work_groups[], sample_work_items[] },
│   │                           SourceLocation, HostAddress, ExecCtxHandle
│   ├── OCLWorkItem.h           OCLWorkItem { global_id, local_id, group_id, work_dim (NEW,
│   │                             NDRange dim 1-3), exec_ctx, exec_ctx_storage }, OCLStopContext
│   ├── LocationBackend.h       VarValue, VarInfo, LocationBackend (abstract)
│   ├── Backend.h                Backend (abstract): launch/breakpoints/stepping/WI-select/mem
│   └── DebuggerContext.h        DebuggerContext (PIMPL) — the CPU/LLDB orchestrator's public API
│
├── ocldbg/                      implementation
│   ├── main.cpp                 CLI entry; run_dry_run(); dispatches to CLI/DAP/oclgrind-session
│   ├── args.h / args.cpp        ProgramArgs (now has `dap_mode`), parse_arguments(), print_help/version
│   ├── cli.cpp                  DebuggerContext::run_cli() — interactive/batch LLDB+ocl loop
│   ├── CustomCommands.h/.cpp     handle_ocl_command(): ocl break/print/vars/select/workitem
│   ├── DebuggerContext.cpp      1149-LOC core: launch, NDRange inference, WG tracking,
│   │                           breakpoints, variable inspection, work-item selection, DAP handoff
│   ├── DebuggerContextImpl.h    DebuggerContext::Impl (private LLDB/state fields)
│   │
│   ├── backends/
│   │   ├── cpu/
│   │   │   ├── CPUABI.h/.cpp             ✅ shared base (NEW .cpp) — see §1/§4
│   │   │   ├── x86_64/X86_64CPUABI.h/.cpp   ✅ SysV-specific bits only now (shrunk ~180 lines
│   │   │   │                                  as shared logic moved to CPUABI.cpp)
│   │   │   ├── aarch64/AArch64CPUABI.h/.cpp ✅ NEW — AAPCS64-specific bits
│   │   │   ├── CPUExecContext.h         exec-ctx struct: host_thread_id + SBFrame + SBThread
│   │   │   ├── CPULocationBackend.h     interface decl only — impl is IN PoclCPUBackend.cpp
│   │   │   ├── WIContextExtractor.h/.cpp ✅ frame → OCLWorkItem
│   │   │   ├── WorkGroupTracker.h/.cpp   ✅ tid↔wg_id map (in-proc); shm path stub/unused
│   │   │   └── PoclCPUBackend.h/.cpp     ✅ NOW REAL: thin Backend adapter wrapping an owned
│   │   │                                    DebuggerContext; CPULocationBackend::evaluate lives
│   │   │                                    here too (bottom of file, real, rich DW_OP support)
│   │   ├── oclgrind/
│   │   │   ├── OclgrindProtocol.h        LineChannel + wire protocol consts — see §5
│   │   │   ├── OclgrindLocationBackend.h interface decl
│   │   │   ├── OclgrindBackend.h/.cpp    ✅ Backend impl: fork/exec host, socket IPC
│   │   │   ├── OclgrindSession.h/.cpp    ✅ run_oclgrind_session() — used by --dry-run --backend oclgrind
│   │   │   └── plugin/DebugPlugin.h/.cpp ✅ injected oclgrind::Plugin (separate .so, needs OCLGRIND_ROOT)
│   │   └── amd/                          UNMERGED (PR #25). Only compiled into ocldbg_core if
│   │       │                             amd-dbgapi found (needs the vendor header at compile
│   │       │                             time, unlike Oclgrind's split design)
│   │       ├── AMDDbgApiSession.h/.cpp   ✅ hardware-verified: fork/PTRACE_TRACEME/execv + full
│   │       │                             amd_dbgapi_callbacks_s impl (real int3 breakpoint injection via
│   │       │                             /proc/<pid>/mem -- for the library's OWN host-side tracking
│   │       │                             breakpoints, not GPU ones) + ptrace wait-loop + wave
│   │       │                             discovery/halt/PC/register read. See §5b.
│   │       ├── AMDBackend.h/.cpp         ⚠️ Backend impl: launch/detach/resume/read_global_memory real;
│   │       │                             set_breakpoint/step_over/step_in NOT implemented (open question,
│   │       │                             see header); select_work_item not lane-precise
│   │       ├── AMDLocationBackend.h      interface decl; impl at bottom of AMDBackend.cpp, DWARF->register
│   │       │                             path wired but unverified on hardware
│   │       └── AMDSession.h/.cpp         ✅ run_amd_session() — used by --dry-run --backend amd
│   │
│   ├── dap/
│   │   └── DAPServer.h/.cpp             ✅ REAL now — see §1. Header's own doc comment is stale.
│   │
│   ├── dwarf/
│   │   ├── DWARFSourceModel.h/.cpp      ✅ LLVM DWARF: pc_to_source, source_to_pcs,
│   │   │                                   variables_in_scope, type_name
│   │   └── LocationExprEval.h/.cpp      ❌ dead stub, never instantiated
│   │
│   └── ocl_debug_model/
│       ├── OCLAddressSpaces.h/.cpp      ✅ NEW — real __global/__local/__constant/__private
│       │                                   recovery from pocl's cached bitcode metadata
│       └── OCLVariableResolver.h/.cpp   ✅ DWARF vars + address spaces + LocationBackend +
│                                            LLDB-frame fallback; ctor now (dwarf, address_spaces)
│
├── runtime_inject/               ❌ dead/superseded LD_PRELOAD shim, builds libocldbg_rt.so
│   ├── wg_dispatch_hook.cpp      no real interposition (commented example only)
│   ├── wi_state_export.h/.cpp    shm header + no-op stub functions
│
├── examples/                     RENAMED from tests/kernels/ + tests/host_runner.c, and grown
│   ├── host_runner.c             ✅ major rewrite (361→687 lines): flexible CLI
│   │                             (--global-size/--local-size/--kernel/--file/--buffers/
│   │                             --arg-buf/--arg-int/--arg-float) PLUS auto-detects launch
│   │                             params from a `// FLAGS: ...` header comment in the kernel
│   │                             source if no CLI args given. See §7.1.
│   └── kernels/                  vector_add.cl (was hello_kernel.cl), reduction_sum.cl (was
│                                 reduction_bug.cl, same off-by-one bug), matrix_multiply.cl
│                                 (was matrix_mul.cl), + NEW: orbital_telemetry.cl,
│                                 particle_physics.cl, satellite_telemetry.cl
│
├── tests/                        see §8 for what each RUN line actually checks
│   ├── cli/                      cpp_passthrough.cpp, ocl_break.cl, ocl_workitem.cl
│   ├── cpu_backend/               variable_inspection.cl, address_spaces.cl (NEW)
│   ├── dap/                      dap_driver.py + dap_session.cl (NEW, real, passing)
│   ├── oclgrind_backend/breakpoint_stepping.cl   (REQUIRES: oclgrind)
│   ├── pocl_debug_verify/        pocl_ir.cl, reduction_ir.cl (M1 DWARF-survival checks)
│   ├── workgroup_tracking/workgroup_bounds.cl
│   ├── integration/test_breakpoint.py   ❌ dead stub, superseded by tests/dap/ — see §1
│   ├── lit.cfg.py / lit.site.cfg.py.in  LIT harness config; `config.excludes` lists dirs with
│   │                                   no RUN: lines (kernels, integration, and — on this
│   │                                   branch only — amd_backend)
│   └── amd_backend/              UNMERGED (PR #25). HIP kernels for `ocldbg --backend amd`,
│       │                         NOT lit-wired (no HIP language in CMake; needs real AMD GPU).
│       │                         Manual: build with `hipcc`, run
│       │                         `./build/ocldbg --backend amd --dry-run <bin>`.
│       ├── spin_kernel.cpp       correct kernel, busy-loops for an observable halt window
│       └── oob_write_kernel.cpp  GPU analog of reduction_sum.cl's bug: wrong index stride
│                                 walks out of bounds -> real hipErrorIllegalAddress. ocldbg's
│                                 AMD backend correctly reports MEMORY_VIOLATION, verified live.
│
└── scripts/
    ├── build.sh                  cmake configure+build wrapper (Debug default)
    └── demo.sh                   ⚠️ just builds + prints `ocldbg` usage, not a full demo
```

## 3. Core data types (`include/ocldbg/Types.h`, `OCLWorkItem.h`, `LocationBackend.h`)

```
Size3 { x,y,z: size_t }                          .str() -> "(x,y,z)"; has std::formatter
WorkGroupBound { group_id, min_wi, max_wi, item_count }
WorkItemMapping { global_id, group_id, local_id }
KernelLaunchInfo { kernel_name: string, work_dim: size_t = 1, global_size, local_size,
                   num_groups, work_groups[], sample_work_items[] }   // kernel_name/work_dim are new
SourceLocation { file: string, line: unsigned }
HostAddress = uint64_t
ExecCtxHandle = void*   // backend-specific: CPUExecContext* | OclgrindExecContext* | AMDExecContext*

OCLWorkItem { global_id, local_id, group_id, work_dim: size_t = 0 /* NEW, 0=unset */,
              exec_ctx: ExecCtxHandle, exec_ctx_storage: shared_ptr<void> }
  .valid() -> exec_ctx != nullptr
  .str() -> "WI(x,y,z) grp(x,y,z)"
OCLStopContext { stopped: OCLWorkItem, visible: vector<OCLWorkItem> }

VarValue { name, type_name, value_str, address_space ("__global"/"__local"/"__private"/""), available }
VarInfo  { name, type_name, address_space, location_pc, dwarf_location_expr: vector<uint8_t> }
```

`CPUExecContext` (backends/cpu/CPUExecContext.h): `{ host_thread_id, lldb::SBFrame frame, lldb::SBThread thread }`
`OclgrindExecContext` (backends/oclgrind/OclgrindBackend.h): `{ Size3 global_id, oclgrind_proto::LineChannel* channel }`
`AMDExecContext` (backends/amd/AMDBackend.h, **unmerged**): `{ amd_dbgapi_wave_id_t wave, AMDDbgApiSession* session }`
`AMDWaveInfo` (backends/amd/AMDDbgApiSession.h, **unmerged**): `{ amd_dbgapi_wave_id_t wave, amd_dbgapi_global_address_t pc, uint32_t stop_reason }` — `stop_reason` is the raw `AMD_DBGAPI_WAVE_STOP_REASON_*` bitmask; 0 means ocldbg's own `wave_stop()` halted it, nonzero means the hardware did (e.g. `MEMORY_VIOLATION`).

## 4. Interfaces / abstract classes

**`Backend`** (`include/ocldbg/Backend.h`) — pure virtual, implemented by `PoclCPUBackend` (real, wraps `DebuggerContext`), `OclgrindBackend` (real), `AMDBackend` (real except set_breakpoint/step_over/step_in, unmerged):
`launch, detach, set_breakpoint, remove_breakpoint, on_stop, resume, step_over, step_in, select_work_item, location_backend, read_global_memory`

**`LocationBackend`** (`include/ocldbg/LocationBackend.h`) — one method: `evaluate(VarInfo, ExecCtxHandle) -> VarValue`.
Implementations: `CPULocationBackend` (impl in `PoclCPUBackend.cpp`), `OclgrindLocationBackend` (impl at bottom of `OclgrindBackend.cpp`), `AMDLocationBackend` (impl at bottom of `AMDBackend.cpp`, unmerged, DWARF→register path unverified).

**`CPUABI`** (`backends/cpu/CPUABI.h/.cpp`) — **restructured this cycle**: most logic is now a shared, non-virtual base implementation, not a pure-virtual interface:
```
non-virtual, shared, implemented in CPUABI.cpp:
  read_enqueue_ndrange(process, frame, &global, &local, *work_dim=nullptr)
  read_enqueue_kernel_name(process, frame, &kernel_name)   // NEW
  read_workgroup_id(frame, &wg)
  static read_register(frame, reg64, reg32, &found)         // protected helper
  static read_local_id_from_variables(frame, local_size, &local_id)  // protected helper,
                                                              reads kernel's own debug-info locals
pure virtual, per-architecture:
  read_local_id(frame, local_size, &local_id)
  dwarf_register_name(dwarf_regnum) -> const char*           // arch's DWARF reg numbering -> LLDB name
  arg_registers() -> ArgRegisters (protected)                 // first-6-int-args reg64/reg32 tables
factory: create_host_abi() -> unique_ptr<CPUABI>              // picks X86_64CPUABI or AArch64CPUABI
```
Concrete impls: `X86_64CPUABI` (SysV), `AArch64CPUABI` (AAPCS64, x0-x4 for the workgroup-function args).

**`DebuggerContext`** (`include/ocldbg/DebuggerContext.h`, PIMPL, impl in `ocldbg/DebuggerContext.cpp` + `cli.cpp`) — the actual CPU-backend engine, now reachable both from the CLI *and* from `PoclCPUBackend` (which owns one as `impl_->dbg`). Key methods (all in `DebuggerContext.cpp` unless noted; **line numbers below are current as of this refresh — the file has grown/shifted since any older note**):
```
static init()/terminate()                    L322/327  lldb::SBDebugger::Initialize/Terminate
DebuggerContext()                             L331
launch(host_binary, args, stop_at_entry=false) L343     CreateTarget + bp on clEnqueueNDRangeKernel + Launch;
                                                        stop_at_entry is NEW (PoclCPUBackend passes true)
infer_kernel_launch()                         L376      reads NDRange + kernel_name + work_dim via CPUABI
set_workgroup_breakpoint(kernel_name="")      L424      byte-pattern scan of libpocl-devices-pthread.so;
                                                        empty kernel_name now defaults sensibly (was required)
track_workgroup_dispatches(...)               L469      main Continue()-loop: records WG hits, handles
                                                        line breakpoints, optional first-dispatch var inspect
set_source_breakpoint(line)                   L528      DWARF source_to_pcs -> BreakpointCreateByAddress
load_kernel_dwarf()                           L553      finds kernel .so module, loads DWARFSourceModel
ensure_ocl_trampoline()                       L574      regex bp ".*_workgroup" that opportunistically
                                                        resolves pending breakpoints + records WG on any hit
select_work_item(global_id)                   L659      finds LLDB thread for target WG, builds OCLWorkItem
get_selected_work_item()/list_stopped_work_items()  L710/714
add_ocl_breakpoint/delete_ocl_breakpoint/list_ocl_breakpoints/resolve_pending_ocl_breakpoints  L781-...
inspect_variables(wi)                         L888      OCLVariableResolver(dwarf, address_spaces).resolve()
inspect_current_frame_variables()             L895      resolves current thread -> WI -> inspect_variables
get_variable_value(name)                      L949
resolve_stopped_work_item(thread_id)          L959      wg_tracker + WIContextExtractor
continue_execution()                          L1056     NEW — Continue() + report halted/exited, used by
                                                        PoclCPUBackend::resume()
read_memory(addr, buf, length)                L1102     NEW — process.ReadMemory wrapper, used by
                                                        PoclCPUBackend::read_global_memory()
terminate_process()                           L1110
redirect_output_to_stderr()                   L1116     NEW — used in DAP mode so stdout stays clean for
                                                        the DAP wire protocol
run_dap(backend_name, port)                   L1124     constructs Backend (oclgrind/amd-if-built/cpu) +
                                                        DWARFSourceModel + OCLAddressSpaces + resolver + DAPServer
run_cli(config)                               cli.cpp:75  interactive/batch loop, "ocl" cmd interception
```
`DebuggerContext::Impl` fields (`DebuggerContextImpl.h`): `debugger, target, process, wg_breakpoint, line_breakpoint, launch_info, kernel_name, wg_tracker, abi (CPUABI::create_host_abi()), dwarf_model, ocl_breakpoints[], next_ocl_bp_id, ocl_trampoline_bp, selected_work_item`.

## 5. Oclgrind IPC protocol (`backends/oclgrind/OclgrindProtocol.h`)

Unchanged this cycle. Newline-framed text protocol over a Unix domain socket (`LineChannel`, blocking send/recv). Debugger process = client; injected `DebugPlugin` in the host process = server, blocks host execution while serving.

```
plugin -> ocldbg   ready
                   stopped <line> <gx> <gy> <gz> <lx> <ly> <lz> <bx> <by> <bz>
ocldbg -> plugin   break <id> <line>       -> ok
                   delete <id>             -> ok
                   select <gx> <gy> <gz>   -> ok <lx> <ly> <lz> <bx> <by> <bz>
                   print <expr>            -> value <text> | err <reason>
                   read <addr> <len>       -> data <hex> | err <reason>
                   continue                -> (no reply; plugin resumes)
                   step in | step over     -> (no reply; plugin resumes)
```
Socket path env var: `OCLDBG_OCLGRIND_SOCKET`. Host process launched with `LD_PRELOAD=liboclgrind-rt.so`, `OCLGRIND_PLUGINS=libocldbg_oclgrind_plugin.so` (overridable via `OCLDBG_OCLGRIND_PLUGIN` / `OCLDBG_OCLGRIND_LIBDIR`).

`DebugPlugin` internals: `should_halt()` dedupes repeated-line hits per work-item index, 3 run modes (`kRunning`/`kStepIn`/`kStepOver`), `isThreadSafe() = false` (deterministic WI order). Variable read captures `WorkItem::printExpression()`'s stdout via `streambuf` swap (no structured value-return API).

## 5b. AMD backend (unmerged, PR #25): amd-dbgapi callback contract (`backends/amd/AMDDbgApiSession.cpp`)

Unlike Oclgrind (a cooperating in-process plugin) or pocl (host threads under
LLDB/ptrace directly), amd-dbgapi is a **passive library that does no process
control itself** — it tells the client what to do via `amd_dbgapi_callbacks_s`
and the client (`AMDDbgApiSession`) must actually do it via raw ptrace:

```
amd_dbgapi_callbacks_s (all implemented as AMDDbgApiSession private statics,
since they're plain C function pointers with no `this`; client_process_id is
always `impl_.get()`, cast back inside each callback):
  allocate_memory / deallocate_memory     malloc/free
  client_process_get_info                 answers OS_PID; declines CORE_STATE
  insert_breakpoint / remove_breakpoint   REAL int3 (0xCC) patch over
                                           /proc/<pid>/mem, save/restore
                                           original byte -- this is for the
                                           LIBRARY's own host-side tracking
                                           breakpoints (HSA runtime load,
                                           code-object load), NOT a "set a
                                           breakpoint in my kernel" API
  xfer_global_memory                      pread/pwrite over /proc/<pid>/mem;
                                           works for both host AND GPU-mapped
                                           (__global buffer) addresses since
                                           HSA unifies the address space
  log_message                             stderr

mem_fd_for(Impl&) is ALSO a private static member (not a free function) even
though it's not a dbgapi callback -- Impl is a private nested type, and only
class members can name it from outside the class body. Impl itself is kept a
pure data aggregate (no member functions) specifically so
misc-non-private-member-variables-in-classes doesn't fire on its public
fields -- that check only objects to public data mixed with behavior.
```

Process control: fork + `PTRACE_TRACEME` + execv, `PTRACE_SETOPTIONS` with
`PTRACE_O_TRACECLONE|PTRACE_O_EXITKILL`, then `amd_dbgapi_initialize` +
`amd_dbgapi_process_attach`. `handle_stop()` runs the standard software-
breakpoint dance (rewind RIP, restore byte, singlestep, re-arm) and calls
`amd_dbgapi_report_breakpoint_hit()`.

`resume_until_wave_stop()` is split into three private helpers (originally
one function, refactored to satisfy clang-tidy's cognitive-complexity check):
`drain_pending_events()` (pumps the dbgapi event queue), `refresh_known_waves()`
(agent/queue/wave list queries), `scan_waves_for_stop(AMDWaveInfo&)` (per-wave
state polling + proactive `wave_stop()`).

**The trap that cost real debugging time:** `amd_dbgapi_process_agent_list` /
`_queue_list` / `_wave_list` are **delta/changed-tracking** APIs — if the set
is unchanged since your last call, they return `count=0`, `array=NULL`
(confirmed in the header's doc comment for `agent_list`), *not* "nothing
exists". `refresh_known_waves()` therefore retains `Impl::known_waves` and
only replaces it when the call reports `AMD_DBGAPI_CHANGED_YES`; per-wave
state polling then runs against the retained handles every loop iteration,
independent of whether that iteration's list call reported a change.
Querying `wave_list` alone every iteration without this caching silently
reports zero waves forever, even with a kernel genuinely executing on the
GPU (confirmed via `rocm-smi --showpids` showing the process actively using
VRAM the whole time).

`scan_waves_for_stop()` proactively calls `amd_dbgapi_wave_stop()` on the
first non-stopped wave it finds (`Impl::wave_stop_requested` gates this to
once); the resulting `AMDWaveInfo::stop_reason` is 0 for that user-requested
halt, or a real `AMD_DBGAPI_WAVE_STOP_REASON_*` bitmask if the hardware
stopped the wave itself first (e.g. `MEMORY_VIOLATION`, verified against
`tests/amd_backend/oob_write_kernel.cpp`).

**clang-tidy pitfalls hit and fixed in this file** (project's `.clang-tidy`
has `WarningsAsErrors: '*'` over broad check groups — see §6): the
int-as-opaque-pointer idiom (`reinterpret_cast<T*>(intptr_t)`) needed to be
pulled into small dedicated helper functions (`client_thread_for()`) with
`NOLINTBEGIN`/`NOLINTEND` blocks, not `NOLINTNEXTLINE` — the latter anchors
to whatever line clang-format wraps the actual flagged token onto, which is
NOT reliably "the next line" once a statement wraps across lines. Also:
`run-clang-tidy` (the real driver `tidy`/CI use) and bare `clang-tidy file --`
give **different** results for pre-existing issues in included headers
outside the file being analyzed (e.g. `Size3` in `Types.h`) — trust
`run-clang-tidy -p <build-dir> '<file>\.cpp$'` output, not bare `clang-tidy`,
when deciding whether something is really your problem to fix.

## 6. Build system (`CMakeLists.txt`)

Targets:
- `ocldbg_rt` (SHARED) — `runtime_inject/*.cpp` → `libocldbg_rt.so` (dead code, still builds)
- `ocldbg_oclgrind_plugin` (SHARED, conditional on `OCLGRIND_ROOT`/found `oclgrind/Plugin.h`+lib) — `ocldbg/backends/oclgrind/plugin/DebugPlugin.cpp`, built with `-fno-rtti`
- `ocldbg_core` (STATIC) — everything except `main.cpp` and the oclgrind plugin/runtime_inject; now includes `ocldbg/backends/cpu/CPUABI.cpp`, `ocldbg/backends/cpu/aarch64/AArch64CPUABI.cpp`, `ocldbg/ocl_debug_model/OCLAddressSpaces.cpp`. Links `Threads`, LLVM libs, `LLDB_LIB`, and (unmerged branch) `AMD_DBGAPI_LIB` **only inside an `if (OCLDBG_AMD_SOURCES)` guard** — linking a `find_library()` result unconditionally is a real CMake footgun: when not found it's literally the string `AMD_DBGAPI_LIB-NOTFOUND`, and `target_link_libraries` treats that as a hard configure error, not a no-op. AMD backend sources are compile-time-gated the same way (unlike Oclgrind's plugin-only split, AMD's `.cpp` files `#include <amd-dbgapi/amd-dbgapi.h>` directly and cannot compile at all without the SDK)
- `ocldbg` (EXECUTABLE) — `main.cpp` + `ocldbg_core`
- `test_host_runner` (conditional on OpenCL found) — now `examples/host_runner.c` (was `tests/host_runner.c`)
- `runtests` / `check-lit` — runs `lit -v tests/` (needs LLVM_LIT_BIN + FILECHECK_BIN + OpenCL)
- `format` / `check-format` — clang-format over include/, ocldbg/, runtime_inject/, tests/*.c — **CMakeLists.txt itself is never in this glob; never run clang-format on it directly, it will mangle `#comment` lines into invalid-looking CMake (clang-format doesn't understand CMake syntax)**
- `tidy` — `run-clang-tidy` over compile_commands.json, `WarningsAsErrors: '*'` — see §5b for real pitfalls hit fixing this

Key CMake cache vars: `OCLGRIND_ROOT` (Oclgrind install; enables the plugin + backend). `AMD_DBGAPI_ROOT` (unmerged branch; defaults to `/opt/rocm`, auto-detects on ROCm machines; CMake package name is `amd-dbgapi` but the actual `.so` is `librocm-dbgapi.so` — `find_library(... NAMES rocm-dbgapi ...)`, easy to get backwards). Compile definitions: `OCLDBG_GIT_COMMIT`, `OCLDBG_VERSION`, `OCLDBG_OCLGRIND_PLUGIN_PATH`, `OCLDBG_OCLGRIND_LIB_DIR`, `OCLDBG_HAVE_AMD_DBGAPI` (unmerged; guards `#ifdef` call sites in `main.cpp`/`DebuggerContext.cpp`).

CI (`.github/workflows/ci.yml`) is now a **2×2-ish matrix**: a lint job (format + tidy, single arch) and a `Build & Test` job that runs once for **x86_64** and once for **aarch64** (cross-compiled/emulated presumably — check the workflow if this matters) — this is new since the aarch64 backend landed; don't assume only one architecture is validated.

Build artifacts land in `build/`: `ocldbg`, `libocldbg_rt.so`, `libocldbg_oclgrind_plugin.so` (if Oclgrind found), `libocldbg_core.a`, `test_host_runner`.

## 7. CLI surface (`ocldbg/args.h`, `main.cpp`)

`ocldbg [options] [host_binary] [args...]`. Run modes chosen in `main.cpp:main()`:
1. `--dry-run` → `run_dry_run()` — oclgrind backend uses `run_oclgrind_session()`; amd backend (unmerged) uses `run_amd_session()` (`#ifdef OCLDBG_HAVE_AMD_DBGAPI`, else prints "built without AMD GPU support"); cpu backend uses `DebuggerContext` directly
2. `--dap` or `--port <N>` → `DebuggerContext::run_dap()` — **now real** (was a stub throw before this cycle). Backend selection: `"oclgrind"` / `"amd"` (unmerged, guarded) / else cpu-default (`PoclCPUBackend`)
3. else → `DebuggerContext::run_cli()` — LLDB-passthrough REPL + `ocl` subcommands (CPU-only)

`ocl` subcommands (`CustomCommands.cpp`): `ocl break [<file>:]<line> | list | delete <id>`, `ocl print|p <name>`, `ocl vars|v`, `ocl select|workitem|wi [<gx> [<gy> [<gz>]]] | list`.

Flags: `-o/-O` (one-line after/before), `-s/-S` (source file after/before), `-b/--batch`, `--dry-run`, `--show-wg-bounds`, `--track-wg`, `--inspect-vars`, `--break-at <line>`, `--break-for <n>`, `--print <expr>` (oclgrind only), `--backend <cpu|oclgrind|amd>` (default cpu), `--dap`, `--port <n>`, `-v/--version`, `-h/--help`.

### 7.1 `examples/host_runner.c` (moved+rewritten from `tests/host_runner.c`)
Now a general-purpose host runner, not hardcoded to 3 kernel functions: `--global-size/-g_size/-g`, `--local-size/-l_size/-l`, `--kernel/-k`, `--file/-f`, `--buffers <N>` (allocate N float buffers sized to total global work), `--arg-buf <n_floats>`, `--arg-int`/`--arg-float`. If no CLI flags are given, it looks for a `// FLAGS: -k <kernel> -g_size <...> ...` (or `// FLAGS[kernel_name]: ...`) header comment in the kernel source and uses that instead — this is how the new `examples/kernels/*.cl` (orbital_telemetry, particle_physics, satellite_telemetry) self-describe their launch parameters. Errors out if neither is present.

## 8. Test → coverage map

| File | RUN checks |
|---|---|
| `tests/cli/cpp_passthrough.cpp` | plain LLDB commands (`b`, `r`, `p`, `c`) work on a non-OpenCL C++ binary via `-b -o` batch flags |
| `tests/cli/ocl_break.cl` | `ocl break`/`list`/`delete`, `ocl print`, `ocl vars` on `reduce_sum` kernel |
| `tests/cli/ocl_workitem.cl` | `ocl wi list`/`select`, per-WI variable values differ by selection |
| `tests/cpu_backend/variable_inspection.cl` | `--dry-run --track-wg --inspect-vars` and `--break-at/--break-for` value evolution |
| `tests/cpu_backend/address_spaces.cl` | **new** — verifies `OCLAddressSpaces` correctly annotates `__global`/`__local`/`__constant` kernel arguments |
| `tests/dap/dap_session.cl` (+ `dap_driver.py`) | **new** — real DAP smoke test over the actual `ocldbg --dap` wire protocol |
| `tests/oclgrind_backend/breakpoint_stepping.cl` | `REQUIRES: oclgrind`; `--backend oclgrind --dry-run --break-at --print` |
| `tests/pocl_debug_verify/pocl_ir.cl`, `reduction_ir.cl` | pocl's `program.bc`/`parallel.bc` retain `!dbg` after `POCL_EXTRA_BUILD_FLAGS="-g -cl-opt-disable"` |
| `tests/workgroup_tracking/workgroup_bounds.cl` | `--show-wg-bounds`/`--track-wg` across 1D/3D kernels |
| `tests/integration/test_breakpoint.py` | dead stub, all `skipTest` — ignore, see `tests/dap/` instead |
| `tests/amd_backend/oob_write_kernel.cpp` (unmerged) | manual (not lit-wired): `--backend amd --dry-run` halts the wave and reports stop reason `MEMORY_VIOLATION` for a real out-of-bounds GPU write; verified live against real hardware |

Kernels (now in `examples/kernels/`, not `tests/kernels/`): `vector_add.cl`, `reduction_sum.cl` (**intentional off-by-one** `i<=n` should be `i<n` — the flagship CPU-backend debugging demo target), `matrix_multiply.cl`, plus `orbital_telemetry.cl`/`particle_physics.cl`/`satellite_telemetry.cl` (new, use `// FLAGS:` self-description). AMD-backend flagship bug (unmerged): `tests/amd_backend/oob_write_kernel.cpp`, same off-by-one-class bug, GPU analog.

## 9. Where `docs/PLANNING.md` disagrees with actual code

PLANNING.md is the design doc (research framing, milestones, open questions) — read it for **why**, not **what exists**. Known staleness:
- §3 "Component Map" lists files that don't exist: `OCLBreakpoint.h/cpp`, `OCLWorkItemSelector.h/cpp`, `plugin/OCLDebugPlugin.cpp`, `plugin/OCLBreakpointResolver.cpp`, `intel/`, `nvidia/` dirs. Treat §3 as aspirational only. (`amd/` now exists on the unmerged PR #25 branch, with different filenames than PLANNING guessed.)
- Frames Oclgrind (Milestone 6) as a future/stretch item; in the actual repo it was the **first** fully-complete backend.
- Frames DAP (Milestone 5) as a future milestone; **DAPServer is now real** (this cycle) — this is the single biggest way PLANNING.md is now out of date.
- §5/§8e frame the AMD backend as hardware-gated/far-future. The `amd-gpu-backend` dev sandbox used to build PR #25 actually has both a real GPU and ROCm — not something to assume elsewhere, but worth checking. `amd_dbgapi_dwarf_register_to_register` (real vendor API) handles the DWARF register mapping PLANNING.md worried about.
- §9 open-questions table already marks M1/M2's core risks "Resolved" — matches reality.
- Nowhere does PLANNING.md mention the aarch64 CPU backend, the `!kernel_arg_addr_space` address-space recovery approach, or the VS Code extension — all added without a corresponding PLANNING.md update.

## 10. Quick recipes

- Find where a CLI flag is parsed: `ocldbg/args.cpp` (`parse_flag_opts`/`parse_numeric_opts`).
- Find where a new `ocl <cmd>` should be added: `ocldbg/CustomCommands.cpp`, `handle_ocl_command()` dispatcher (bottom of file) + a new `handle_*_command()` helper above it.
- Find where CPU register/ABI assumptions live: `ocldbg/backends/cpu/CPUABI.cpp` (shared logic) + `x86_64/X86_64CPUABI.cpp` or `aarch64/AArch64CPUABI.cpp` (per-arch register tables and `dwarf_register_name`).
- Find where DW_OP_* opcodes are decoded for the CPU backend: anonymous namespace + free functions in `ocldbg/backends/cpu/PoclCPUBackend.cpp` (`eval_from_register`, `eval_from_memory`, `eval_from_breg`, `eval_from_fbreg`, `eval_const`, `eval_from_dwarf_expr`) — NOT `LocationExprEval` (dead).
- Find the Oclgrind wire protocol constants: `ocldbg/backends/oclgrind/OclgrindProtocol.h`.
- Find the real DAP message handlers: `ocldbg/dap/DAPServer.cpp` (ignore the stale TODO comment in `DAPServer.h`).
- Find how kernel argument address spaces are recovered: `ocldbg/ocl_debug_model/OCLAddressSpaces.cpp` (parses pocl's cached bitcode's `!kernel_arg_addr_space` metadata).
- To add a new execution backend: implement `Backend` (`include/ocldbg/Backend.h`) + a `LocationBackend`. `OclgrindBackend`, `PoclCPUBackend` (now), and `AMDBackend` (unmerged, partial) are all real templates now.
- (Unmerged AMD branch) Find the ptrace/dbgapi callback plumbing: `ocldbg/backends/amd/AMDDbgApiSession.cpp` — see §5b, especially the delta/changed-tracking trap and the clang-tidy NOLINT placement gotchas.
- (Unmerged AMD branch) To test without a full `ocldbg` build (e.g. LLDB not installed): `ocldbg/backends/amd/*.cpp` have no LLDB dependency; compile+link standalone against `-I include -I ocldbg -I <rocm>/include -L <rocm>/lib -lrocm-dbgapi`.
- To verify clang-tidy locally before pushing: generate `compile_commands.json` via a plain `cmake -B <dir> -DCMAKE_EXPORT_COMPILE_COMMANDS=ON` (configure-only is enough, doesn't need a working build), then `run-clang-tidy -p <dir> '<filename>\.cpp$'` — bare `clang-tidy file.cpp --` gives different (noisier, includes unrelated header issues) results than the real `run-clang-tidy` driver CI uses.
