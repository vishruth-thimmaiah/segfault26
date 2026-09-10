#pragma once

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
