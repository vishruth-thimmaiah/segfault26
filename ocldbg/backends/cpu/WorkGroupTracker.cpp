#include "WorkGroupTracker.h"

namespace ocldbg {

WorkGroupTracker::WorkGroupTracker() = default;
WorkGroupTracker::~WorkGroupTracker() = default;

void WorkGroupTracker::record_wg(uint64_t host_thread_id, const Size3 &wg_id) {
    std::scoped_lock lk(mu_);
    thread_to_wg_[host_thread_id] = wg_id;
}

void WorkGroupTracker::set_ndrange(const Size3 &global_size, const Size3 &local_size) {
    std::scoped_lock lk(mu_);
    global_size_ = global_size;
    local_size_ = local_size;
}

void WorkGroupTracker::clear() {
    std::scoped_lock lk(mu_);
    thread_to_wg_.clear();
}

bool WorkGroupTracker::connect_shm() {
    return true;
}

void WorkGroupTracker::refresh() {
    // Under Option A (LLDB-native tracking), work-group dispatches are tracked
    // dynamically through LLDB hooks.
}

uint64_t WorkGroupTracker::host_thread_for_wi(const Size3 &global_id) const {
    std::scoped_lock lk(mu_);
    size_t lx = (local_size_.x > 0) ? local_size_.x : 1;
    size_t ly = (local_size_.y > 0) ? local_size_.y : 1;
    size_t lz = (local_size_.z > 0) ? local_size_.z : 1;

    Size3 target_wg{.x = global_id.x / lx, .y = global_id.y / ly, .z = global_id.z / lz};

    for (const auto &[tid, wg] : thread_to_wg_) {
        if (wg == target_wg) {
            return tid;
        }
    }
    return 0;
}

Size3 WorkGroupTracker::wg_for_thread(uint64_t host_thread_id) const {
    std::scoped_lock lk(mu_);
    auto it = thread_to_wg_.find(host_thread_id);
    if (it != thread_to_wg_.end()) {
        return it->second;
    }
    return {.x = 0, .y = 0, .z = 0};
}

} // namespace ocldbg
