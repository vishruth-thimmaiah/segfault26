#include "PoclCPUBackend.h"

#include "WIContextExtractor.h"
#include "WorkGroupTracker.h"
#include "ocldbg/DebuggerContext.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <lldb/API/SBData.h>
#include <lldb/API/SBError.h>
#include <lldb/API/SBValue.h>
#include <map>
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

constexpr std::array<const char *, 32> kX86Regs = {
    "rax",  "rdx",  "rcx",  "rbx",  "rsi",  "rdi",   "rbp",   "rsp",   "r8",    "r9",   "r10",
    "r11",  "r12",  "r13",  "r14",  "r15",  "rip",   "xmm0",  "xmm1",  "xmm2",  "xmm3", "xmm4",
    "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14"};

int64_t decode_sleb128(const uint8_t *&p, const uint8_t *end) {
    int64_t result = 0;
    int shift = 0;
    uint8_t byte = 0;
    do {
        if (p >= end) {
            break;
        }
        byte = *p++;
        result |= static_cast<int64_t>(byte & 0x7f) << shift;
        shift += 7;
    } while ((byte & 0x80) != 0);
    if ((shift < 64) && ((byte & 0x40) != 0)) {
        result |= -(static_cast<int64_t>(1) << shift);
    }
    return result;
}

uint64_t decode_uleb128(const uint8_t *&p, const uint8_t *end) {
    uint64_t result = 0;
    int shift = 0;
    while (p < end) {
        uint8_t byte = *p++;
        result |= static_cast<uint64_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) {
            break;
        }
        shift += 7;
    }
    return result;
}

VarValue eval_from_memory(const lldb::SBFrame &frame, lldb::addr_t addr, const VarInfo &var) {
    VarValue result;
    result.name = var.name;
    result.type_name = var.type_name;
    result.address_space = var.address_space;
    if (addr == 0 || addr == LLDB_INVALID_ADDRESS) {
        return result;
    }
    lldb::SBProcess process = frame.GetThread().GetProcess();
    if (!process.IsValid()) {
        return result;
    }

    lldb::SBError err;
    if (var.type_name == "int" || var.type_name == "signed int") {
        int32_t ival = 0;
        process.ReadMemory(addr, &ival, sizeof(ival), err);
        if (err.Success()) {
            result.value_str = std::to_string(ival);
            result.available = true;
        }
    } else if (var.type_name == "float") {
        float fval = 0.0F;
        process.ReadMemory(addr, &fval, sizeof(fval), err);
        if (err.Success()) {
            result.value_str = std::to_string(fval);
            result.available = true;
        }
    } else if (var.type_name.ends_with("*")) {
        uint64_t pval = 0;
        process.ReadMemory(addr, &pval, sizeof(pval), err);
        if (err.Success()) {
            std::array<char, 32> buf{};
            std::snprintf(buf.data(), buf.size(), "0x%lx", static_cast<unsigned long>(pval));
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
    if (reg_idx >= kX86Regs.size()) {
        return result;
    }

    lldb::SBValue reg_val = frame.FindRegister(kX86Regs[reg_idx]);
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
    std::unique_ptr<DebuggerContext> dbg;
    struct PendingBp {
        SourceLocation loc;
    };
    std::map<uint64_t, PendingBp> pending_bps;
    uint64_t next_bp_id = 1;
    bool launched = false;
    bool halted = false;
};

void PoclCPUBackend::notify_stop() {
    if (!impl_->dbg) {
        return;
    }
    auto stopped_items = impl_->dbg->list_stopped_work_items();
    if (stopped_items.empty()) {
        return;
    }
    OCLStopContext stop_ctx;
    stop_ctx.stopped = stopped_items.front();
    for (size_t i = 1; i < stopped_items.size(); ++i) {
        stop_ctx.visible.push_back(stopped_items[i]);
    }
    impl_->halted = true;
    if (stop_cb_) {
        stop_cb_(stop_ctx);
    }
}

PoclCPUBackend::PoclCPUBackend()
    : wg_tracker_(std::make_unique<WorkGroupTracker>()),
      wi_extractor_(std::make_unique<WIContextExtractor>()), impl_(std::make_unique<Impl>()) {}

PoclCPUBackend::~PoclCPUBackend() = default;

bool PoclCPUBackend::launch(const std::string &host_binary, const std::vector<std::string> &args) {
    if (!impl_->dbg) {
        impl_->dbg = std::make_unique<DebuggerContext>();
    }
    impl_->dbg->redirect_output_to_stderr();
    for (const auto &[id, bp] : impl_->pending_bps) {
        impl_->dbg->add_ocl_breakpoint(bp.loc.file, bp.loc.line);
    }
    if (!impl_->dbg->launch(host_binary, args, /*stop_at_entry=*/true)) {
        return false;
    }
    impl_->dbg->set_workgroup_breakpoint();
    impl_->dbg->ensure_ocl_trampoline();
    impl_->dbg->resolve_pending_ocl_breakpoints();
    impl_->launched = true;
    return true;
}

void PoclCPUBackend::detach() {
    if (impl_->dbg) {
        impl_->dbg->terminate_process();
    }
    impl_->launched = false;
    impl_->halted = false;
}

uint64_t PoclCPUBackend::set_breakpoint(const SourceLocation &loc) {
    uint64_t id = impl_->next_bp_id++;
    impl_->pending_bps[id] = {loc};
    if (impl_->dbg) {
        impl_->dbg->add_ocl_breakpoint(loc.file, loc.line);
    }
    return id;
}

void PoclCPUBackend::remove_breakpoint(uint64_t bp_id) {
    impl_->pending_bps.erase(bp_id);
    if (impl_->dbg) {
        impl_->dbg->delete_ocl_breakpoint(bp_id);
    }
}

void PoclCPUBackend::on_stop(StopCallback cb) {
    stop_cb_ = std::move(cb);
}

void PoclCPUBackend::resume() {
    if (!impl_->dbg) {
        return;
    }
    impl_->halted = false;
    if (impl_->dbg->continue_execution()) {
        notify_stop();
    }
}

void PoclCPUBackend::step_over(const OCLWorkItem &wi) {
    if (wi.exec_ctx != nullptr) {
        const auto *ctx = static_cast<const CPUExecContext *>(wi.exec_ctx);
        if (ctx->thread.IsValid()) {
            lldb::SBThread thread = ctx->thread;
            thread.StepOver();
            notify_stop();
        }
    }
}

void PoclCPUBackend::step_in(const OCLWorkItem &wi) {
    if (wi.exec_ctx != nullptr) {
        const auto *ctx = static_cast<const CPUExecContext *>(wi.exec_ctx);
        if (ctx->thread.IsValid()) {
            lldb::SBThread thread = ctx->thread;
            thread.StepInto();
            notify_stop();
        }
    }
}

bool PoclCPUBackend::select_work_item(const Size3 &global_id, OCLWorkItem &out) {
    if (!impl_->dbg) {
        return false;
    }
    if (impl_->dbg->select_work_item(global_id)) {
        auto sel = impl_->dbg->get_selected_work_item();
        if (sel.has_value()) {
            out = *sel;
            return true;
        }
    }
    return false;
}

LocationBackend &PoclCPUBackend::location_backend() {
    return loc_backend_;
}

size_t PoclCPUBackend::read_global_memory(HostAddress addr, void *buf, size_t length) {
    if (!impl_->dbg) {
        return 0;
    }
    return impl_->dbg->read_memory(addr, buf, length);
}

static VarValue eval_const(uint8_t op, const std::vector<uint8_t> &expr, const VarInfo &var) {
    int64_t val = 0;
    if (op >= 0x30 && op <= 0x4f) { // DW_OP_lit0..DW_OP_lit31
        val = op - 0x30;
    } else if (op == 0x11) { // DW_OP_consts
        const uint8_t *p = expr.data() + 1;
        val = decode_sleb128(p, expr.data() + expr.size());
    } else if (op == 0x10) { // DW_OP_constu
        const uint8_t *p = expr.data() + 1;
        val = static_cast<int64_t>(decode_uleb128(p, expr.data() + expr.size()));
    } else {
        return {};
    }
    VarValue result;
    result.name = var.name;
    result.type_name = var.type_name;
    result.address_space = var.address_space;
    if (var.type_name == "float") {
        result.value_str = std::to_string(static_cast<float>(val));
    } else {
        result.value_str = std::to_string(val);
    }
    result.available = true;
    return result;
}

static VarValue eval_breg(lldb::SBFrame &frame, uint8_t op, const std::vector<uint8_t> &expr,
                          const VarInfo &var) {
    uint8_t reg_idx = op - 0x70;
    if (reg_idx >= kX86Regs.size()) {
        return {};
    }
    lldb::SBValue reg_val = frame.FindRegister(kX86Regs[reg_idx]);
    if (!reg_val.IsValid()) {
        return {};
    }
    const uint8_t *p = expr.data() + 1;
    int64_t offset = decode_sleb128(p, expr.data() + expr.size());
    lldb::addr_t addr = reg_val.GetValueAsUnsigned(0) + offset;
    if (!expr.empty() && expr.back() == 0x9f) { // DW_OP_stack_value
        VarValue result;
        result.name = var.name;
        result.type_name = var.type_name;
        result.address_space = var.address_space;
        if (var.type_name == "float") {
            float fval = 0.0F;
            std::memcpy(&fval, &addr, sizeof(float));
            result.value_str = std::to_string(fval);
        } else if (var.type_name == "int" || var.type_name == "signed int") {
            result.value_str = std::to_string(static_cast<int32_t>(addr));
        } else {
            result.value_str = std::to_string(addr);
        }
        result.available = true;
        return result;
    }
    return eval_from_memory(frame, addr, var);
}

static VarValue eval_fbreg(lldb::SBFrame &frame, const std::vector<uint8_t> &expr,
                           const VarInfo &var) {
    lldb::addr_t fb = frame.GetFP();
    if (fb == 0 || fb == LLDB_INVALID_ADDRESS) {
        fb = frame.GetSP();
    }
    const uint8_t *p = expr.data() + 1;
    int64_t offset = decode_sleb128(p, expr.data() + expr.size());
    return eval_from_memory(frame, fb + offset, var);
}

static VarValue eval_from_dwarf_expr(lldb::SBFrame &frame, const VarInfo &var) {
    const auto &expr = var.dwarf_location_expr;
    if (expr.empty()) {
        return {};
    }
    uint8_t op = expr[0];
    if (op >= 0x50 && op <= 0x6f) { // DW_OP_reg0..DW_OP_reg31
        return eval_from_register(frame, op, var);
    }
    if (op == 0x9e) { // DW_OP_implicit_value
        return eval_implicit_value(expr, var);
    }
    if ((op >= 0x30 && op <= 0x4f) || op == 0x11 || op == 0x10) {
        return eval_const(op, expr, var);
    }
    if (op >= 0x70 && op <= 0x8f) { // DW_OP_breg0..DW_OP_breg31
        return eval_breg(frame, op, expr, var);
    }
    if (op == 0x91) { // DW_OP_fbreg
        return eval_fbreg(frame, expr, var);
    }
    return {};
}

static std::optional<VarValue> eval_alias(lldb::SBFrame &frame, const VarInfo &var) {
    for (const char *prefix : {"k_", "v_"}) {
        std::string alias = std::string(prefix) + var.name;
        lldb::SBValue alias_val = frame.FindVariable(alias.c_str());
        if (alias_val.IsValid()) {
            VarValue sb_res = eval_from_sbvalue(alias_val, var);
            if (sb_res.available) {
                sb_res.name = var.name;
                return sb_res;
            }
        }
    }
    return std::nullopt;
}

VarValue CPULocationBackend::evaluate(const VarInfo &var, ExecCtxHandle exec_ctx) {
    if (exec_ctx == nullptr) {
        return {};
    }
    const auto *ctx = static_cast<const CPUExecContext *>(exec_ctx);
    if (!ctx->frame.IsValid()) {
        return {};
    }

    static std::map<std::string, VarValue> s_param_cache;

    lldb::SBFrame frame = ctx->frame;
    if (!var.name.empty()) {
        lldb::SBValue val = frame.FindVariable(var.name.c_str());
        if (val.IsValid()) {
            VarValue sb_res = eval_from_sbvalue(val, var);
            if (sb_res.available) {
                s_param_cache[var.name] = sb_res;
                return sb_res;
            }
        }
    }

    if (!var.dwarf_location_expr.empty()) {
        VarValue dwarf_res = eval_from_dwarf_expr(frame, var);
        if (dwarf_res.available) {
            s_param_cache[var.name] = dwarf_res;
            return dwarf_res;
        }
    }

    if (!var.name.empty()) {
        auto it = s_param_cache.find(var.name);
        if (it != s_param_cache.end()) {
            VarValue cached = it->second;
            if (!var.type_name.empty()) {
                cached.type_name = var.type_name;
            }
            if (!var.address_space.empty()) {
                cached.address_space = var.address_space;
            }
            return cached;
        }

        if (auto alias_res = eval_alias(frame, var)) {
            s_param_cache[var.name] = *alias_res;
            return *alias_res;
        }
    }

    VarValue result;
    result.name = var.name;
    result.type_name = var.type_name;
    result.address_space = var.address_space;
    return result;
}

} // namespace ocldbg
