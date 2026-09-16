#pragma once
#include "ocldbg/LocationBackend.h"

namespace ocldbg {

/// LocationBackend for AMD GPUs via amd-dbgapi.
///
/// Resolves DW_OP_reg<n> / DW_OP_regx <uleb> location expressions to a GPU
/// register via amd_dbgapi_dwarf_register_to_register + amd_dbgapi_read_register.
/// This DWARF->register translation is UNVERIFIED on real hardware (the
/// proof of concept behind this backend read architecture-enumerated
/// registers directly, not by DWARF number) -- treat values from this class
/// with corresponding caution until tested against a real DWARF-carrying
/// AMDGPU kernel binary.
///
/// Known limitation: reads lane 0 of the target register regardless of
/// which work-item (lane) is actually selected -- see AMDBackend::select_work_item.
/// Memory-resident (DW_OP_fbreg and friends) locations are not implemented;
/// AMDGPU private/local address spaces need separate handling from the
/// unified-address-space read_global_memory path.
class AMDLocationBackend final : public LocationBackend {
public:
    /// @param exec_ctx must point to an AMDExecContext (AMDBackend.h).
    VarValue evaluate(const VarInfo &var, ExecCtxHandle exec_ctx) override;
};

} // namespace ocldbg
