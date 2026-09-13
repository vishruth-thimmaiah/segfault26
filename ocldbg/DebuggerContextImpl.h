#pragma once

#include "backends/cpu/CPUABI.h"
#include "backends/cpu/WorkGroupTracker.h"
#include "dwarf/DWARFSourceModel.h"
#include "ocldbg/DebuggerContext.h"

#include <lldb/API/SBBreakpoint.h>
#include <lldb/API/SBDebugger.h>
#include <lldb/API/SBProcess.h>
#include <lldb/API/SBTarget.h>
#include <memory>
#include <optional>
#include <string>

namespace ocldbg {

struct DebuggerContext::Impl {
    lldb::SBDebugger debugger;
    lldb::SBTarget target;
    lldb::SBProcess process;
    lldb::SBBreakpoint wg_breakpoint;
    lldb::SBBreakpoint line_breakpoint;
    std::optional<KernelLaunchInfo> launch_info;
    std::string kernel_name;
    WorkGroupTracker wg_tracker;
    std::unique_ptr<CPUABI> abi{CPUABI::create_host_abi()};
    DWARFSourceModel dwarf_model;
};

} // namespace ocldbg
