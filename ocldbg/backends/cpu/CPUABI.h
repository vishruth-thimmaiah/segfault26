#pragma once
#include "ocldbg/Types.h"

#include <array>
#include <lldb/API/SBFrame.h>
#include <lldb/API/SBProcess.h>
#include <memory>
#include <string>

namespace ocldbg {

/// Architecture-independent interface for CPU calling conventions and register ABI.
/// Handles extracting work-group arguments and work-item loop induction variables
/// from host machine frames.
///
/// pocl lowers every kernel to an ordinary host function, so reading work-group
/// state is a matter of decoding that function's arguments. Which registers carry
/// them is the only part that varies by architecture; a subclass supplies that
/// mapping and the loop induction variable, and inherits the rest.
class CPUABI {
public:
    virtual ~CPUABI() = default;

    /// Extract global-size and local-size dimensions from the arguments passed
    /// to clEnqueueNDRangeKernel at its call site.
    bool read_enqueue_ndrange(lldb::SBProcess process, lldb::SBFrame frame, Size3 &out_global,
                              Size3 &out_local, size_t *out_work_dim = nullptr);

    /// Extract the kernel name passed to clEnqueueNDRangeKernel at its call site.
    bool read_enqueue_kernel_name(lldb::SBProcess process, lldb::SBFrame frame,
                                  std::string &out_kernel_name);

    /// Extract work-group coordinates (group_x, group_y, group_z) from the
    /// entry frame of _pocl_kernel_<name>_workgroup according to ABI convention.
    bool read_workgroup_id(lldb::SBFrame frame, Size3 &out_wg);

    /// Extract work-item local coordinates (lx, ly, lz) from a frame stopped
    /// inside the kernel loop.
    virtual bool read_local_id(lldb::SBFrame frame, const Size3 &local_size,
                               Size3 &out_local_id) = 0;

    /// Name of the register that DWARF numbers as @p dwarf_regnum, or nullptr
    /// when the architecture assigns that number to nothing ocldbg can read.
    /// DWARF register numbering is defined per architecture by its psABI, so it
    /// does not follow the argument order above.
    [[nodiscard]] virtual const char *dwarf_register_name(unsigned dwarf_regnum) const = 0;

    /// Factory method returning the CPUABI appropriate for the current host
    /// architecture, or nullptr when ocldbg implements none for it.
    static std::unique_ptr<CPUABI> create_host_abi();

protected:
    /// Registers carrying the first six integer arguments of a call, in argument
    /// order, as 64-bit names and their 32-bit views. Six is all the call sites
    /// ocldbg decodes need, and both supported ABIs pass at least that many in
    /// registers.
    struct ArgRegisters {
        std::array<const char *, 6> reg64;
        std::array<const char *, 6> reg32;
    };

    [[nodiscard]] virtual const ArgRegisters &arg_registers() const = 0;

    /// Read @p reg64, falling back to @p reg32 when LLDB does not know the 64-bit
    /// name. Pass nullptr for @p reg32 to read a 64-bit value only.
    static uint64_t read_register(lldb::SBFrame frame, const char *reg64, const char *reg32,
                                  bool &found);

    /// Resolve a work-item's local id from the kernel's own debug info, which is
    /// where a kernel built with -cl-opt-disable keeps it. Returns false when no
    /// such variable is in scope, leaving @p out_local_id untouched.
    static bool read_local_id_from_variables(lldb::SBFrame frame, const Size3 &local_size,
                                             Size3 &out_local_id);
};

} // namespace ocldbg
