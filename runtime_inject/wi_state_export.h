#pragma once
#include <stdint.h>
#include <stddef.h>

#define OCLDBG_SHM_NAME "/ocldbg_wg_map"
#define OCLDBG_SHM_MAX_ENTRIES 1024

#ifdef __cplusplus
extern "C" {
#endif

/// Single work-group to host thread mapping entry
struct OcldbgShmEntry {
    uint64_t thread_id;
    uint32_t gx, gy, gz;       // Global size / offset
    uint32_t wgx, wgy, wgz;    // Work-group coordinate
};

/// Shared memory header
struct OcldbgShmHeader {
    uint32_t entry_count;
    uint32_t capacity;
};

/// Initialize or open the shared memory segment
int ocldbg_shm_init(int create);

/// Export work-group dispatch coordinate for the calling thread
int ocldbg_shm_export_wg(uint64_t tid,
                         uint32_t gx, uint32_t gy, uint32_t gz,
                         uint32_t wgx, uint32_t wgy, uint32_t wgz);

#ifdef __cplusplus
}
#endif
