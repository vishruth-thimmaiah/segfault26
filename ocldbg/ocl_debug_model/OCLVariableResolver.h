#pragma once
#include "dwarf/DWARFSourceModel.h"
#include "ocldbg/Backend.h"
#include "ocldbg/OCLWorkItem.h"

#include <memory>
#include <vector>

namespace ocldbg {

/// Resolves source-level variable names and values for a stopped work-item.
///
/// Owner: Person B
///
/// This class combines:
///   - DWARFSourceModel (target-independent: what variables exist at a PC)
///   - LocationBackend  (target-specific: what value does the variable hold)
///
/// Callers (DAP server, CLI) never talk to DWARFSourceModel or LocationBackend
/// directly — they go through this class.
class OCLVariableResolver {
public:
    explicit OCLVariableResolver(DWARFSourceModel &dwarf_model);

    /// Return all variables visible at the stopped work-item's current PC,
    /// with values resolved via the backend's LocationBackend.
    ///
    /// @param wi      The selected work-item (must have a valid exec_ctx).
    /// @param backend The active execution backend (provides location_backend()).
    std::vector<VarValue> resolve(const OCLWorkItem &wi, Backend &backend) const;

private:
    DWARFSourceModel &dwarf_;
};

} // namespace ocldbg
