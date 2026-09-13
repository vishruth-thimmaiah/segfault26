#include "PoclCPUBackend.h"

#include "WIContextExtractor.h"
#include "WorkGroupTracker.h"

#include <cstdio>
#include <cstring>
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
    if (!exec_ctx) {
        return {};
    }
    auto *ctx = static_cast<const CPUExecContext *>(exec_ctx);
    if (!ctx->frame.IsValid()) {
        return {};
    }

    lldb::SBFrame frame = ctx->frame;
    lldb::SBValue val;
    if (!var.name.empty()) {
        val = frame.FindVariable(var.name.c_str());
    }

    VarValue result;
    result.name = var.name;
    result.type_name = var.type_name;
    result.address_space = var.address_space;

    if (val.IsValid()) {
        if (result.name.empty() && val.GetName()) {
            result.name = val.GetName();
        }
        if (result.type_name.empty() && val.GetTypeName()) {
            result.type_name = val.GetTypeName();
        }

        const char *val_str = val.GetValue();
        if (val_str) {
            result.value_str = val_str;
            result.available = true;
        } else {
            const char *summary = val.GetSummary();
            if (summary) {
                result.value_str = summary;
                result.available = true;
            } else if (val.GetType().IsPointerType()) {
                lldb::addr_t addr = val.GetValueAsUnsigned(0);
                char buf[32];
                std::snprintf(buf, sizeof(buf), "0x%lx", static_cast<unsigned long>(addr));
                result.value_str = buf;
                result.available = true;
            }
        }
        return result;
    }

    // Fallback: evaluate DWARF location expression directly using registers
    if (!var.dwarf_location_expr.empty()) {
        const auto &expr = var.dwarf_location_expr;
        uint8_t op = expr[0];
        // DW_OP_reg0..DW_OP_reg31 (0x50 .. 0x6f)
        if (op >= 0x50 && op <= 0x6f) {
            uint8_t reg_idx = op - 0x50;
            static const char *const x86_regs[] = {
                "rax",  "rdx",  "rcx",  "rbx",   "rsi",   "rdi",   "rbp",   "rsp",
                "r8",   "r9",   "r10",  "r11",   "r12",   "r13",   "r14",   "r15",
                "rip",  "xmm0", "xmm1", "xmm2",  "xmm3",  "xmm4",  "xmm5",  "xmm6",
                "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14"};
            if (reg_idx < sizeof(x86_regs) / sizeof(x86_regs[0])) {
                lldb::SBValue reg_val = frame.FindRegister(x86_regs[reg_idx]);
                if (reg_val.IsValid()) {
                    uint64_t reg_uval = reg_val.GetValueAsUnsigned(0);
                    if (var.type_name.ends_with("*")) {
                        char buf[32];
                        std::snprintf(buf, sizeof(buf), "0x%lx",
                                      static_cast<unsigned long>(reg_uval));
                        result.value_str = buf;
                        result.available = true;
                    } else if (var.type_name == "int" || var.type_name == "signed int") {
                        result.value_str = std::to_string(static_cast<int32_t>(reg_uval));
                        result.available = true;
                    } else if (var.type_name == "float") {
                        float fval = 0.0f;
                        std::memcpy(&fval, &reg_uval, sizeof(float));
                        result.value_str = std::to_string(fval);
                        result.available = true;
                    } else {
                        const char *r_str = reg_val.GetValue();
                        if (r_str) {
                            result.value_str = r_str;
                            result.available = true;
                        }
                    }
                }
            }
        } else if (op == 0x9e && expr.size() >= 2) { // DW_OP_implicit_value
            size_t len = expr[1];
            if (expr.size() >= 2 + len) {
                if (var.type_name == "float" && len == sizeof(float)) {
                    float fval = 0.0f;
                    std::memcpy(&fval, &expr[2], sizeof(float));
                    result.value_str = std::to_string(fval);
                    result.available = true;
                } else if (var.type_name == "int" && len == sizeof(int)) {
                    int ival = 0;
                    std::memcpy(&ival, &expr[2], sizeof(int));
                    result.value_str = std::to_string(ival);
                    result.available = true;
                }
            }
        }
    }

    return result;
}

} // namespace ocldbg
