#pragma once
#include "ocldbg/Types.h"
#include <string>
#include <vector>

namespace ocldbg {

/// The central debugger abstraction: one OpenCL work-item.
///
/// This struct is the shared currency across ALL backends. A CPU thread,
/// an Oclgrind WorkItem, an AMD wave lane, and a CUDA thread all map to
/// this single type. Backend-specific detail lives in exec_ctx only.
struct OCLWorkItem {
    Size3 global_id; ///< (gx, gy, gz)
    Size3 local_id;  ///< (lx, ly, lz) within the work-group
    Size3 group_id;  ///< work-group coordinate

    /// Backend-specific execution context handle.
    /// CPU:      pointer to a CPUExecContext (wraps LLDB SBFrame)
    /// Oclgrind: pointer to the Oclgrind WorkItem object
    /// AMD:      pointer to an AMDWaveLane struct
    ExecCtxHandle exec_ctx = nullptr;

    bool valid() const { return exec_ctx != nullptr; }

    std::string str() const {
        return "WI" + global_id.str() + " grp" + group_id.str();
    }
};

/// What the debugger actually has state for at a given halt point.
///
/// Do NOT expose the full NDRange as "threads" to DAP — a 1024x1024
/// NDRange produces over a million entries. Instead surface only the
/// work-items whose state is concretely accessible right now.
struct OCLStopContext {
    OCLWorkItem stopped;        ///< the WI at which execution halted
    std::vector<OCLWorkItem> visible; ///< other WIs accessible from this halt
                                      ///  (e.g. peers in the same work-group
                                      ///   / wave that the backend can read)
};

} // namespace ocldbg
