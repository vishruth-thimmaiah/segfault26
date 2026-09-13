#include "OCLVariableResolver.h"

#include "backends/cpu/CPUExecContext.h"

#include <lldb/API/SBAddress.h>
#include <lldb/API/SBFileSpec.h>
#include <lldb/API/SBFrame.h>
#include <lldb/API/SBModule.h>
#include <lldb/API/SBValue.h>
#include <lldb/API/SBValueList.h>

namespace ocldbg {

OCLVariableResolver::OCLVariableResolver(DWARFSourceModel &dwarf_model) : dwarf_(dwarf_model) {}

std::vector<VarValue> OCLVariableResolver::resolve(const OCLWorkItem &wi, Backend &backend) const {
    std::vector<VarValue> results;

    HostAddress file_pc = 0;
    HostAddress runtime_pc = 0;

    if (wi.exec_ctx) {
        auto *ctx = static_cast<const CPUExecContext *>(wi.exec_ctx);
        if (ctx->frame.IsValid()) {
            runtime_pc = ctx->frame.GetPC();
            lldb::SBAddress sb_addr = ctx->frame.GetPCAddress();
            lldb::SBModule mod = sb_addr.GetModule();
            if (!dwarf_.loaded() && mod.IsValid()) {
                char mod_path[1024];
                if (mod.GetFileSpec().GetPath(mod_path, sizeof(mod_path)) > 0) {
                    dwarf_.load(mod_path);
                }
            }
            file_pc = sb_addr.GetFileAddress();
        }
    }

    std::vector<VarInfo> vars;
    if (file_pc != 0 && file_pc != LLDB_INVALID_ADDRESS) {
        vars = dwarf_.variables_in_scope(file_pc);
    }
    if (vars.empty() && runtime_pc != 0) {
        vars = dwarf_.variables_in_scope(runtime_pc);
    }

    for (const auto &v : vars) {
        VarValue val = backend.location_backend().evaluate(v, wi.exec_ctx);
        results.push_back(std::move(val));
    }

    if (results.empty() && wi.exec_ctx) {
        auto *ctx = static_cast<const CPUExecContext *>(wi.exec_ctx);
        if (ctx->frame.IsValid()) {
            lldb::SBFrame frame = ctx->frame;
            lldb::SBValueList var_list = frame.GetVariables(true, true, false, true);
            uint32_t count = var_list.GetSize();
            for (uint32_t i = 0; i < count; ++i) {
                lldb::SBValue v = var_list.GetValueAtIndex(i);
                if (!v.IsValid()) {
                    continue;
                }
                VarInfo info;
                info.name = v.GetName() ? v.GetName() : "";
                info.type_name = v.GetTypeName() ? v.GetTypeName() : "";
                if (info.type_name.ends_with("*")) {
                    info.address_space = "__global";
                }
                VarValue val = backend.location_backend().evaluate(info, wi.exec_ctx);
                results.push_back(std::move(val));
            }
        }
    }

    return results;
}

} // namespace ocldbg
