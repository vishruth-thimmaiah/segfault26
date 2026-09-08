#include "wi_state_export.h"

#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// TODO (Person C): Complete the shared memory synchronization and entry ring buffer.
// This is the stub implementation for compiling libocldbg_rt.so.

int ocldbg_shm_init(int create) {
    (void)create;
    return 0;
}

int ocldbg_shm_export_wg(uint64_t tid, uint32_t gx, uint32_t gy, uint32_t gz, uint32_t wgx,
                         uint32_t wgy, uint32_t wgz) {
    (void)tid;
    (void)gx;
    (void)gy;
    (void)gz;
    (void)wgx;
    (void)wgy;
    (void)wgz;
    return 0;
}
