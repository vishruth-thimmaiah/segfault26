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

/// Configuration for an interactive or batch CLI session.
struct CLIConfig {
    std::string host_binary;
    std::vector<std::string> host_args;
    std::vector<std::string> one_line_before;
    std::vector<std::string> source_before;
    std::vector<std::string> source_after;
    std::vector<std::string> one_line_after;
    bool batch = false;
};

/// Represents a registered OpenCL source breakpoint.
struct OCLBreakpoint {
    size_t id = 0;
    std::string file;
    unsigned line = 0;
    bool resolved = false;
    uint64_t address = 0;
};

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

    /// Resume the process and consume stop events.
    /// If inspect_vars is true, inspects variables at the first work-group dispatch.
    /// If break_at > 0, breaks at the specified kernel source line up to break_for times (or
    /// indefinitely if break_for == 0). Returns the number of work-group dispatch events recorded.
    /// Stops when the process exits, crashes, or hits a non-WG breakpoint.
    [[nodiscard]] size_t track_workgroup_dispatches(bool inspect_vars = false,
                                                    unsigned break_at = 0, size_t break_for = 0);

    /// Load DWARF information from the loaded kernel module, if available.
    bool load_kernel_dwarf();

    /// Set a source-line breakpoint in the loaded kernel module.
    bool set_source_breakpoint(unsigned line);

    /// Add an OpenCL breakpoint by file (optional) and line number.
    size_t add_ocl_breakpoint(const std::string &file, unsigned line);

    /// Delete an OpenCL breakpoint by ID. Returns true if removed.
    bool delete_ocl_breakpoint(size_t id);

    /// List all registered OpenCL breakpoints.
    [[nodiscard]] std::vector<OCLBreakpoint> list_ocl_breakpoints() const;

    /// Attempt to resolve any pending OpenCL breakpoints using currently loaded modules.
    bool resolve_pending_ocl_breakpoints();

    /// Ensure the internal kernel workgroup trampoline breakpoint is armed on the target.
    void ensure_ocl_trampoline();

    /// Inspect visible variables for a given work-item.
    [[nodiscard]] std::vector<VarValue> inspect_variables(const OCLWorkItem &wi);

    /// Inspect visible variables in the currently stopped frame.
    [[nodiscard]] std::vector<VarValue> inspect_current_frame_variables();

    /// Fetch a specific variable by name in the currently stopped frame.
    [[nodiscard]] std::optional<VarValue> get_variable_value(const std::string &name);

    /// Given a stopped host thread ID, resolve the active OCLWorkItem by combining
    /// the WorkGroupTracker (WG coordinates) with WIContextExtractor (local ID).
    [[nodiscard]] std::optional<OCLWorkItem> resolve_stopped_work_item(uint64_t thread_id) const;

    /// Access the inferred launch parameters and bounds, if available.
    [[nodiscard]] const std::optional<KernelLaunchInfo> &kernel_launch_info() const;

    /// Terminate the debugee process (e.g., after a dry-run or when halting).
    void terminate_process();

    /// Hand over execution to the DAP server.
    int run_dap(const std::string &backend_name, uint16_t dap_port);

    /// Run an interactive or batch CLI session, executing pre-session commands and
    /// passing interactive commands to LLDB (or handling 'ocl' commands).
    int run_cli(const CLIConfig &config = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ocldbg
