#pragma once
#include <lldb/API/SBThread.h>

namespace ocldbg {

/// Execution context for one thread of an accelerator target.
/// The ExecCtxHandle in OCLWorkItem points to one of these.
struct AcceleratorExecContext {
    lldb::SBThread thread;
};

} // namespace ocldbg
