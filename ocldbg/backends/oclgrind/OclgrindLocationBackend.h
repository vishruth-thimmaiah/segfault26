#pragma once
#include "ocldbg/Backend.h"
#include "ocldbg/LocationBackend.h"

#include <memory>

namespace ocldbg {

/// LocationBackend for the Oclgrind LLVM IR interpreter.
///
/// Owner: Person D
///
/// IMPORTANT — establish from Oclgrind source before implementing:
///   The exact mechanism for retrieving named variable values from Oclgrind's
///   interpreter is not yet confirmed. Do NOT assume wi->getVariable() exists.
///
///   Read these Oclgrind source files first:
///     src/core/WorkItem.h / WorkItem.cpp
///     src/plugins/InteractiveDebugger.cpp  <- how the existing debugger does it
///     src/core/KernelInvocation.cpp        <- interpreter internals
///
///   The instructionExecuted(wi, inst, result) callback gives the TypedValue
///   result of each instruction. This may be sufficient, or you may need to
///   access Oclgrind's internal memory model.
class OclgrindLocationBackend final : public LocationBackend {
public:
    VarValue evaluate(const VarInfo &var, ExecCtxHandle exec_ctx) override;
    // exec_ctx points to an OclgrindExecContext (see OclgrindBackend.h)
    // TODO (Person D): implement after reading Oclgrind internals
};

} // namespace ocldbg
