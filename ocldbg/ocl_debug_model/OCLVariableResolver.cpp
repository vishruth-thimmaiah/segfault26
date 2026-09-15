#include "OCLVariableResolver.h"

#include "backends/cpu/CPUExecContext.h"

#include <array>
#include <lldb/API/SBAddress.h>
#include <lldb/API/SBFileSpec.h>
#include <lldb/API/SBFrame.h>
#include <lldb/API/SBModule.h>
#include <lldb/API/SBValue.h>
#include <lldb/API/SBValueList.h>

namespace ocldbg {

namespace {

std::vector<VarValue> fallback_from_frame(lldb::SBFrame frame, ExecCtxHandle exec_ctx,
                                          Backend &backend, const OCLAddressSpaces &spaces) {
    std::vector<VarValue> results;
    if (!frame.IsValid()) {
        return results;
    }
    lldb::SBValueList var_list = frame.GetVariables(true, true, false, true);
    uint32_t count = var_list.GetSize();
    for (uint32_t i = 0; i < count; ++i) {
        lldb::SBValue v = var_list.GetValueAtIndex(i);
        if (!v.IsValid()) {
            continue;
        }
        VarInfo info;
        info.name = v.GetName() != nullptr ? v.GetName() : "";
        info.type_name = v.GetTypeName() != nullptr ? v.GetTypeName() : "";
        info.address_space = spaces.lookup(info.name, info.type_name);
        VarValue val = backend.location_backend().evaluate(info, exec_ctx);
        results.push_back(std::move(val));
    }
    return results;
}

void ensure_loaded(DWARFSourceModel &dwarf, OCLAddressSpaces &spaces, const lldb::SBFrame &frame) {
    if ((dwarf.loaded() && spaces.loaded()) || !frame.IsValid()) {
        return;
    }
    lldb::SBAddress sb_addr = frame.GetPCAddress();
    lldb::SBModule mod = sb_addr.GetModule();
    if (!mod.IsValid()) {
        return;
    }
    std::array<char, 1024> mod_path{};
    if (mod.GetFileSpec().GetPath(mod_path.data(), mod_path.size()) == 0) {
        return;
    }
    if (!dwarf.loaded()) {
        dwarf.load(mod_path.data());
    }
    if (!spaces.loaded()) {
        spaces.load(mod_path.data());
    }
}

} // namespace

OCLVariableResolver::OCLVariableResolver(DWARFSourceModel &dwarf_model,
                                         OCLAddressSpaces &address_spaces)
    : dwarf_(dwarf_model), address_spaces_(address_spaces) {}

std::vector<VarValue> OCLVariableResolver::resolve(const OCLWorkItem &wi, Backend &backend) const {
    std::vector<VarValue> results;

    HostAddress file_pc = 0;
    HostAddress runtime_pc = 0;
    const auto *ctx =
        wi.exec_ctx != nullptr ? static_cast<const CPUExecContext *>(wi.exec_ctx) : nullptr;

    if (ctx != nullptr && ctx->frame.IsValid()) {
        runtime_pc = ctx->frame.GetPC();
        ensure_loaded(dwarf_, address_spaces_, ctx->frame);
        file_pc = ctx->frame.GetPCAddress().GetFileAddress();
    }

    std::vector<VarInfo> vars;
    if (file_pc != 0 && file_pc != LLDB_INVALID_ADDRESS) {
        vars = dwarf_.variables_in_scope(file_pc);
    }
    if (vars.empty() && runtime_pc != 0) {
        vars = dwarf_.variables_in_scope(runtime_pc);
    }

    for (auto &v : vars) {
        v.address_space = address_spaces_.lookup(v.name, v.type_name);
        VarValue val = backend.location_backend().evaluate(v, wi.exec_ctx);
        results.push_back(std::move(val));
    }

    if (results.empty() && ctx != nullptr) {
        return fallback_from_frame(ctx->frame, wi.exec_ctx, backend, address_spaces_);
    }

    return results;
}

} // namespace ocldbg
