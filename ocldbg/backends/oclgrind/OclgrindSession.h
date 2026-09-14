#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace ocldbg {

struct OclgrindSessionConfig {
    std::string host_binary;
    std::vector<std::string> host_args;
    unsigned break_at = 0;                ///< kernel source line to break on
    size_t break_for = 0;                 ///< report this many hits (0 = every hit)
    std::vector<std::string> print_exprs; ///< expressions to evaluate at each hit
};

/// Run the host binary under Oclgrind and report each breakpoint hit. Uses
/// neither LLDB nor ptrace; it drives OclgrindBackend directly.
int run_oclgrind_session(const OclgrindSessionConfig &config);

} // namespace ocldbg
