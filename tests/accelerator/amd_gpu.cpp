// RUN: %hipcc -g -O0 --offload-arch=%amd_gpu_arch %s -o %t.exe
// RUN: env LLDB_DEBUGSERVER_PATH=%amd_accelerator_server %ocldbg --dry-run --backend accelerator --break-file amd_gpu.cpp --break-at 14 --break-for 1 --print idx --print not_a_variable %t.exe | %FileCheck %s
// RUN: env LLDB_DEBUGSERVER_PATH=%amd_accelerator_server %ocldbg --dry-run --backend accelerator %t.exe | %FileCheck %s --check-prefix=NOBREAK
// REQUIRES: amd-accelerator

#include <hip/hip_runtime.h>

#include <cstdio>

constexpr int kBlocks = 2;
constexpr int kLanes = 32;

__device__ __noinline__ void store(float *out, int idx) {
    out[idx] = static_cast<float>(idx) * 2.0F;
}

__global__ void kernel(float *out) {
    store(out, static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x));
}

int main() {
    float *device = nullptr;
    float host[kBlocks * kLanes] = {};
    if (hipMalloc(&device, sizeof(host)) != hipSuccess) {
        return 1;
    }
    kernel<<<dim3(kBlocks), dim3(kLanes)>>>(device);
    if (hipDeviceSynchronize() != hipSuccess) {
        return 1;
    }
    if (hipMemcpy(host, device, sizeof(host), hipMemcpyDeviceToHost) != hipSuccess) {
        return 1;
    }
    std::printf("kernel result: %g\n", static_cast<double>(host[kBlocks * kLanes - 1]));
    return 0;
}

// The accelerator connects when the GPU runtime loads, before the kernel runs.
// CHECK: [ocldbg] Accelerator target amdgcn-amd-amdhsa--{{gfx[0-9a-f]+}} connected with 1 thread(s)
// The breakpoint on the store in `store` is hit by every lane of the launch.
// CHECK: [ocldbg] Accelerator stopped (stop 1) with 64 thread(s)
// CHECK: in store({{.*}}) at amd_gpu.cpp:14
// CHECK: idx = {{[0-9]+}}
// CHECK: not_a_variable = <unavailable>
// CHECK: [ocldbg] Accelerator session completed with 1 stop(s).

// With no breakpoint the kernel runs to completion under the debugger.
// NOBREAK: kernel result: 126
// NOBREAK: [ocldbg] Host exited with status 0
// NOBREAK: [ocldbg] Accelerator session completed with 0 stop(s).
