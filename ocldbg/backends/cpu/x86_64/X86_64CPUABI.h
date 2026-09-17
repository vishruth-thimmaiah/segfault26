#pragma once
#include "backends/cpu/CPUABI.h"

namespace ocldbg {

/// Implements x86_64 (System V AMD64 ABI) conventions for pocl CPU kernel execution.
///
/// In pocl's x86_64 lowered work-group functions:
///   _pocl_kernel_<name>_workgroup(args, context, group_x, group_y, group_z)
/// Arguments are passed in:
///   - %rdi: args pointer
///   - %rsi: context pointer
///   - %rdx: group_x
///   - %rcx: group_y
///   - %r8:  group_z
///
/// Inside the kernel body, the work-item loop induction variable is held in
/// %rsi (basic loops) or %r10 (context-alloca loops), or exposed via DWARF
/// local variables (_local_id_x, gx).
class X86_64CPUABI : public CPUABI {
public:
    X86_64CPUABI() = default;
    ~X86_64CPUABI() override = default;

    bool read_local_id(lldb::SBFrame frame, const Size3 &local_size, Size3 &out_local_id) override;

    [[nodiscard]] const char *dwarf_register_name(unsigned dwarf_regnum) const override;

protected:
    [[nodiscard]] const ArgRegisters &arg_registers() const override;
};

} // namespace ocldbg
