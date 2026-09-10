#pragma once
#include "ocldbg/OCLWorkItem.h"
#include "ocldbg/Types.h"

#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace ocldbg {

/// Records the mapping: host thread ID -> work-group coordinate.
///
/// Owner: Person C
///
/// Populated by the LD_PRELOAD shim (libocldbg_rt.so) which intercepts
/// pocl's work-group dispatch and writes into a shared-memory segment.
/// This class reads that segment and keeps an in-process copy.
///
/// IMPORTANT: Whether LD_PRELOAD can hook pocl's dispatch site is an
/// open question (PLANNING.md §9 item 3). Validate experimentally before
/// implementing the shim. If LD_PRELOAD fails, a minimal pocl source patch
/// at lib/CL/devices/cpu/ dispatch is the fallback.
class WorkGroupTracker {
public:
    WorkGroupTracker();
    ~WorkGroupTracker();

    /// Record work-group mapping for a host thread (Option A: LLDB-native tracking)
    void record_wg(uint64_t host_thread_id, const Size3 &wg_id);

    /// Set current kernel NDRange dimensions to support reverse coordinate lookups
    void set_ndrange(const Size3 &global_size, const Size3 &local_size);

    /// Clear all active thread mappings
    void clear();

    /// Connect to the shared-memory segment written by the shim (legacy stub).
    /// Call after the host process is launched.
    bool connect_shm();

    /// Called when the LLDB stop event fires — refresh state.
    void refresh();

    /// Look up which work-group a given global_id belongs to and return
    /// the host thread ID executing that work-group.
    /// Returns 0 (invalid) if not found.
    uint64_t host_thread_for_wi(const Size3 &global_id) const;

    /// Return the work-group coordinate for a stopped host thread.
    /// Returns a zeroed Size3 if not found.
    Size3 wg_for_thread(uint64_t host_thread_id) const;

private:
    mutable std::mutex mu_;
    // host_thread_id -> work-group id
    std::unordered_map<uint64_t, Size3> thread_to_wg_;
    Size3 global_size_{0, 0, 0};
    Size3 local_size_{1, 1, 1};
    int shm_fd_ = -1;
};

} // namespace ocldbg
