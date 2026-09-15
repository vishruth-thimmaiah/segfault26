#pragma once

#include "ocldbg/DebuggerContext.h"

#include <string_view>

namespace ocldbg {

/// Handle custom commands prefixed with "ocl".
/// Returns true if the command was recognized and processed (or handled as unsupported).
bool handle_ocl_command(DebuggerContext &dbg, std::string_view cmd);

} // namespace ocldbg
