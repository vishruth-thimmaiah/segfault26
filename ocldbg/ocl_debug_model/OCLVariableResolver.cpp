#include "OCLVariableResolver.h"

// TODO: implement resolve().
//
// Algorithm:
//   1. Get stopped WI's current PC from exec_ctx
//      (ask backend or read from exec_ctx directly)
//   2. dwarf_.variables_in_scope(pc)  -> vector<VarInfo>
//   3. for each VarInfo v:
//        VarValue val = backend.location_backend().evaluate(v, wi.exec_ctx)
//        results.push_back(val)
//   4. return results

namespace ocldbg {

OCLVariableResolver::OCLVariableResolver(DWARFSourceModel &dwarf_model) : dwarf_(dwarf_model) {}

std::vector<VarValue> OCLVariableResolver::resolve(const OCLWorkItem & /*wi*/,
                                                   Backend & /*backend*/) const {
    return {};
}

} // namespace ocldbg
