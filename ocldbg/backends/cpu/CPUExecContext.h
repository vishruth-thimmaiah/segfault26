#pragma once
#include <cstdint>
#include <lldb/API/SBFrame.h>
#include <lldb/API/SBThread.h>

namespace ocldbg {

/// Execution context for a stopped pocl CPU work-item.
/// The ExecCtxHandle in OCLWorkItem points to one of these.
struct CPUExecContext {
    uint64_t host_thread_id = 0; ///< OS thread ID of the pocl worker thread
    lldb::SBFrame frame;         ///< LLDB SBFrame of the stopped work-item thread
    lldb::SBThread thread;       ///< LLDB SBThread of the stopped work-item thread
};

} // namespace ocldbg
