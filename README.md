# ocldbg (segfault26) — Source-Level OpenCL Debugger

A source-level debugger for OpenCL kernels running on CPU device backends (`pocl`) and software GPU emulators (`Oclgrind`), providing standard debugger capabilities — breakpoints, stepping, and source-level variable inspection — without vendor-proprietary hardware driver hooks.

---

## 1. Prerequisites & Dependencies

### System Requirements
- Linux (x86_64)
- CMake 3.20+
- Modern C++20 compiler (`gcc` 11+ or `clang` 14+)
- OpenCL 1.2+ ICD loader and development headers (`ocl-icd`, `opencl-headers`)
- LLVM / LLDB development libraries (LLVM 15+ recommended; tested on LLVM 22)
- pocl (Portable Computing Language)
- Oclgrind (for emulator backend)

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

2. **Compile:**
   ```bash
   cmake --build build -- -j$(nproc)
   ```

3. **Artifacts Generated in `build/`:**
   - `build/ocldbg` — The main debugger CLI executable and DAP server
   - `build/libocldbg_rt.so` — The runtime interposition shared library for pocl
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
Usage: ocldbg [--backend cpu|oclgrind] [--port <N>] <host_binary> [args...]
```

### Run Host Test Target
Verify OpenCL platform and device discovery on your host:
```bash
./build/test_host_runner tests/kernels/hello_kernel.cl
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