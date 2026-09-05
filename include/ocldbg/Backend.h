#pragma once
#include "ocldbg/LocationBackend.h"
#include "ocldbg/OCLWorkItem.h"
#include "ocldbg/Types.h"
#include <functional>
#include <memory>
#include <vector>

namespace ocldbg {

/// Called by the backend whenever execution halts (breakpoint, step-end).
using StopCallback = std::function<void(OCLStopContext)>;

/// Pure-virtual interface every execution backend must implement.
///
/// The three concrete backends are:
///   CPUBackend      (Person C) — pocl loops strategy + LLDB/ptrace
///   OclgrindBackend (Person D) — Oclgrind plugin adapter
///   [stretch] AMDBackend, IntelBackend, NvidiaBackend
///
/// Person A defines this interface. All other persons depend on it.
class Backend {
public:
    virtual ~Backend() = default;

    // ------------------------------------------------------------------ //
    //  Lifecycle
    // ------------------------------------------------------------------ //

    /// Launch the host program and attach the backend.
    /// @param host_binary  Path to the compiled OpenCL host executable.
    /// @param args         Arguments forwarded to the host program.
    /// @return true on success.
    virtual bool launch(const std::string &host_binary,
                        const std::vector<std::string> &args) = 0;

    /// Detach and clean up. Called on debugger exit or kernel completion.
    virtual void detach() = 0;

    // ------------------------------------------------------------------ //
    //  Breakpoints
    // ------------------------------------------------------------------ //

    /// Set a breakpoint at the given source location.
    /// One source line may expand to multiple PCs; the backend handles this.
    /// @return opaque breakpoint ID (0 on failure).
    virtual uint64_t set_breakpoint(const SourceLocation &loc) = 0;

    /// Remove a previously set breakpoint.
    virtual void remove_breakpoint(uint64_t bp_id) = 0;

    // ------------------------------------------------------------------ //
    //  Execution control
    // ------------------------------------------------------------------ //

    /// Register a callback to be invoked when the backend halts.
    virtual void on_stop(StopCallback cb) = 0;

    /// Resume execution after a halt.
    virtual void resume() = 0;

    /// Step over one source line in the context of the given work-item.
    virtual void step_over(const OCLWorkItem &wi) = 0;

    /// Step into the next instruction / source line.
    virtual void step_in(const OCLWorkItem &wi) = 0;

    // ------------------------------------------------------------------ //
    //  Work-item state
    // ------------------------------------------------------------------ //

    /// Make the given work-item the "selected" context for variable reads.
    /// Returns false if the WI's state is not accessible from this halt.
    virtual bool select_work_item(const Size3 &global_id,
                                  OCLWorkItem &out_wi) = 0;

    /// Return the LocationBackend for this execution target.
    /// Used by OCLVariableResolver to evaluate DWARF location expressions.
    virtual LocationBackend &location_backend() = 0;

    // ------------------------------------------------------------------ //
    //  Memory
    // ------------------------------------------------------------------ //

    /// Read @p length bytes from the address @p addr in the __global address
    /// space into @p buf. Returns the number of bytes actually read.
    virtual size_t read_global_memory(HostAddress addr,
                                      void *buf, size_t length) = 0;
};

} // namespace ocldbg
