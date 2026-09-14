#pragma once
#include "ocldbg/LocationBackend.h"

namespace ocldbg {

/// Resolves variables by name through Oclgrind rather than evaluating
/// VarInfo::dwarf_location_expr: there is no DWARF expression to run against an
/// IR interpreter. Oclgrind fills its name map as debug records execute, so a
/// variable resolves only once execution has passed its declaration.
class OclgrindLocationBackend final : public LocationBackend {
public:
    /// @param exec_ctx must point to an OclgrindExecContext (OclgrindBackend.h).
    VarValue evaluate(const VarInfo &var, ExecCtxHandle exec_ctx) override;
};

} // namespace ocldbg
