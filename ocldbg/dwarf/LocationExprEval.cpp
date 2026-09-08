#include "LocationExprEval.h"

// TODO (Person B): Implement the DWARF expression stack machine.
//
// Key opcodes to handle for -O0 OpenCL:
//   DW_OP_fbreg <offset>   : Frame base + SLEB128 offset (common for stack variables)
//   DW_OP_reg0..31         : Direct machine register
//   DW_OP_regx <reg>       : ULEB128 register number
//   DW_OP_deref            : Pop address, dereference from memory via backend, push value
//   DW_OP_lit0..31         : Push literal integer
//   DW_OP_plus_uconst      : Add unsigned offset to stack top

namespace ocldbg {

LocationExprEval::LocationExprEval(LocationBackend &loc_backend) : loc_backend_(loc_backend) {}

uint64_t LocationExprEval::evaluate(const uint8_t * /*expr_bytes*/, size_t /*expr_len*/,
                                    void * /*exec_ctx*/) const {
    // TODO (Person B): Implement DWARF bytecode evaluation
    return 0;
}

} // namespace ocldbg
