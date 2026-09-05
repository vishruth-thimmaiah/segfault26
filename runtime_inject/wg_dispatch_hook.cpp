/**
 * wg_dispatch_hook.cpp  —  LD_PRELOAD shim for pocl work-group dispatch
 *
 * Owner: Person C
 *
 * Goal: intercept pocl's work-group dispatch call so we know which
 * host thread is executing which work-group.
 *
 * IMPORTANT — validate before implementing:
 *   This hook assumes pocl's CPU driver calls a stable, exported function
 *   at work-group dispatch time that we can interpose via LD_PRELOAD.
 *   Inspect pocl source first:
 *     lib/CL/devices/cpu/cpu_cl.c (or similar) for the dispatch loop.
 *   If no stable symbol exists, fall back to a minimal pocl source patch.
 *
 * Shm layout (must match WorkGroupTracker.cpp):
 *   /ocldbg_wg_map  (POSIX shared memory)
 *   Header:  { uint32_t count; uint32_t capacity; }
 *   Entry[]: { uint64_t tid; uint32_t gx,gy,gz; uint32_t wgx,wgy,wgz; }
 */

#include "wi_state_export.h"
#include <pthread.h>
#include <stdint.h>
#include <string.h>

// TODO (Person C):
//   1. Find the right pocl symbol to interpose (e.g. pocl_run_kernel or
//      the pthread_create call inside the CPU driver's work-group loop).
//   2. Implement __ocldbg_register_wg() to write to the shm segment.
//   3. Interpose the target symbol with the real_ prefix pattern below.

static void __ocldbg_register_wg(uint64_t tid,
                                  uint32_t gx, uint32_t gy, uint32_t gz,
                                  uint32_t wgx, uint32_t wgy, uint32_t wgz) {
    ocldbg_shm_export_wg(tid, gx, gy, gz, wgx, wgy, wgz);
}

// Example interposition pattern (adapt to actual pocl symbol):
//
// #include <dlfcn.h>
// typedef void (*real_dispatch_t)(...);
// void pocl_target_dispatch_fn(...) {
//     static real_dispatch_t real_fn = NULL;
//     if (!real_fn) real_fn = dlsym(RTLD_NEXT, "pocl_target_dispatch_fn");
//     __ocldbg_register_wg(pthread_self(), gx, gy, gz, wgx, wgy, wgz);
//     real_fn(...);
// }
