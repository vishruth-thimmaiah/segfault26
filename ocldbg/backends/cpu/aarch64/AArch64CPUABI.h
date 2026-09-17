#pragma once
#include "backends/cpu/CPUABI.h"

namespace ocldbg {

/// Implements AArch64 (AAPCS64) conventions for pocl CPU kernel execution.
///
/// In pocl's AArch64 lowered work-group functions:
///   _pocl_kernel_<name>_workgroup(args, context, group_x, group_y, group_z)
/// Arguments are passed in:
///   - x0: args pointer
///   - x1: context pointer
///   - x2: group_x
///   - x3: group_y
///   - x4: group_z
///
/// Inside the kernel body the work-item loop induction variable is read from the
/// kernel's debug info; see read_local_id.
class AArch64CPUABI : public CPUABI {
public:
    AArch64CPUABI() = default;
    ~AArch64CPUABI() override = default;

    bool read_local_id(lldb::SBFrame frame, const Size3 &local_size, Size3 &out_local_id) override;

    [[nodiscard]] const char *dwarf_register_name(unsigned dwarf_regnum) const override;

protected:
    [[nodiscard]] const ArgRegisters &arg_registers() const override;
};

} // namespace ocldbg
