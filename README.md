# ocldbg (segfault26) — Source-Level OpenCL Debugger

A source-level debugger for OpenCL kernels running on CPU device backends (`pocl`) and software GPU emulators (`Oclgrind`), providing standard debugger capabilities — breakpoints, stepping, and source-level variable inspection — without vendor-proprietary hardware driver hooks.

---

## 1. Prerequisites & Dependencies

### System Requirements
- Linux (x86_64 or AArch64)
- CMake 3.20+
- Modern C++20 compiler (`gcc` 11+ or `clang` 14+)
- OpenCL 1.2+ ICD loader and development headers (`ocl-icd`, `opencl-headers`)
- LLVM / LLDB development libraries: **LLVM 22.0+ minimum** (tested and verified on **LLVM 23**)
- PoCL (Portable Computing Language): **PoCL 7.0+ minimum** (**PoCL 7.1+ recommended** via `conda-forge` for modern LLVM compatibility)
- Oclgrind (for the emulator backend) — build from source, see below

### Package Installation

#### Arch Linux
```bash
sudo pacman -S base-devel cmake clang llvm lldb opencl-headers ocl-icd pocl
# Optional (from AUR for Oclgrind):
# yay -S oclgrind
```

#### Ubuntu / Debian (22.04 LTS+)
```bash
sudo apt update
sudo apt install -y build-essential cmake clang llvm lldb liblldb-dev \
                    opencl-headers ocl-icd-opencl-dev pocl-opencl-icd
```

#### Fedora (39+)
```bash
sudo dnf install -y gcc-c++ cmake clang llvm-devel lldb-devel \
                    opencl-headers ocl-icd-devel pocl
```

#### Conda / Mamba (PoCL 7.1+ & LIT)
```bash
mamba install -c conda-forge pocl lit
```

#### Oclgrind (emulator backend)

Build Oclgrind from source against the same LLVM as `ocldbg`. Distribution
packages are built against older LLVM releases, and the debugger plugin shares
Oclgrind's C++ ABI, so a mismatched pair will fail to load at run time.

```bash
git clone https://github.com/jrprice/Oclgrind.git
cmake -S Oclgrind -B Oclgrind/build \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=$HOME/.local/oclgrind \
      -DLLVM_DIR=/usr/lib/llvm-22/lib/cmake/llvm \
      -DCLANG_ROOT=/usr/lib/llvm-22
cmake --build Oclgrind/build -- -j4
cmake --install Oclgrind/build
```

---

## 2. Build Instructions

### Quick Build
Run the provided build script:
```bash
./scripts/build.sh Debug
```

### Standard CMake Build

1. **Configure:**
   ```bash
   cmake -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
   ```

   Add `-DOCLGRIND_ROOT=$HOME/.local/oclgrind` to build the Oclgrind backend's
   plugin. Without it the rest of the project still builds, and the Oclgrind
   backend reports that it was built without support when asked to launch.

2. **Compile:**
   ```bash
   cmake --build build -- -j$(nproc)
   ```

3. **Artifacts Generated in `build/`:**
   - `build/ocldbg` — The main debugger CLI executable and DAP server
   - `build/libocldbg_rt.so` — The runtime interposition shared library for pocl
   - `build/libocldbg_oclgrind_plugin.so` — Oclgrind plugin (only when Oclgrind was found)
   - `build/libocldbg_core.a` — Debugger core static library
   - `build/test_host_runner` — OpenCL host application for testing

---

## 3. Running & Verification

### Run CLI Usage
```bash
./build/ocldbg
```
Outputs:
```text
Usage: ocldbg [--backend cpu|oclgrind|accelerator] [--port <N>] <host_binary> [args...]
```

### Debugging on the Oclgrind Emulator

Oclgrind interprets the kernel inside the host program, so this path uses neither
LLDB nor ptrace. `ocldbg` injects its plugin, halts the interpreter on a kernel
source line, and reports one work-item per stop:

```bash
OCLDBG_BUILD_OPTIONS=-cl-opt-disable ./build/ocldbg --backend oclgrind --dry-run \
    --break-at 7 --break-for 3 --print gx \
    ./build/test_host_runner tests/kernels/hello_kernel.cl
```

```text
[ocldbg] Breakpoint hit at line 7 for WI(0,0,0) grp(0,0,0) (hit 1):
  gx = 0
[ocldbg] Breakpoint hit at line 7 for WI(1,0,0) grp(0,0,0) (hit 2):
  gx = 1
```

Oclgrind always compiles kernels with debug info and rejects `-g`, which is why
the build options are overridden above. Work-items run one at a time in a
deterministic order, so the reported sequence is reproducible.

### Debugging Through LLDB's Accelerator Plugin Framework (experimental)

LLVM's LLDB can debug an accelerator alongside the host process: an `lldb-server`
plugin has LLDB set breakpoints in the host, then connect a second target for the
accelerator. `--backend accelerator` drives that flow, sets a source breakpoint on
the accelerator, and maps its threads to work-items. It needs an `lldb-server` built
with an accelerator plugin, selected with `LLDB_DEBUGSERVER_PATH`. Only `--dry-run`
is supported, and stepping is not.

Two builds of LLDB have been used:

- **Upstream (LLDB 23 or later)** ships the framework and only a mock plugin
  (`-DLLDB_ENABLE_MOCK_ACCELERATOR_PLUGIN=ON`). It exercises the flow without a GPU.
- **The `llvm-server-plugins` branch of `clayborg/llvm-project`** has the AMD GPU
  plugin (`-DLLDB_ENABLE_AMDGPU_PLUGIN=ON -DROCM_PATH=/opt/rocm`). It uses an older
  protocol than upstream, so its `liblldb` and `lldb-server` must be used together.
  Verified on an RX 9060 XT (gfx1200) with ROCm 7.2, after two fixes to the plugin: it
  picked the first amd-dbgapi agent even when that was an unsupported integrated GPU,
  and it set its loader breakpoint by symbol name, which does not resolve in a
  stripped `libhsa-runtime64.so`.

```bash
LLDB_DEBUGSERVER_PATH=/path/to/lldb-server ./build/ocldbg --dry-run \
    --backend accelerator --break-file kernel.hip --break-at 26 --break-for 1 \
    --print idx ./a.out
```

```text
[ocldbg] Accelerator target amdgcn-amd-amdhsa--gfx1200 connected with 1 thread(s)
[ocldbg] Accelerator stopped (stop 1) with 64 thread(s)
[ocldbg]   WI(0,0,0) grp(0,0,0) in compute_element(Vec3 const*, ...) at kernel.hip:26
[ocldbg]     idx = 32
```

Known limits: work-item ids are the thread's position in LLDB's list, not NDRange
coordinates. Continuing past a breakpoint that many lanes hit does not finish in the
AMD plugin, which does not use amd-dbgapi's displaced stepping, so use `--break-for 1`
(a resume that does not finish is reported after `OCLDBG_ACCELERATOR_TIMEOUT_SECONDS`,
120 by default). Reading GPU memory needs the plugin branch's `SBProcess::ReadMemoryFromSpec`.

`tests/accelerator/` has an end-to-end test for each. They run only when
`OCLDBG_MOCK_ACCELERATOR_SERVER` (mock), or `OCLDBG_AMD_ACCELERATOR_SERVER` and
`OCLDBG_AMD_GPU_ARCH` (AMD, with `hipcc` and a GPU), are set.

### Run Host Test Target
Verify OpenCL platform and device discovery on your host:
```bash
./build/test_host_runner tests/kernels/hello_kernel.cl
```

### Running Tests (`runtests`)
Run the LLVM LIT test suite (verifying kernel lowering, LLVM IR passes, and DWARF debug info):
```bash
cmake --build build --target runtests
```
Or run `lit` directly:
```bash
lit -v tests/
```

### Run Automated Demo Script
```bash
./scripts/demo.sh
```

---

## 4. Code Formatting & Linting

### Formatting (`clang-format`)
```bash
# Format all C/C++ source files in place:
cmake --build build --target format

# Check formatting without modifying:
cmake --build build --target check-format
```

### Static Analysis (`clang-tidy`)
```bash
# Run clang-tidy on all targets using compile_commands.json:
cmake --build build --target tidy
```
