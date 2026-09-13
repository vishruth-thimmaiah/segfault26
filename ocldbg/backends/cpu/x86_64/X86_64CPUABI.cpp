#include "X86_64CPUABI.h"

#include <array>
#include <lldb/API/SBValue.h>
#include <string>

namespace ocldbg {

// NOLINTNEXTLINE(performance-unnecessary-value-param)
uint64_t X86_64CPUABI::read_reg(lldb::SBFrame frame, const char *reg64, const char *reg32,
                                bool &found) {
    if (!frame.IsValid()) {
        found = false;
        return 0;
    }

    if (reg64 != nullptr) {
        lldb::SBValue val = frame.FindRegister(reg64);
        if (val.IsValid()) {
            found = true;
            return val.GetValueAsUnsigned(0);
        }
    }

    if (reg32 != nullptr) {
        lldb::SBValue val = frame.FindRegister(reg32);
        if (val.IsValid()) {
            found = true;
            return val.GetValueAsUnsigned(0);
        }
    }

    found = false;
    return 0;
}

// NOLINTNEXTLINE(performance-unnecessary-value-param)
bool X86_64CPUABI::read_enqueue_ndrange(lldb::SBProcess process, lldb::SBFrame frame,
                                        Size3 &out_global, Size3 &out_local) {
    if (!process.IsValid() || !frame.IsValid()) {
        return false;
    }

    // In System V AMD64 ABI for clEnqueueNDRangeKernel:
    // arg 3 (work_dim)           -> %rdx / %edx
    // arg 4 (global_work_offset) -> %rcx
    // arg 5 (global_work_size)   -> %r8
    // arg 6 (local_work_size)    -> %r9
    bool found = false;
    uint64_t work_dim = read_reg(frame, "rdx", "edx", found);
    if (!found || work_dim == 0) {
        work_dim = 1;
    }

    uint64_t g_ptr = read_reg(frame, "r8", nullptr, found);
    if (!found || g_ptr == 0) {
        return false;
    }

    uint64_t l_ptr = read_reg(frame, "r9", nullptr, found);

    lldb::SBError err;
    constexpr uint64_t kPtrSize = sizeof(size_t);
    out_global.x = process.ReadUnsignedFromMemory(g_ptr, kPtrSize, err);
    out_global.y =
        (work_dim > 1) ? process.ReadUnsignedFromMemory(g_ptr + kPtrSize, kPtrSize, err) : 1;
    out_global.z =
        (work_dim > 2) ? process.ReadUnsignedFromMemory(g_ptr + (2 * kPtrSize), kPtrSize, err) : 1;

    if (l_ptr != 0) {
        out_local.x = process.ReadUnsignedFromMemory(l_ptr, kPtrSize, err);
        out_local.y =
            (work_dim > 1) ? process.ReadUnsignedFromMemory(l_ptr + kPtrSize, kPtrSize, err) : 1;
        out_local.z = (work_dim > 2)
                          ? process.ReadUnsignedFromMemory(l_ptr + (2 * kPtrSize), kPtrSize, err)
                          : 1;
    } else {
        out_local = {.x = 1, .y = 1, .z = 1};
    }

    return true;
}

// NOLINTNEXTLINE(performance-unnecessary-value-param)
bool X86_64CPUABI::read_workgroup_id(lldb::SBFrame frame, Size3 &out_wg) {
    if (!frame.IsValid()) {
        return false;
    }

    // In System V AMD64 ABI:
    // arg 2 (group_x) -> %rdx / %edx
    // arg 3 (group_y) -> %rcx / %ecx
    // arg 4 (group_z) -> %r8  / %r8d
    bool found_x = false;
    bool found_y = false;
    bool found_z = false;

    uint64_t gx = read_reg(frame, "rdx", "edx", found_x);
    uint64_t gy = read_reg(frame, "rcx", "ecx", found_y);
    uint64_t gz = read_reg(frame, "r8", "r8d", found_z);

    if (!found_x) {
        return false;
    }

    out_wg.x = gx;
    out_wg.y = (found_y && gy < 0x100000) ? gy : 0;
    out_wg.z = (found_z && gz < 0x100000) ? gz : 0;
    return true;
}

// NOLINTNEXTLINE(performance-unnecessary-value-param)
bool X86_64CPUABI::read_local_id(lldb::SBFrame frame, const Size3 &local_size,
                                 Size3 &out_local_id) {
    if (!frame.IsValid()) {
        return false;
    }

    // 1. Try DWARF local variables first
    constexpr std::array<const char *, 4> local_var_candidates{"_local_id_x", "local_id_x",
                                                               "local_id", "_local_id"};
    for (const char *var_name : local_var_candidates) {
        lldb::SBValue v = frame.FindVariable(var_name);
        if (v.IsValid()) {
            out_local_id.x = v.GetValueAsUnsigned(0);
            out_local_id.y = 0;
            out_local_id.z = 0;
            return true;
        }
    }

    // 2. Check global_id variable in DWARF: local_id.x = global_id % local_size.x
    constexpr std::array<const char *, 4> global_var_candidates{"gx", "global_id_x", "global_id",
                                                                "_global_id_x"};
    for (const char *var_name : global_var_candidates) {
        lldb::SBValue v = frame.FindVariable(var_name);
        if (v.IsValid()) {
            uint64_t gval = v.GetValueAsUnsigned(0);
            size_t lsz = (local_size.x > 0) ? local_size.x : 1;
            out_local_id.x = gval % lsz;
            out_local_id.y = 0;
            out_local_id.z = 0;
            return true;
        }
    }

    // 3. Check loop induction registers for x86_64
    // In pocl's lowered workgroup loop:
    // - Complex loops (like reduce_sum with context allocas) use %r10 / %r10d as the induction
    // variable
    // - Basic loops (like vec_add) use %rsi / %esi as the induction variable
    bool found_reg = false;
    uint64_t r10_val = read_reg(frame, "r10", "r10d", found_reg);
    if (found_reg && (local_size.x == 0 || r10_val < local_size.x)) {
        out_local_id.x = r10_val;
        out_local_id.y = 0;
        out_local_id.z = 0;
        return true;
    }

    uint64_t rsi_val = read_reg(frame, "rsi", "esi", found_reg);
    if (found_reg && (local_size.x == 0 || rsi_val < local_size.x)) {
        out_local_id.x = rsi_val;
        out_local_id.y = 0;
        out_local_id.z = 0;
        return true;
    }

    // 4. Fallback at workgroup entry point: initial local_id is (0,0,0)
    out_local_id = {.x = 0, .y = 0, .z = 0};
    return true;
}

std::unique_ptr<CPUABI> CPUABI::create_host_abi() {
#if defined(__x86_64__) || defined(_M_X64)
    return std::make_unique<X86_64CPUABI>();
#else
    return std::make_unique<X86_64CPUABI>();
#endif
}

} // namespace ocldbg
