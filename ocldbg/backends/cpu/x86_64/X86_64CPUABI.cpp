#include "X86_64CPUABI.h"

#include <array>

namespace ocldbg {

const CPUABI::ArgRegisters &X86_64CPUABI::arg_registers() const {
    // System V AMD64 passes the first six integer or pointer arguments in these
    // registers, in this order.
    static constexpr ArgRegisters kRegisters{
        .reg64 = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"},
        .reg32 = {"edi", "esi", "edx", "ecx", "r8d", "r9d"},
    };
    return kRegisters;
}

const char *X86_64CPUABI::dwarf_register_name(unsigned dwarf_regnum) const {
    // The System V AMD64 psABI fixes this numbering; it is not the argument order.
    static constexpr std::array<const char *, 32> kRegisters{
        "rax",  "rdx",  "rcx",  "rbx",  "rsi",  "rdi",   "rbp",   "rsp",   "r8",    "r9",   "r10",
        "r11",  "r12",  "r13",  "r14",  "r15",  "rip",   "xmm0",  "xmm1",  "xmm2",  "xmm3", "xmm4",
        "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14"};
    return dwarf_regnum < kRegisters.size() ? kRegisters.at(dwarf_regnum) : nullptr;
}

// NOLINTNEXTLINE(performance-unnecessary-value-param)
bool X86_64CPUABI::read_local_id(lldb::SBFrame frame, const Size3 &local_size,
                                 Size3 &out_local_id) {
    if (!frame.IsValid()) {
        return false;
    }

    if (read_local_id_from_variables(frame, local_size, out_local_id)) {
        return true;
    }

    // In pocl's lowered workgroup loop, complex loops that spill through context
    // allocas keep the induction variable in %r10, and basic loops keep it in %rsi.
    constexpr std::array<std::array<const char *, 2>, 2> induction_registers{
        {{"r10", "r10d"}, {"rsi", "esi"}}};
    for (const auto &candidate : induction_registers) {
        bool found = false;
        uint64_t value = read_register(frame, candidate.at(0), candidate.at(1), found);
        if (found && (local_size.x == 0 || value < local_size.x)) {
            out_local_id = {.x = value, .y = 0, .z = 0};
            // The induction variable names the x axis only.
            return work_group_is_one_dimensional(local_size);
        }
    }

    // Nothing here identifies the work-item. Reporting the origin would make an
    // unknown look like a fact, and the caller renders a failure as WI(?).
    return false;
}

} // namespace ocldbg
