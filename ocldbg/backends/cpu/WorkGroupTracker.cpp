#include "WorkGroupTracker.h"

// TODO (Person C): implement using POSIX shared memory.
//
// Shm layout (must match runtime_inject/wi_state_export.cpp):
//   struct ShmHeader { uint32_t entry_count; uint32_t capacity; };
//   struct ShmEntry  { uint64_t thread_id; uint32_t gx,gy,gz; uint32_t wgx,wgy,wgz; };
//
// The shim writes ShmEntry records atomically; this class reads them.

namespace ocldbg {

WorkGroupTracker::WorkGroupTracker() = default;
WorkGroupTracker::~WorkGroupTracker() {
    if (shm_fd_ >= 0) {
        // TODO: close(shm_fd_); shm_unlink(...)
    }
}

bool WorkGroupTracker::connect_shm() {
    // TODO (Person C): shm_open(OCLDBG_SHM_NAME, O_RDONLY, 0)
    //                  mmap it; store fd in shm_fd_
    return false;
}

void WorkGroupTracker::refresh() {
    // TODO (Person C): read ShmEntry records from mapped region;
    // rebuild thread_to_wg_
}

uint64_t WorkGroupTracker::host_thread_for_wi(const Size3 & /*global_id*/) const {
    // TODO (Person C): compute wg coord from global_id + NDRange dims,
    // then reverse-lookup thread from thread_to_wg_
    return 0;
}

Size3 WorkGroupTracker::wg_for_thread(uint64_t /*host_thread_id*/) const {
    std::scoped_lock lk(mu_);
    // TODO (Person C)
    return {};
}

} // namespace ocldbg
