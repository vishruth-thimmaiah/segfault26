/**
 * oob_write_kernel.cpp — deliberately buggy HIP kernel, the GPU analog of
 * tests/kernels/reduction_bug.cl's off-by-one: a wrong index stride walks
 * each thread's write far past a small allocation, producing a real
 * hardware memory violation.
 *
 * Not wired into the LIT suite (see spin_kernel.cpp for why); this is a
 * manual verification target for `ocldbg --backend amd`.
 *
 * Build:  hipcc -O0 -g oob_write_kernel.cpp -o oob_write_kernel
 * Run:    ./build/ocldbg --backend amd --dry-run ./oob_write_kernel
 *
 * Verified end-to-end (segfault26 issue #12): running without a debugger,
 * the kernel crashes with hipErrorIllegalAddress (700). Running under
 * ocldbg's AMD backend, the halted wave's stop reason correctly reports
 * AMD_DBGAPI_WAVE_STOP_REASON_MEMORY_VIOLATION rather than a plain
 * user-requested halt -- ocldbg catching a real, known class of AMDGPU bug
 * (out-of-bounds __global__ access) via amd-dbgapi's hardware fault
 * reporting, not merely a source-line breakpoint (which this backend does
 * not implement yet -- see AMDBackend.h).
 */
#include <cstdio>
#include <hip/hip_runtime.h>
#include <unistd.h>

__global__ void oob_write_kernel(int *buf) {
    int i = threadIdx.x;
    // BUG: should be `buf[i] = i;` -- the stray `* 1000000` walks each
    // thread's write far past the small allocation.
    int idx = i * 1000000;
    buf[idx] = i;
}

int main() {
    printf("[oob_write_kernel] pid=%d starting\n", getpid());
    fflush(stdout);

    int *d_buf = nullptr;
    hipError_t e = hipMalloc(&d_buf, 64 * sizeof(int)); // only 64 ints allocated
    if (e != hipSuccess) {
        printf("[oob_write_kernel] hipMalloc failed: %d\n", e);
        return 1;
    }

    hipLaunchKernelGGL(oob_write_kernel, dim3(1), dim3(64), 0, 0, d_buf);
    printf("[oob_write_kernel] kernel launched, syncing...\n");
    fflush(stdout);
    e = hipDeviceSynchronize();
    printf("[oob_write_kernel] kernel done, err=%d (%s)\n", e, hipGetErrorString(e));
    return 0;
}
