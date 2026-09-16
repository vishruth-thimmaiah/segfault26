#include "AMDBackend.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace ocldbg {

namespace {

/// Minimal ULEB128 decoder for DW_OP_regx's operand. AMDGPU DWARF register
/// numbers commonly exceed 31 (there are far more VGPRs/SGPRs than x86 GP
/// registers), so DW_OP_regx is expected to dominate over DW_OP_reg0..31
/// here, unlike the x86 CPU backend.
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

} // namespace

AMDBackend::AMDBackend() = default;
AMDBackend::~AMDBackend() {
    detach();
}

bool AMDBackend::launch(const std::string &host_binary, const std::vector<std::string> &args) {
    return session_.launch(host_binary, args);
}

void AMDBackend::detach() {
    session_.detach();
    current_ctx_.reset();
}

uint64_t AMDBackend::set_breakpoint(const SourceLocation & /*loc*/) {
    std::cerr << "[ocldbg/amd] set_breakpoint: not yet implemented -- amd-dbgapi's "
                 "insert_breakpoint callback is for the client's own host-side breakpoints, "
                 "not GPU-instruction breakpoints. See AMDBackend.h.\n";
    return 0;
}

void AMDBackend::remove_breakpoint(uint64_t /*bp_id*/) {
    // Nothing to remove: set_breakpoint() never registers one yet.
}

void AMDBackend::on_stop(StopCallback cb) {
    stop_cb_ = std::move(cb);
}

void AMDBackend::resume() {
    AMDWaveInfo wave_info;
    if (!session_.resume_until_wave_stop(wave_info)) {
        current_ctx_.reset();
        return;
    }

    current_ctx_ = AMDExecContext{wave_info.wave, &session_};
    last_stop_reason_ = wave_info.stop_reason;

    OCLWorkItem wi;
    wi.exec_ctx = &(*current_ctx_);

    // Best-effort group_id from the wave's real dispatch coordinate; local_id
    // is left at (0,0,0) -- computing it needs the dispatch's workgroup
    // dimensions (AMD_DBGAPI_WAVE_INFO_DISPATCH -> amd_dbgapi_dispatch_get_info)
    // combined with AMD_DBGAPI_WAVE_INFO_WAVE_NUMBER_IN_WORKGROUP and the
    // per-lane index, which isn't wired up yet.
    std::array<uint32_t, 3> coord{0, 0, 0};
    if (amd_dbgapi_wave_get_info(wave_info.wave, AMD_DBGAPI_WAVE_INFO_WORKGROUP_COORD,
                                 sizeof(coord), coord.data()) == AMD_DBGAPI_STATUS_SUCCESS) {
        wi.group_id = Size3{.x = coord[0], .y = coord[1], .z = coord[2]};
    }

    if (stop_cb_) {
        stop_cb_(OCLStopContext{.stopped = wi, .visible = {wi}});
    }
}

void AMDBackend::step_over(const OCLWorkItem & /*wi*/) {
    std::cerr << "[ocldbg/amd] step_over: not yet implemented (blocked on the same open "
                 "question as set_breakpoint).\n";
}

void AMDBackend::step_in(const OCLWorkItem & /*wi*/) {
    std::cerr << "[ocldbg/amd] step_in: not yet implemented (blocked on the same open "
                 "question as set_breakpoint).\n";
}

bool AMDBackend::select_work_item(const Size3 &global_id, OCLWorkItem &out) {
    // Selects whichever wave is currently halted, regardless of whether
    // global_id actually falls within it -- see the class comment in
    // AMDBackend.h for why lane-precise selection isn't implemented yet.
    if (!current_ctx_.has_value()) {
        return false;
    }
    out.global_id = global_id;
    out.exec_ctx = &(*current_ctx_);

    std::array<uint32_t, 3> coord{0, 0, 0};
    if (amd_dbgapi_wave_get_info(current_ctx_->wave, AMD_DBGAPI_WAVE_INFO_WORKGROUP_COORD,
                                 sizeof(coord), coord.data()) == AMD_DBGAPI_STATUS_SUCCESS) {
        out.group_id = Size3{.x = coord[0], .y = coord[1], .z = coord[2]};
    }
    return true;
}

LocationBackend &AMDBackend::location_backend() {
    return loc_backend_;
}

size_t AMDBackend::read_global_memory(HostAddress addr, void *buf, size_t length) {
    return session_.read_memory(addr, buf, length);
}

VarValue AMDLocationBackend::evaluate(const VarInfo &var, ExecCtxHandle exec_ctx) {
    VarValue result;
    result.name = var.name;
    result.type_name = var.type_name;
    result.address_space = var.address_space;

    auto *ctx = static_cast<AMDExecContext *>(exec_ctx);
    if (ctx == nullptr || ctx->session == nullptr || var.dwarf_location_expr.empty()) {
        return result;
    }

    const auto &expr = var.dwarf_location_expr;
    uint8_t op = expr[0];

    uint64_t dwarf_reg = 0;
    bool have_reg = false;
    if (op >= 0x50 && op <= 0x6f) { // DW_OP_reg0..DW_OP_reg31
        dwarf_reg = op - 0x50;
        have_reg = true;
    } else if (op == 0x90) { // DW_OP_regx
        const uint8_t *p = expr.data() + 1;
        dwarf_reg = decode_uleb128(p, expr.data() + expr.size());
        have_reg = true;
    }

    if (!have_reg) {
        // DW_OP_fbreg and other memory-based locations: not implemented.
        // AMDGPU private/local address spaces need handling distinct from
        // the unified-address-space read_global_memory path.
        return result;
    }

    auto reg_id = ctx->session->dwarf_register(ctx->wave, dwarf_reg);
    if (!reg_id.has_value()) {
        return result;
    }

    // Best-effort scalar decode at lane 0 -- see AMDLocationBackend.h for
    // the known lane-selection limitation.
    size_t elem_size = 4;
    bool is_float = var.type_name == "float";
    if (var.type_name == "double" || var.type_name.ends_with("*")) {
        elem_size = 8;
    }

    std::array<uint8_t, 8> buf{};
    if (!ctx->session->read_register(ctx->wave, *reg_id, 0, elem_size, buf.data())) {
        return result;
    }

    if (is_float) {
        float v = 0.0F;
        std::memcpy(&v, buf.data(), sizeof(v));
        result.value_str = std::to_string(v);
    } else if (var.type_name.ends_with("*")) {
        uint64_t v = 0;
        std::memcpy(&v, buf.data(), sizeof(v));
        std::array<char, 32> hex{};
        std::snprintf(hex.data(), hex.size(), "0x%lx", static_cast<unsigned long>(v));
        result.value_str = hex.data();
    } else {
        int32_t v = 0;
        std::memcpy(&v, buf.data(), sizeof(v));
        result.value_str = std::to_string(v);
    }
    result.available = true;
    return result;
}

} // namespace ocldbg
