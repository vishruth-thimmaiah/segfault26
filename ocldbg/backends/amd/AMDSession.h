#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace ocldbg {

/// Plain config struct (no amd-dbgapi types) so this header stays includable
/// from translation units that aren't compiled with AMD_DBGAPI support,
/// mirroring OclgrindSessionConfig.
struct AMDSessionConfig {
    std::string host_binary;
    std::vector<std::string> host_args;
};

/// Run the host binary under amd-dbgapi and report the first halted GPU
/// wavefront. Uses neither LLDB nor pocl. There is no source-line breakpoint
/// support yet (see AMDBackend.h), so this halts whatever wave is running
/// once the runtime and a dispatch are up -- a "see what's actually
/// executing on the GPU" session, not a targeted one.
int run_amd_session(const AMDSessionConfig &config);

} // namespace ocldbg
