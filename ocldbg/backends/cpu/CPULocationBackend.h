#pragma once
#include "backends/cpu/CPUABI.h"
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

private:
    /// DW_OP_reg / DW_OP_breg carry psABI register numbers, so decoding them
    /// needs the host architecture's mapping.
    std::unique_ptr<CPUABI> abi_{CPUABI::create_host_abi()};
};

} // namespace ocldbg
