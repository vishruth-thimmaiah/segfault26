#pragma once
#include "ocldbg/LocationBackend.h"

namespace ocldbg {

/// Resolves a name against the selected frame of an accelerator thread: a
/// source variable first, then a register. Location expressions are not
/// evaluated here; LLDB does that for the accelerator target, as it does for
/// the CPU one.
class AcceleratorLocationBackend final : public LocationBackend {
public:
    VarValue evaluate(const VarInfo &var, ExecCtxHandle exec_ctx) override;
};

} // namespace ocldbg
