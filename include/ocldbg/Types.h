#pragma once
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace ocldbg {

/// Three-dimensional index used for global/local/group IDs.
struct Size3 {
    size_t x = 0, y = 0, z = 0;

    bool operator==(const Size3 &o) const { return x == o.x && y == o.y && z == o.z; }
    bool operator!=(const Size3 &o) const { return !(*this == o); }

    [[nodiscard]] std::string str() const {
        return "(" + std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(z) + ")";
    }
};

/// Represents the global work-item bounds and item count for a single work-group.
struct WorkGroupBound {
    Size3 group_id;
    Size3 min_wi;
    Size3 max_wi;
    size_t item_count = 0;
};

/// Represents the mapping of a global work-item coordinate to its group and local ID.
struct WorkItemMapping {
    Size3 global_id;
    Size3 group_id;
    Size3 local_id;
};

/// Summary of an inferred OpenCL kernel launch.
struct KernelLaunchInfo {
    Size3 global_size{1, 1, 1};
    Size3 local_size{1, 1, 1};
    Size3 num_groups{1, 1, 1};
    std::vector<WorkGroupBound> work_groups;
    std::vector<WorkItemMapping> sample_work_items;
};

/// A source-level location (file, 1-based line).
struct SourceLocation {
    std::string file;
    unsigned line = 0;
};

/// An address in the host process's virtual address space.
using HostAddress = uint64_t;

/// Opaque handle to a backend-specific execution context.
/// Each backend defines what this points to:
///   CPU     -> pointer to LLDB SBThread / SBFrame wrapper
///   Oclgrind -> pointer to Oclgrind WorkItem object
///   AMD     -> (wave_id, lane) pair
using ExecCtxHandle = void *;

} // namespace ocldbg

template <> struct std::formatter<ocldbg::Size3> : std::formatter<std::string_view> {
    auto format(const ocldbg::Size3 &s, std::format_context &ctx) const {
        return std::formatter<std::string_view>::format(s.str(), ctx);
    }
};
