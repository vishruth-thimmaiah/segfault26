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

struct InternalOCLBreakpoint {
    size_t id = 0;
    std::string file;
    unsigned line = 0;
    bool resolved = false;
    uint64_t address = 0;
    lldb::SBBreakpoint sb_bp;
};

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
    std::vector<InternalOCLBreakpoint> ocl_breakpoints;
    size_t next_ocl_bp_id = 1;
    lldb::SBBreakpoint ocl_trampoline_bp;
};

} // namespace ocldbg
