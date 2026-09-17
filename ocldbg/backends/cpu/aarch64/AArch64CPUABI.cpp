#include "AArch64CPUABI.h"

#include <array>

namespace ocldbg {

const CPUABI::ArgRegisters &AArch64CPUABI::arg_registers() const {
    // AAPCS64 passes the first eight integer or pointer arguments in x0-x7, with
    // w0-w7 naming their low 32 bits. ocldbg decodes no call that reaches x6.
    static constexpr ArgRegisters kRegisters{
        .reg64 = {"x0", "x1", "x2", "x3", "x4", "x5"},
        .reg32 = {"w0", "w1", "w2", "w3", "w4", "w5"},
    };
    return kRegisters;
}

const char *AArch64CPUABI::dwarf_register_name(unsigned dwarf_regnum) const {
    // The AArch64 psABI numbers the general-purpose registers 0-30 and the stack
    // pointer 31. The SIMD registers start at 64, which DW_OP_reg0..31 and
    // DW_OP_breg0..31 cannot encode, so nothing above the stack pointer is mapped.
    static constexpr std::array<const char *, 32> kRegisters{
        "x0",  "x1",  "x2",  "x3",  "x4",  "x5",  "x6",  "x7",  "x8",  "x9",  "x10",
        "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18", "x19", "x20", "x21",
        "x22", "x23", "x24", "x25", "x26", "x27", "x28", "x29", "x30", "sp"};
    return dwarf_regnum < kRegisters.size() ? kRegisters.at(dwarf_regnum) : nullptr;
}

// NOLINTNEXTLINE(performance-unnecessary-value-param)
bool AArch64CPUABI::read_local_id(lldb::SBFrame frame, const Size3 &local_size,
                                  Size3 &out_local_id) {
    if (!frame.IsValid()) {
        return false;
    }

    if (read_local_id_from_variables(frame, local_size, out_local_id)) {
        return true;
    }

    // There is no register ladder here as there is on x86_64. AAPCS64 fixes the
    // argument registers but says nothing about where a lowered work-item loop
    // keeps its induction variable, and the register LLVM allocates for it has not
    // been established by observation. Guessing one would report a plausible but
    // wrong work-item whenever the guess happened to hold a small value.
    out_local_id = {.x = 0, .y = 0, .z = 0};
    return true;
}

} // namespace ocldbg
