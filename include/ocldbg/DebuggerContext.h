#pragma once

#include "ocldbg/LocationBackend.h"
#include "ocldbg/OCLWorkItem.h"
#include "ocldbg/Types.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ocldbg {

/// Encapsulates LLDB initialization, target creation, launch orchestration,
/// NDRange call-site parameter inference, and DAP server handover.
class DebuggerContext {
public:
    /// Initialize global LLDB subsystem and return the LLDB version string.
    static std::string init();

    /// Terminate global LLDB subsystem.
    static void terminate();

    DebuggerContext();
    ~DebuggerContext();

    DebuggerContext(const DebuggerContext &) = delete;
    DebuggerContext &operator=(const DebuggerContext &) = delete;
    DebuggerContext(DebuggerContext &&) noexcept;
    DebuggerContext &operator=(DebuggerContext &&) noexcept;

    /// Launch the host binary with given arguments, intercepting at kernel dispatch.
    bool launch(const std::string &host_binary, const std::vector<std::string> &args);

    /// If halted at clEnqueueNDRangeKernel, inspect arguments and compute launch bounds.
    bool infer_kernel_launch();

    /// Set a breakpoint on `_pocl_kernel_<name>_workgroup` for live dispatch tracking.
    /// Must be called after infer_kernel_launch() and before track_workgroup_dispatches().
    /// Returns true if the breakpoint resolved to at least one location (i.e. the
    /// kernel .so is already loaded), false otherwise (will resolve lazily on first run).
    bool set_workgroup_breakpoint(const std::string &kernel_name);

    /// Resume the process and consume all _pocl_kernel_*_workgroup stop events,
    /// recording each stopped thread's work-group coordinates in WorkGroupTracker.
    /// If inspect_vars is true, inspects variables at the first work-group dispatch.
    /// Returns the number of work-group dispatch events recorded.
    /// Stops when the process exits, crashes, or hits a non-WG breakpoint.
    [[nodiscard]] size_t track_workgroup_dispatches(bool inspect_vars = false);

    /// Load DWARF information from the loaded kernel module, if available.
    bool load_kernel_dwarf();

    /// Inspect visible variables for a given work-item.
    [[nodiscard]] std::vector<VarValue> inspect_variables(const OCLWorkItem &wi);

    /// Given a stopped host thread ID, resolve the active OCLWorkItem by combining
    /// the WorkGroupTracker (WG coordinates) with WIContextExtractor (local ID).
    [[nodiscard]] std::optional<OCLWorkItem> resolve_stopped_work_item(uint64_t thread_id) const;

    /// Access the inferred launch parameters and bounds, if available.
    [[nodiscard]] const std::optional<KernelLaunchInfo> &kernel_launch_info() const;

    /// Terminate the debugee process (e.g., after a dry-run or when halting).
    void terminate_process();

    /// Hand over execution to the DAP server.
    int run_dap(const std::string &backend_name, uint16_t dap_port);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ocldbg
