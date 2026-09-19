#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace ocldbg {

struct AcceleratorSessionConfig {
    std::string host_binary;
    std::vector<std::string> host_args;
    std::string break_file;  ///< source file of the accelerator breakpoint
    unsigned break_at = 0;   ///< source line to break on (0 = no breakpoint)
    size_t break_for = 0;    ///< report this many stops (0 = every stop)
    std::string read_memory; ///< "<hex address>[:<bytes>]" read at connect and at each stop
    std::vector<std::string> print_exprs; ///< names (variables or registers) to read per thread
};

/// Run the host binary under LLDB with an accelerator plugin. Reports the
/// accelerator's threads once it connects, then each time the accelerator stops
/// on the breakpoint, until the host exits. The caller must have initialized
/// LLDB (DebuggerContext::init).
int run_accelerator_session(const AcceleratorSessionConfig &config);

} // namespace ocldbg
