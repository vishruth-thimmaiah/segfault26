#pragma once
#include "ocldbg/LocationBackend.h"
#include "ocldbg/Types.h"
#include <cstdint>
#include <vector>

namespace ocldbg {

/// Evaluator for DWARF location expressions (DW_OP_* opcodes).
///
/// Owner: Person B
///
/// Consumes target-independent DWARF bytecode and delegates machine-specific
/// register and memory queries to a LocationBackend instance.
class LocationExprEval {
public:
    explicit LocationExprEval(LocationBackend &loc_backend);

    /// Evaluate a DWARF expression using the given machine/frame execution context.
    /// @param expr_bytes   Raw DWARF expression bytecode from DW_AT_location.
    /// @param expr_len     Length of bytecode in bytes.
    /// @param exec_ctx     Target-specific register/frame execution context pointer.
    /// @return Evaluated target address, or 0 if evaluation fails / register-only.
    uint64_t evaluate(const uint8_t *expr_bytes,
                      size_t expr_len,
                      void *exec_ctx) const;

private:
    LocationBackend &loc_backend_;
};

} // namespace ocldbg
