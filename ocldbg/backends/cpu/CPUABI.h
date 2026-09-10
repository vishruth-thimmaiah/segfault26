#pragma once
#include "ocldbg/Types.h"

#include <lldb/API/SBFrame.h>
#include <lldb/API/SBProcess.h>
#include <memory>

namespace ocldbg {

/// Architecture-independent interface for CPU calling conventions and register ABI.
/// Handles extracting work-group arguments and work-item loop induction variables
/// from host machine frames.
class CPUABI {
public:
    virtual ~CPUABI() = default;

    /// Extract global-size and local-size dimensions from the arguments passed
    /// to clEnqueueNDRangeKernel at its call site.
    virtual bool read_enqueue_ndrange(lldb::SBProcess process, lldb::SBFrame frame,
                                      Size3 &out_global, Size3 &out_local) = 0;

    /// Extract work-group coordinates (group_x, group_y, group_z) from the
    /// entry frame of _pocl_kernel_<name>_workgroup according to ABI convention.
    virtual bool read_workgroup_id(lldb::SBFrame frame, Size3 &out_wg) = 0;

    /// Extract work-item local coordinates (lx, ly, lz) from a frame stopped
    /// inside the kernel loop.
    virtual bool read_local_id(lldb::SBFrame frame, const Size3 &local_size,
                               Size3 &out_local_id) = 0;

    /// Factory method returning the CPUABI appropriate for the current host architecture.
    static std::unique_ptr<CPUABI> create_host_abi();
};

} // namespace ocldbg
