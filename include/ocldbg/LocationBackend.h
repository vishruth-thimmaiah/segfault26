#pragma once
#include "ocldbg/Types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ocldbg {

/// A resolved source-level variable value.
struct VarValue {
    std::string name;
    std::string type_name;
    std::string value_str;     ///< human-readable representation
    std::string address_space; ///< "__global", "__local", "__private", ""
    bool available = false;    ///< false if location could not be resolved
};

/// Describes a variable known at a source location (before value lookup).
struct VarInfo {
    std::string name;
    std::string type_name;
    std::string address_space;
    HostAddress location_pc; ///< PC at which this variable is in scope
    // DWARF location expression bytes (to be evaluated by LocationBackend)
    std::vector<uint8_t> dwarf_location_expr;
};

/// Abstract interface for target-specific variable location evaluation.
///
/// One concrete implementation per backend:
///   CPULocationBackend     - evaluates DW_OP_* exprs via LLDB SBFrame
///   OclgrindLocationBackend - reads from Oclgrind's interpreter state
///   AMDLocationBackend     - reads VGPR/SGPR for a wave lane  [stretch]
///
/// Person B owns the interface; each backend owner implements their version.
class LocationBackend {
public:
    virtual ~LocationBackend() = default;

    /// Evaluate a DWARF location expression in the context of a specific
    /// work-item's execution context. Returns the resolved value string,
    /// or an empty VarValue with available=false on failure.
    virtual VarValue evaluate(const VarInfo &var, ExecCtxHandle exec_ctx) = 0;
};

} // namespace ocldbg
