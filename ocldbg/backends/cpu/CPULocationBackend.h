#pragma once
#include "ocldbg/Backend.h"
#include "ocldbg/LocationBackend.h"
#include <memory>

namespace ocldbg {

/// LocationBackend for the pocl CPU device.
///
/// Owner: Person C
///
/// Evaluates DWARF location expressions (DW_OP_*) in the context of an LLDB
/// SBFrame for the stopped host thread. The ExecCtxHandle must point to a
/// CPUExecContext (defined in PoclCPUBackend.h).
class CPULocationBackend final : public LocationBackend {
public:
    VarValue evaluate(const VarInfo &var, ExecCtxHandle exec_ctx) override;
    // TODO (Person C): use LLDB SBFrame / SBValue to evaluate the DWARF
    // location expression stored in var.dwarf_location_expr.
};

} // namespace ocldbg
