#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace ocldbg {

/// Three-dimensional index used for global/local/group IDs.
struct Size3 {
    size_t x = 0, y = 0, z = 0;

    bool operator==(const Size3 &o) const {
        return x == o.x && y == o.y && z == o.z;
    }
    bool operator!=(const Size3 &o) const { return !(*this == o); }

    std::string str() const {
        return "(" + std::to_string(x) + "," + std::to_string(y) + "," +
               std::to_string(z) + ")";
    }
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
