/**
 * spin_kernel.cpp — minimal HIP kernel for exercising the AMD backend.
 *
 * Not wired into the LIT suite (LIT/CMake here have no HIP language support
 * yet, and running it needs a real AMD GPU + ROCm/amd-dbgapi); this is a
 * manual verification target, same role hello_kernel.cl plays for the CPU
 * backend's DWARF checks but for `ocldbg --backend amd`.
 *
 * Build:  hipcc -O0 -g spin_kernel.cpp -o spin_kernel
 * Run:    ./build/ocldbg --backend amd --dry-run ./spin_kernel [iters]
 *
 * The kernel just busy-loops so there is an observable window with a live
 * GPU wavefront to halt; it does nothing incorrect. See oob_write_kernel.cpp
 * for the deliberately buggy companion.
 */
#include <cstdio>
#include <hip/hip_runtime.h>
#include <unistd.h>

__global__ void spin_kernel(volatile long long *counter, long long iters) {
    long long local = 0;
    for (long long i = 0; i < iters; ++i) {
        local += (i & 7);
        if ((i & 0xFFFFFF) == 0) {
            *counter = local;
        }
    }
    *counter = local;
}

int main(int argc, char **argv) {
    long long iters = (argc > 1) ? atoll(argv[1]) : 500000000LL;
    printf("[spin_kernel] pid=%d starting, iters=%lld\n", getpid(), iters);
    fflush(stdout);

    long long *d_counter = nullptr;
    hipError_t e = hipMalloc(&d_counter, sizeof(long long));
    if (e != hipSuccess) {
        printf("[spin_kernel] hipMalloc failed: %d\n", e);
        return 1;
    }

    hipLaunchKernelGGL(spin_kernel, dim3(1), dim3(64), 0, 0, d_counter, iters);
    printf("[spin_kernel] kernel launched, syncing...\n");
    fflush(stdout);
    e = hipDeviceSynchronize();
    printf("[spin_kernel] kernel done, err=%d (%s)\n", e, hipGetErrorString(e));
    return 0;
}
