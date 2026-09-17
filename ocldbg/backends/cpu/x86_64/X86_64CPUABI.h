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

    bool read_enqueue_ndrange(lldb::SBProcess process, lldb::SBFrame frame, Size3 &out_global,
                              Size3 &out_local) override;
    bool read_enqueue_kernel_name(lldb::SBProcess process, lldb::SBFrame frame,
                                  std::string &out_kernel_name) override;
    bool read_workgroup_id(lldb::SBFrame frame, Size3 &out_wg) override;
    bool read_local_id(lldb::SBFrame frame, const Size3 &local_size, Size3 &out_local_id) override;

private:
    static uint64_t read_reg(lldb::SBFrame frame, const char *reg64, const char *reg32,
                             bool &found);
};

} // namespace ocldbg
