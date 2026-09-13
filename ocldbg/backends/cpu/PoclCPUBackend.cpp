#include "PoclCPUBackend.h"

#include "WIContextExtractor.h"
#include "WorkGroupTracker.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <lldb/API/SBData.h>
#include <lldb/API/SBError.h>
#include <lldb/API/SBValue.h>
#include <stdexcept>

// TODO (Person C): implement all methods below.
//
// Prerequisites:
//   1. Resolve the LD_PRELOAD shim question (PLANNING.md §9 item 3).
//      Read pocl source: lib/CL/devices/cpu/ to find the WG dispatch site.
//   2. Establish LLDB control: link against liblldb, create SBDebugger,
//      set POCL_EXTRA_BUILD_FLAGS="-g -cl-opt-disable" in the environment
//      before launching so the kernel carries DWARF.
//
// Stepping semantics note:
//   In pocl's `loops` strategy, step_over() steps the host thread one
//   source line — which may advance the loop induction variable (WI index)
//   by more than one iteration. Document the exact observed behavior.

namespace ocldbg {

namespace {

VarValue eval_from_sbvalue(lldb::SBValue val, const VarInfo &var) {
    VarValue result;
    result.name = var.name;
    result.type_name = var.type_name;
    result.address_space = var.address_space;

    if (result.name.empty() && val.GetName() != nullptr) {
        result.name = val.GetName();
    }
    if (result.type_name.empty() && val.GetTypeName() != nullptr) {
        result.type_name = val.GetTypeName();
    }

    const char *val_str = val.GetValue();
    if (val_str != nullptr) {
        result.value_str = val_str;
        result.available = true;
    } else {
        const char *summary = val.GetSummary();
        if (summary != nullptr) {
            result.value_str = summary;
            result.available = true;
        } else if (val.GetType().IsPointerType()) {
            lldb::addr_t addr = val.GetValueAsUnsigned(0);
            std::array<char, 32> buf{};
            std::snprintf(buf.data(), buf.size(), "0x%lx", static_cast<unsigned long>(addr));
            result.value_str = buf.data();
            result.available = true;
        }
    }
    return result;
}

VarValue eval_from_register(lldb::SBFrame frame, uint8_t op, const VarInfo &var) {
    VarValue result;
    result.name = var.name;
    result.type_name = var.type_name;
    result.address_space = var.address_space;

    uint8_t reg_idx = op - 0x50;
    constexpr std::array<const char *, 32> x86_regs = {
        "rax",  "rdx",  "rcx",  "rbx",  "rsi",  "rdi",   "rbp",   "rsp",   "r8",    "r9",   "r10",
        "r11",  "r12",  "r13",  "r14",  "r15",  "rip",   "xmm0",  "xmm1",  "xmm2",  "xmm3", "xmm4",
        "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14"};

    if (reg_idx >= x86_regs.size()) {
        return result;
    }

    lldb::SBValue reg_val = frame.FindRegister(x86_regs[reg_idx]);
    if (!reg_val.IsValid()) {
        return result;
    }

    uint64_t reg_uval = reg_val.GetValueAsUnsigned(0);
    if (var.type_name.ends_with("*")) {
        std::array<char, 32> buf{};
        std::snprintf(buf.data(), buf.size(), "0x%lx", static_cast<unsigned long>(reg_uval));
        result.value_str = buf.data();
        result.available = true;
    } else if (var.type_name == "int" || var.type_name == "signed int") {
        result.value_str = std::to_string(static_cast<int32_t>(reg_uval));
        result.available = true;
    } else if (var.type_name == "float") {
        lldb::SBError err;
        float fval = reg_val.GetData().GetFloat(err, 0);
        if (err.Success()) {
            result.value_str = std::to_string(fval);
            result.available = true;
        } else {
            float fallback = 0.0F;
            std::memcpy(&fallback, &reg_uval, sizeof(float));
            result.value_str = std::to_string(fallback);
            result.available = true;
        }
    } else {
        const char *r_str = reg_val.GetValue();
        if (r_str != nullptr) {
            result.value_str = r_str;
            result.available = true;
        }
    }
    return result;
}

VarValue eval_implicit_value(const std::vector<uint8_t> &expr, const VarInfo &var) {
    VarValue result;
    result.name = var.name;
    result.type_name = var.type_name;
    result.address_space = var.address_space;

    if (expr.size() < 2) {
        return result;
    }
    size_t len = expr[1];
    if (expr.size() < 2 + len) {
        return result;
    }

    if (var.type_name == "float" && len == sizeof(float)) {
        float fval = 0.0F;
        std::memcpy(&fval, &expr[2], sizeof(float));
        result.value_str = std::to_string(fval);
        result.available = true;
    } else if (var.type_name == "int" && len == sizeof(int)) {
        int ival = 0;
        std::memcpy(&ival, &expr[2], sizeof(int));
        result.value_str = std::to_string(ival);
        result.available = true;
    }
    return result;
}

} // namespace

struct PoclCPUBackend::Impl {
    // TODO: LLDB SBDebugger debugger;
    // TODO: LLDB SBTarget   target;
    // TODO: LLDB SBProcess  process;
};

PoclCPUBackend::PoclCPUBackend()
    : wg_tracker_(std::make_unique<WorkGroupTracker>()),
      wi_extractor_(std::make_unique<WIContextExtractor>()), impl_(std::make_unique<Impl>()) {}

PoclCPUBackend::~PoclCPUBackend() = default;

bool PoclCPUBackend::launch(const std::string & /*host_binary*/,
                            const std::vector<std::string> & /*args*/) {
    // TODO (Person C):
    //   1. Set env: POCL_EXTRA_BUILD_FLAGS="-g -cl-opt-disable",
    //               POCL_LEAVE_KERNEL_COMPILER_TEMP_FILES=1,
    //               LD_PRELOAD=path/to/libocldbg_rt.so
    //   2. SBDebugger::Create() -> impl_->debugger
    //   3. debugger.CreateTarget(host_binary) -> impl_->target
    //   4. target.Launch(...) -> impl_->process
    //   5. Set LLDB breakpoint listener
    return false;
}

void PoclCPUBackend::detach() {
    // TODO (Person C): process.Detach()
}

uint64_t PoclCPUBackend::set_breakpoint(const SourceLocation & /*loc*/) {
    // TODO (Person C):
    //   Use DWARFSourceModel::source_to_pcs() to get PCs, then:
    //   impl_->target.BreakpointCreateByAddress(pc) for each PC.
    return 0;
}

void PoclCPUBackend::remove_breakpoint(uint64_t /*bp_id*/) {
    // TODO (Person C): impl_->target.BreakpointDelete(bp_id)
}

void PoclCPUBackend::on_stop(StopCallback cb) {
    stop_cb_ = std::move(cb);
}

void PoclCPUBackend::resume() {
    // TODO (Person C): impl_->process.Continue()
}

void PoclCPUBackend::step_over(const OCLWorkItem & /*wi*/) {
    // TODO (Person C): SBThread::StepOver() on the WI's host thread.
    // Document observed stepping semantics with loops strategy.
}

void PoclCPUBackend::step_in(const OCLWorkItem & /*wi*/) {
    // TODO (Person C): SBThread::StepInto()
}

bool PoclCPUBackend::select_work_item(const Size3 & /*global_id*/, OCLWorkItem & /*out*/) {
    // TODO (Person C):
    //   1. wg_tracker_->find_wg_for_wi(global_id) -> host_thread_id
    //   2. wi_extractor_->extract(host_thread_id, global_id) -> OCLWorkItem
    return false;
}

LocationBackend &PoclCPUBackend::location_backend() {
    return loc_backend_;
}

size_t PoclCPUBackend::read_global_memory(HostAddress /*addr*/, void * /*buf*/, size_t /*length*/) {
    // TODO (Person C): impl_->process.ReadMemory(addr, buf, length, error)
    return 0;
}

VarValue CPULocationBackend::evaluate(const VarInfo &var, ExecCtxHandle exec_ctx) {
    if (exec_ctx == nullptr) {
        return {};
    }
    const auto *ctx = static_cast<const CPUExecContext *>(exec_ctx);
    if (!ctx->frame.IsValid()) {
        return {};
    }

    lldb::SBFrame frame = ctx->frame;
    if (!var.name.empty()) {
        lldb::SBValue val = frame.FindVariable(var.name.c_str());
        if (val.IsValid()) {
            return eval_from_sbvalue(val, var);
        }
    }

    // Fallback: evaluate DWARF location expression directly using registers
    if (!var.dwarf_location_expr.empty()) {
        const auto &expr = var.dwarf_location_expr;
        uint8_t op = expr[0];
        // DW_OP_reg0..DW_OP_reg31 (0x50 .. 0x6f)
        if (op >= 0x50 && op <= 0x6f) {
            return eval_from_register(frame, op, var);
        }
        if (op == 0x9e) { // DW_OP_implicit_value
            return eval_implicit_value(expr, var);
        }
    }

    VarValue result;
    result.name = var.name;
    result.type_name = var.type_name;
    result.address_space = var.address_space;
    return result;
}

} // namespace ocldbg
