#include "CPUABI.h"

#include "aarch64/AArch64CPUABI.h"
#include "x86_64/X86_64CPUABI.h"

#include <array>
#include <format>
#include <lldb/API/SBError.h>
#include <lldb/API/SBValue.h>

namespace ocldbg {

namespace {

/// Zero-based positions of the arguments ocldbg decodes, shared by every ABI
/// because they are properties of the functions, not of the machine.
///
///   clEnqueueNDRangeKernel(queue, kernel, work_dim, global_work_offset,
///                          global_work_size, local_work_size, ...)
///   _pocl_kernel_<name>_workgroup(args, context, group_x, group_y, group_z)
constexpr size_t kArgKernel = 1;
constexpr size_t kArgWorkDim = 2;
constexpr size_t kArgGlobalSize = 4;
constexpr size_t kArgLocalSize = 5;
constexpr size_t kArgGroupX = 2;
constexpr size_t kArgGroupY = 3;
constexpr size_t kArgGroupZ = 4;

/// Upper bound used to reject a register that never carried a group id.
constexpr uint64_t kMaxPlausibleGroupId = 0x100000;

} // namespace

// NOLINTNEXTLINE(performance-unnecessary-value-param)
uint64_t CPUABI::read_register(lldb::SBFrame frame, const char *reg64, const char *reg32,
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
bool CPUABI::read_enqueue_ndrange(lldb::SBProcess process, lldb::SBFrame frame, Size3 &out_global,
                                  Size3 &out_local, size_t *out_work_dim) {
    if (!process.IsValid() || !frame.IsValid()) {
        return false;
    }

    const ArgRegisters &regs = arg_registers();
    bool found = false;

    uint64_t work_dim =
        read_register(frame, regs.reg64.at(kArgWorkDim), regs.reg32.at(kArgWorkDim), found);
    if (!found || work_dim == 0) {
        work_dim = 1;
    }

    uint64_t g_ptr = read_register(frame, regs.reg64.at(kArgGlobalSize), nullptr, found);
    if (!found || g_ptr == 0) {
        return false;
    }

    uint64_t l_ptr = read_register(frame, regs.reg64.at(kArgLocalSize), nullptr, found);

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

    if (out_work_dim != nullptr) {
        *out_work_dim = static_cast<size_t>(work_dim);
    }

    return true;
}

// NOLINTNEXTLINE(performance-unnecessary-value-param)
bool CPUABI::read_enqueue_kernel_name(lldb::SBProcess process, lldb::SBFrame frame,
                                      std::string &out_kernel_name) {
    if (!process.IsValid() || !frame.IsValid()) {
        return false;
    }

    bool found = false;
    uint64_t kernel_ptr =
        read_register(frame, arg_registers().reg64.at(kArgKernel), nullptr, found);
    if (!found || kernel_ptr == 0) {
        return false;
    }

    // Ask the loaded ICD for CL_KERNEL_FUNCTION_NAME (0x1190) rather than decoding
    // pocl's own kernel struct, whose layout is not part of any interface.
    std::string expr =
        std::format("char __kname[128] = {{0}}; "
                    "((int(*)(void*, int, unsigned long, void*, void*))clGetKernelInfo)"
                    "((void*){:#x}, 0x1190, 128, __kname, (void*)0); __kname",
                    kernel_ptr);
    lldb::SBValue val = frame.EvaluateExpression(expr.c_str());
    if (!val.IsValid() || val.GetError().Fail()) {
        return false;
    }
    const char *summary = val.GetSummary();
    if (summary == nullptr) {
        return false;
    }

    std::string name = summary;
    if (name.size() >= 2 && name.front() == '"' && name.back() == '"') {
        name = name.substr(1, name.size() - 2);
    }
    if (name.empty()) {
        return false;
    }
    out_kernel_name = name;
    return true;
}

// NOLINTNEXTLINE(performance-unnecessary-value-param)
bool CPUABI::read_workgroup_id(lldb::SBFrame frame, Size3 &out_wg) {
    if (!frame.IsValid()) {
        return false;
    }

    const ArgRegisters &regs = arg_registers();
    bool found_x = false;
    bool found_y = false;
    bool found_z = false;

    uint64_t gx =
        read_register(frame, regs.reg64.at(kArgGroupX), regs.reg32.at(kArgGroupX), found_x);
    uint64_t gy =
        read_register(frame, regs.reg64.at(kArgGroupY), regs.reg32.at(kArgGroupY), found_y);
    uint64_t gz =
        read_register(frame, regs.reg64.at(kArgGroupZ), regs.reg32.at(kArgGroupZ), found_z);

    if (!found_x) {
        return false;
    }

    out_wg.x = gx;
    out_wg.y = (found_y && gy < kMaxPlausibleGroupId) ? gy : 0;
    out_wg.z = (found_z && gz < kMaxPlausibleGroupId) ? gz : 0;
    return true;
}

// NOLINTNEXTLINE(performance-unnecessary-value-param)
bool CPUABI::read_local_id_from_variables(lldb::SBFrame frame, const Size3 &local_size,
                                          Size3 &out_local_id) {
    constexpr std::array<const char *, 4> local_var_candidates{"_local_id_x", "local_id_x",
                                                               "local_id", "_local_id"};
    for (const char *var_name : local_var_candidates) {
        lldb::SBValue v = frame.FindVariable(var_name);
        if (v.IsValid()) {
            out_local_id = {.x = v.GetValueAsUnsigned(0), .y = 0, .z = 0};
            return true;
        }
    }

    // Failing that, the kernel's own global id gives the local id back, since
    // work-groups partition the global range.
    constexpr std::array<const char *, 4> global_var_candidates{"gx", "global_id_x", "global_id",
                                                                "_global_id_x"};
    for (const char *var_name : global_var_candidates) {
        lldb::SBValue v = frame.FindVariable(var_name);
        if (v.IsValid()) {
            size_t lsz = (local_size.x > 0) ? local_size.x : 1;
            out_local_id = {.x = v.GetValueAsUnsigned(0) % lsz, .y = 0, .z = 0};
            return work_group_is_one_dimensional(local_size);
        }
    }

    return false;
}

std::unique_ptr<CPUABI> CPUABI::create_host_abi() {
#if defined(__x86_64__) || defined(_M_X64)
    return std::make_unique<X86_64CPUABI>();
#elif defined(__aarch64__) || defined(_M_ARM64)
    return std::make_unique<AArch64CPUABI>();
#else
    // Callers already treat a null ABI as "work-item state unavailable". Falling
    // back to another architecture's register names would report wrong work-items
    // rather than none.
    return nullptr;
#endif
}

} // namespace ocldbg
