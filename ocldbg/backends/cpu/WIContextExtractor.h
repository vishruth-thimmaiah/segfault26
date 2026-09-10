#pragma once
#include "CPUABI.h"
#include "ocldbg/OCLWorkItem.h"
#include "ocldbg/Types.h"

#include <cstdint>
#include <lldb/API/SBFrame.h>
#include <lldb/API/SBThread.h>
#include <memory>

namespace ocldbg {

/// Extracts the current work-item identity from a stopped host thread.
///
/// Uses platform-specific CPUABI (e.g. X86_64CPUABI on x86_64) to inspect
/// frame registers and DWARF variables.
class WIContextExtractor {
public:
    WIContextExtractor();
    explicit WIContextExtractor(std::unique_ptr<CPUABI> abi);
    ~WIContextExtractor();

    /// Given a stopped host thread frame, work-group coordinate, and NDRange local size,
    /// reconstruct the current OCLWorkItem.
    bool extract_from_frame(lldb::SBFrame frame, const Size3 &wg_id, const Size3 &local_size,
                            OCLWorkItem &out);

    /// Convenience overload taking an lldb::SBThread.
    bool extract_from_thread(lldb::SBThread thread, const Size3 &wg_id, const Size3 &local_size,
                             OCLWorkItem &out);

    /// Legacy interface using thread ID.
    bool extract(uint64_t host_thread_id, const Size3 &wg_id, const Size3 &local_size,
                 OCLWorkItem &out);

    CPUABI *abi() { return abi_.get(); }

private:
    std::unique_ptr<CPUABI> abi_;
};

} // namespace ocldbg
