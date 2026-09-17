#include "ocldbg/DebuggerContext.h"

#include "DebuggerContextImpl.h"
#include "backends/cpu/CPUABI.h"
#include "backends/cpu/PoclCPUBackend.h"
#include "backends/cpu/WIContextExtractor.h"
#include "backends/cpu/WorkGroupTracker.h"
#include "backends/oclgrind/OclgrindBackend.h"
#include "dap/DAPServer.h"
#include "dwarf/DWARFSourceModel.h"
#include "ocl_debug_model/OCLVariableResolver.h"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <iostream>
#include <lldb/API/SBBreakpoint.h>
#include <lldb/API/SBBreakpointLocation.h>
#include <lldb/API/SBDebugger.h>
#include <lldb/API/SBError.h>
#include <lldb/API/SBFileSpec.h>
#include <lldb/API/SBFrame.h>
#include <lldb/API/SBLaunchInfo.h>
#include <lldb/API/SBModule.h>
#include <lldb/API/SBProcess.h>
#include <lldb/API/SBSymbol.h>
#include <lldb/API/SBTarget.h>
#include <lldb/API/SBThread.h>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ocldbg {

namespace {

std::vector<WorkGroupBound> compute_work_groups(const Size3 &global_size, const Size3 &local_size,
                                                const Size3 &num_wg) {
    size_t lx = (local_size.x > 0) ? local_size.x : 1;
    size_t ly = (local_size.y > 0) ? local_size.y : 1;
    size_t lz = (local_size.z > 0) ? local_size.z : 1;

    std::vector<WorkGroupBound> bounds;
    bounds.reserve(num_wg.x * num_wg.y * num_wg.z);

    for (size_t gz = 0; gz < num_wg.z; ++gz) {
        for (size_t gy = 0; gy < num_wg.y; ++gy) {
            for (size_t gx = 0; gx < num_wg.x; ++gx) {
                Size3 cur_wg{.x = gx, .y = gy, .z = gz};
                Size3 min_wi{.x = gx * lx, .y = gy * ly, .z = gz * lz};
                Size3 max_wi{.x = std::min(global_size.x, (gx + 1) * lx) - 1,
                             .y = std::min(global_size.y, (gy + 1) * ly) - 1,
                             .z = std::min(global_size.z, (gz + 1) * lz) - 1};
                size_t items = (((max_wi.x - min_wi.x) + 1) * ((max_wi.y - min_wi.y) + 1)) *
                               ((max_wi.z - min_wi.z) + 1);
                bounds.push_back(WorkGroupBound{
                    .group_id = cur_wg,
                    .min_wi = min_wi,
                    .max_wi = max_wi,
                    .item_count = items,
                });
            }
        }
    }
    return bounds;
}

std::vector<WorkItemMapping> compute_sample_work_items(const Size3 &global_size,
                                                       const Size3 &local_size) {
    size_t lx = (local_size.x > 0) ? local_size.x : 1;
    size_t ly = (local_size.y > 0) ? local_size.y : 1;
    size_t lz = (local_size.z > 0) ? local_size.z : 1;

    std::vector<Size3> sample_coords;
    sample_coords.push_back({.x = 0, .y = 0, .z = 0});
    if (lx > 1) {
        sample_coords.push_back({.x = lx - 1, .y = 0, .z = 0});
    }
    if (global_size.x > lx) {
        sample_coords.push_back({.x = lx, .y = 0, .z = 0});
        sample_coords.push_back({.x = global_size.x - 1, .y = 0, .z = 0});
    }
    if (global_size.y > 1) {
        sample_coords.push_back({.x = 0, .y = 1, .z = 0});
    }
    if (global_size.z > 1) {
        sample_coords.push_back({.x = 0, .y = 0, .z = 1});
    }

    std::vector<WorkItemMapping> mappings;
    mappings.reserve(sample_coords.size());
    for (const auto &gwi : sample_coords) {
        mappings.push_back(WorkItemMapping{
            .global_id = gwi,
            .group_id = {.x = gwi.x / lx, .y = gwi.y / ly, .z = gwi.z / lz},
            .local_id = {.x = gwi.x % lx, .y = gwi.y % ly, .z = gwi.z % lz},
        });
    }
    return mappings;
}

std::optional<lldb::addr_t> find_pocl_dispatch_address(lldb::SBTarget &target) {
    lldb::SBModule pthread_mod;
    uint32_t num_mods = target.GetNumModules();
    for (uint32_t i = 0; i < num_mods; ++i) {
        lldb::SBModule m = target.GetModuleAtIndex(i);
        const char *fn = m.GetFileSpec().GetFilename();
        if (fn != nullptr &&
            std::string_view(fn).find("libpocl-devices-pthread.so") != std::string_view::npos) {
            pthread_mod = m;
            break;
        }
    }

    if (!pthread_mod.IsValid()) {
        return std::nullopt;
    }

    // In PoCL pthread backend, work-group dispatch is performed via:
    // call *0xb8(%r14)  [Opcode: 41 ff 96 b8 00 00 00]
    // where %rdx, %rcx, %r8 hold group coordinates (X, Y, Z).
    constexpr std::array<uint8_t, 7> kOpcodePattern{0x41, 0xff, 0x96, 0xb8, 0x00, 0x00, 0x00};

    std::array<char, 1024> full_path{};
    uint32_t path_len = pthread_mod.GetFileSpec().GetPath(full_path.data(), full_path.size());
    if (path_len == 0) {
        return std::nullopt;
    }

    std::ifstream file(full_path.data(), std::ios::binary);
    if (!file) {
        return std::nullopt;
    }

    std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
    auto subrange = std::ranges::search(buffer, kOpcodePattern);
    if (subrange.empty()) {
        return std::nullopt;
    }

    auto offset = static_cast<size_t>(std::distance(buffer.begin(), subrange.begin()));
    lldb::addr_t load_base = pthread_mod.GetObjectFileHeaderAddress().GetLoadAddress(target);
    if (load_base == LLDB_INVALID_ADDRESS) {
        return std::nullopt;
    }

    return load_base + offset;
}

bool record_stopped_workgroups(lldb::SBProcess &process, CPUABI *abi, WorkGroupTracker &tracker,
                               size_t &dispatch_count, uint64_t &first_hit_thread_id) {
    bool any_wg_hit = false;
    uint32_t num_threads = process.GetNumThreads();
    for (uint32_t i = 0; i < num_threads; ++i) {
        lldb::SBThread t = process.GetThreadAtIndex(i);
        if (!t.IsValid() || t.GetStopReason() != lldb::eStopReasonBreakpoint) {
            continue;
        }

        lldb::SBFrame f = t.GetSelectedFrame();
        if (!f.IsValid()) {
            f = t.GetFrameAtIndex(0);
        }

        Size3 wg_id;
        if (abi->read_workgroup_id(f, wg_id)) {
            tracker.record_wg(t.GetThreadID(), wg_id);
            if (first_hit_thread_id == 0) {
                first_hit_thread_id = t.GetThreadID();
            }
            ++dispatch_count;
            any_wg_hit = true;
        }
    }
    return any_wg_hit;
}

void step_into_kernel(lldb::SBProcess &process, uint64_t hit_thread_id) {
    uint32_t nthreads = process.GetNumThreads();
    for (uint32_t i = 0; i < nthreads; ++i) {
        lldb::SBThread t = process.GetThreadAtIndex(i);
        if (t.IsValid() && t.GetThreadID() == hit_thread_id) {
            t.StepInto();
            // Step past workgroup wrapper prologue into the inlined kernel body
            t.StepInto();
            break;
        }
    }
}

void print_inspected_variables(const OCLWorkItem &wi, const std::vector<VarValue> &vars) {
    std::cout << std::format("[ocldbg] Inspecting variables for stopped {}:\n", wi.str());
    for (const auto &v : vars) {
        std::string addr_sp = v.address_space.empty() ? "" : " " + v.address_space;
        std::cout << std::format("  {} ({}{}) = {}\n", v.name, v.type_name, addr_sp,
                                 v.available ? v.value_str : "<unavailable>");
    }
}

std::optional<uint64_t> find_line_breakpoint_thread(lldb::SBProcess &process,
                                                    lldb::break_id_t bp_id) {
    uint32_t nthreads = process.GetNumThreads();
    for (uint32_t i = 0; i < nthreads; ++i) {
        lldb::SBThread t = process.GetThreadAtIndex(i);
        if (t.IsValid() && t.GetStopReason() == lldb::eStopReasonBreakpoint) {
            uint64_t hit_id = t.GetStopReasonDataAtIndex(0);
            if (std::cmp_equal(hit_id, bp_id)) {
                return t.GetThreadID();
            }
        }
    }
    return std::nullopt;
}

void print_breakpoint_hit(unsigned line, const std::optional<OCLWorkItem> &wi, size_t hit_count,
                          const std::vector<VarValue> &vars) {
    std::string wi_str = wi ? wi->str() : "WI(?)";
    std::cout << std::format("[ocldbg] Breakpoint hit at line {} for {} (hit {}):\n", line, wi_str,
                             hit_count);
    for (const auto &v : vars) {
        std::string addr_sp = v.address_space.empty() ? "" : " " + v.address_space;
        std::cout << std::format("  {} ({}{}) = {}\n", v.name, v.type_name, addr_sp,
                                 v.available ? v.value_str : "<unavailable>");
    }
}

bool handle_line_breakpoint(DebuggerContext &dbg, lldb::SBProcess &process, lldb::SBTarget &target,
                            lldb::SBBreakpoint &line_bp, unsigned break_at, size_t break_for,
                            size_t &line_hit_count) {
    if (!line_bp.IsValid()) {
        return false;
    }
    auto hit_tid = find_line_breakpoint_thread(process, line_bp.GetID());
    if (!hit_tid.has_value()) {
        return false;
    }
    ++line_hit_count;
    auto wi = dbg.resolve_stopped_work_item(*hit_tid);
    std::vector<VarValue> vars;
    if (wi) {
        vars = dbg.inspect_variables(*wi);
    } else {
        // Which work-item this is could not be established, but the frame's
        // variables are readable and are what the stop was for.
        process.SetSelectedThreadByID(*hit_tid);
        vars = dbg.inspect_current_frame_variables();
    }
    print_breakpoint_hit(break_at, wi, line_hit_count, vars);

    if (break_for > 0 && line_hit_count >= break_for) {
        target.BreakpointDelete(line_bp.GetID());
        line_bp = lldb::SBBreakpoint();
    }
    return true;
}

void handle_first_dispatch_inspection(DebuggerContext &dbg, lldb::SBProcess &process,
                                      uint64_t hit_thread_id, bool &inspected) {
    step_into_kernel(process, hit_thread_id);
    auto wi = dbg.resolve_stopped_work_item(hit_thread_id);
    if (wi) {
        print_inspected_variables(*wi, dbg.inspect_variables(*wi));
    }
    inspected = true;
}

bool is_kernel_module_name(std::string_view fn, const std::string &kernel_name) {
    if (!kernel_name.empty() && fn.find(kernel_name) != std::string_view::npos) {
        return true;
    }
    return fn.ends_with(".so") && !fn.starts_with("lib");
}

lldb::SBModule find_kernel_module(lldb::SBTarget &target, const std::string &kernel_name) {
    uint32_t num_mods = target.GetNumModules();
    for (uint32_t i = 0; i < num_mods; ++i) {
        lldb::SBModule m = target.GetModuleAtIndex(i);
        const char *fn = m.GetFileSpec().GetFilename();
        if (fn != nullptr && is_kernel_module_name(fn, kernel_name)) {
            return m;
        }
    }
    return {};
}

bool resolve_one_breakpoint(InternalOCLBreakpoint &bp, lldb::SBModule &kernel_mod,
                            lldb::SBTarget &target, const DWARFSourceModel &dwarf_model,
                            bool silent = false) {
    if (bp.resolved) {
        return false;
    }
    auto pcs = dwarf_model.source_to_pcs(SourceLocation{.file = bp.file, .line = bp.line});
    if (pcs.empty()) {
        return false;
    }

    lldb::SBAddress sb_addr = kernel_mod.ResolveFileAddress(pcs[0]);
    lldb::addr_t load_addr = sb_addr.GetLoadAddress(target);
    if (load_addr == LLDB_INVALID_ADDRESS) {
        return false;
    }
    bp.sb_bp = target.BreakpointCreateByAddress(load_addr);
    if (!bp.sb_bp.IsValid() || bp.sb_bp.GetNumLocations() == 0) {
        return false;
    }
    bp.resolved = true;
    bp.address = load_addr;
    auto resolved_loc = dwarf_model.pc_to_source(pcs[0]);
    if (resolved_loc.line > 0) {
        bp.line = resolved_loc.line;
    }
    std::string fname = bp.file.empty() ? "<kernel>" : bp.file;
    if (!silent) {
        std::cout << std::format("[ocldbg] Breakpoint #{}: resolved at address {:#x} ({}:{})\n",
                                 bp.id, load_addr, fname, bp.line);
    }
    return true;
}

} // namespace

std::string DebuggerContext::init() {
    lldb::SBDebugger::Initialize();
    return lldb::SBDebugger::GetVersionString();
}

void DebuggerContext::terminate() {
    lldb::SBDebugger::Terminate();
}

DebuggerContext::DebuggerContext() : impl_(std::make_unique<Impl>()) {
    impl_->debugger = lldb::SBDebugger::Create();
    impl_->debugger.SetAsync(false);
}

DebuggerContext::~DebuggerContext() {
    terminate_process();
}

DebuggerContext::DebuggerContext(DebuggerContext &&) noexcept = default;
DebuggerContext &DebuggerContext::operator=(DebuggerContext &&) noexcept = default;

bool DebuggerContext::launch(const std::string &host_binary, const std::vector<std::string> &args,
                             bool stop_at_entry) {
    impl_->target = impl_->debugger.CreateTarget(host_binary.c_str());
    if (!impl_->target.IsValid()) {
        std::cerr << "Failed to create target for: " << host_binary << "\n";
        return false;
    }

    impl_->target.BreakpointCreateByName("clEnqueueNDRangeKernel");

    std::vector<const char *> c_args;
    c_args.reserve(args.size() + 1);
    for (const auto &arg : args) {
        c_args.push_back(arg.c_str());
    }
    c_args.push_back(nullptr);

    lldb::SBLaunchInfo launch_info = impl_->target.GetLaunchInfo();
    launch_info.SetArguments(c_args.data(), true);
    if (stop_at_entry) {
        launch_info.SetLaunchFlags(launch_info.GetLaunchFlags() | lldb::eLaunchFlagStopAtEntry);
    }
    lldb::SBError error;
    impl_->process = impl_->target.Launch(launch_info, error);

    if (!impl_->process.IsValid() || !error.Success()) {
        std::cerr << "Failed to launch host binary: " << error.GetCString() << "\n";
        return false;
    }

    return true;
}

bool DebuggerContext::infer_kernel_launch() {
    if (!impl_->process.IsValid() || impl_->process.GetState() != lldb::eStateStopped) {
        return false;
    }

    lldb::SBThread thread = impl_->process.GetSelectedThread();
    lldb::SBFrame frame = thread.GetSelectedFrame();
    if (!frame.IsValid()) {
        frame = thread.GetFrameAtIndex(0);
    }

    Size3 global_size{.x = 1, .y = 1, .z = 1};
    Size3 local_size{.x = 1, .y = 1, .z = 1};
    size_t work_dim = 1;

    if (!impl_->abi || !impl_->abi->read_enqueue_ndrange(impl_->process, frame, global_size,
                                                         local_size, &work_dim)) {
        return false;
    }

    size_t lx = (local_size.x > 0) ? local_size.x : 1;
    size_t ly = (local_size.y > 0) ? local_size.y : 1;
    size_t lz = (local_size.z > 0) ? local_size.z : 1;

    Size3 num_wg{.x = ((global_size.x + lx) - 1) / lx,
                 .y = ((global_size.y + ly) - 1) / ly,
                 .z = ((global_size.z + lz) - 1) / lz};

    std::string kname;
    if (impl_->abi->read_enqueue_kernel_name(impl_->process, frame, kname)) {
        impl_->kernel_name = kname;
    }

    KernelLaunchInfo info{
        .kernel_name = impl_->kernel_name,
        .work_dim = work_dim,
        .global_size = global_size,
        .local_size = local_size,
        .num_groups = num_wg,
        .work_groups = compute_work_groups(global_size, local_size, num_wg),
        .sample_work_items = compute_sample_work_items(global_size, local_size),
    };

    impl_->launch_info = info;
    impl_->wg_tracker.set_ndrange(global_size, local_size);
    return true;
}

bool DebuggerContext::set_workgroup_breakpoint(const std::string &kernel_name) {
    if (!impl_->target.IsValid()) {
        return false;
    }
    if (!kernel_name.empty()) {
        impl_->kernel_name = kernel_name;
    }

    // Clean up any previously set workgroup breakpoint.
    if (impl_->wg_breakpoint.IsValid()) {
        impl_->target.BreakpointDelete(impl_->wg_breakpoint.GetID());
    }

    // First try locating the PoCL work-group dispatch call in libpocl-devices-pthread.so.
    if (auto addr = find_pocl_dispatch_address(impl_->target)) {
        impl_->wg_breakpoint = impl_->target.BreakpointCreateByAddress(*addr);
        if (impl_->wg_breakpoint.IsValid() && impl_->wg_breakpoint.GetNumLocations() > 0) {
            return true;
        }
    }

    // Both fallbacks below resolve only once pocl dlopens the kernel module, which
    // it does after compiling the kernel, so having no location yet is not failure.
    //
    // Prologue skipping has to be off for them. pocl inlines the kernel body into
    // the work-group function, so its first line-table entry is already inside the
    // per-work-item loop; a breakpoint placed there fires once per work-item rather
    // than once per work-group, and the group ids have left their argument
    // registers by then. The resolver captures this setting when the breakpoint is
    // created, so restoring it does not affect that later resolution.
    const char *instance = impl_->debugger.GetInstanceName();
    lldb::SBDebugger::SetInternalVariable("target.skip-prologue", "false", instance);

    if (!impl_->kernel_name.empty()) {
        std::string sym = std::format("_pocl_kernel_{}_workgroup", impl_->kernel_name);
        impl_->wg_breakpoint = impl_->target.BreakpointCreateByName(sym.c_str());
    } else {
        // Without a kernel name, match any work-group function pocl emits.
        impl_->wg_breakpoint = impl_->target.BreakpointCreateByRegex("_pocl_kernel_.*_workgroup");
    }

    lldb::SBDebugger::SetInternalVariable("target.skip-prologue", "true", instance);
    return impl_->wg_breakpoint.IsValid();
}

size_t DebuggerContext::track_workgroup_dispatches(bool inspect_vars, unsigned break_at,
                                                   size_t break_for) {
    if (!impl_->process.IsValid() || !impl_->abi) {
        return 0;
    }
    if (!impl_->wg_breakpoint.IsValid()) {
        if (!set_workgroup_breakpoint(impl_->kernel_name)) {
            return 0;
        }
    }

    impl_->wg_tracker.clear();

    size_t dispatch_count = 0;
    bool inspected = false;
    size_t line_hit_count = 0;

    while (true) {
        impl_->process.Continue();

        lldb::StateType state = impl_->process.GetState();
        if (state == lldb::eStateExited || state == lldb::eStateCrashed) {
            break;
        }
        if (state != lldb::eStateStopped) {
            continue;
        }

        if (handle_line_breakpoint(*this, impl_->process, impl_->target, impl_->line_breakpoint,
                                   break_at, break_for, line_hit_count)) {
            continue;
        }

        uint64_t hit_thread_id = 0;
        if (!record_stopped_workgroups(impl_->process, impl_->abi.get(), impl_->wg_tracker,
                                       dispatch_count, hit_thread_id)) {
            break;
        }

        if (break_at > 0 && !impl_->line_breakpoint.IsValid() &&
            (break_for == 0 || line_hit_count < break_for)) {
            set_source_breakpoint(break_at);
        }

        if (inspect_vars && !inspected && hit_thread_id != 0) {
            handle_first_dispatch_inspection(*this, impl_->process, hit_thread_id, inspected);
        }
    }

    if (impl_->line_breakpoint.IsValid()) {
        impl_->target.BreakpointDelete(impl_->line_breakpoint.GetID());
    }
    if (impl_->wg_breakpoint.IsValid()) {
        impl_->target.BreakpointDelete(impl_->wg_breakpoint.GetID());
    }

    return dispatch_count;
}

bool DebuggerContext::set_source_breakpoint(unsigned line) {
    load_kernel_dwarf();
    if (!impl_->target.IsValid()) {
        return false;
    }

    // pocl compiles each kernel into its own module, so a line belongs to
    // whichever kernel declares it and the model loaded for one kernel cannot
    // answer for another. A program with several kernels -- FFmpeg's filters
    // routinely have them -- needs every kernel module asked, not just the
    // first one that happened to load.
    std::vector<HostAddress> pcs;
    lldb::SBModule kernel_mod;
    if (impl_->dwarf_model.loaded()) {
        pcs = impl_->dwarf_model.source_to_pcs(SourceLocation{.file = "", .line = line});
        if (!pcs.empty()) {
            kernel_mod = find_kernel_module(impl_->target, impl_->kernel_name);
        }
    }

    for (uint32_t i = 0; pcs.empty() && i < impl_->target.GetNumModules(); ++i) {
        lldb::SBModule m = impl_->target.GetModuleAtIndex(i);
        const char *fn = m.GetFileSpec().GetFilename();
        if (fn == nullptr || !is_kernel_module_name(fn, {})) {
            continue;
        }
        std::array<char, 1024> path{};
        if (m.GetFileSpec().GetPath(path.data(), path.size()) == 0) {
            continue;
        }
        if (!impl_->dwarf_model.load(path.data())) {
            continue;
        }
        pcs = impl_->dwarf_model.source_to_pcs(SourceLocation{.file = "", .line = line});
        if (!pcs.empty()) {
            kernel_mod = m;
        }
    }

    if (pcs.empty() || !kernel_mod.IsValid()) {
        return false;
    }

    // One source line can lower to several addresses; a breakpoint on only the
    // first misses the rest of them.
    std::vector<lldb::addr_t> load_addrs;
    for (HostAddress pc : pcs) {
        lldb::addr_t load_addr = kernel_mod.ResolveFileAddress(pc).GetLoadAddress(impl_->target);
        if (load_addr != LLDB_INVALID_ADDRESS) {
            load_addrs.push_back(load_addr);
        }
    }
    if (load_addrs.empty()) {
        return false;
    }

    impl_->line_breakpoint = impl_->target.BreakpointCreateByAddress(load_addrs.front());
    return impl_->line_breakpoint.IsValid() && impl_->line_breakpoint.GetNumLocations() > 0;
}

bool DebuggerContext::load_kernel_dwarf() {
    if (impl_->dwarf_model.loaded()) {
        return true;
    }
    if (!impl_->target.IsValid()) {
        impl_->target = impl_->debugger.GetSelectedTarget();
    }
    if (!impl_->target.IsValid()) {
        return false;
    }
    lldb::SBModule m = find_kernel_module(impl_->target, impl_->kernel_name);
    if (!m.IsValid()) {
        return false;
    }
    std::array<char, 1024> path{};
    if (m.GetFileSpec().GetPath(path.data(), path.size()) > 0) {
        return impl_->dwarf_model.load(path.data());
    }
    return false;
}

void DebuggerContext::ensure_ocl_trampoline() {
    if (!impl_->target.IsValid()) {
        impl_->target = impl_->debugger.GetSelectedTarget();
    }
    if (!impl_->target.IsValid()) {
        return;
    }
    if (impl_->ocl_trampoline_bp.IsValid()) {
        return;
    }
    impl_->ocl_trampoline_bp = impl_->target.BreakpointCreateByRegex(".*_workgroup");
    if (!impl_->ocl_trampoline_bp.IsValid()) {
        return;
    }
    impl_->ocl_trampoline_bp.SetCallback(
        [](void *baton, lldb::SBProcess & /*process*/, lldb::SBThread &thread,
           lldb::SBBreakpointLocation & /*location*/) -> bool {
            auto *dbg = static_cast<DebuggerContext *>(baton);
            if (dbg != nullptr) {
                dbg->resolve_pending_ocl_breakpoints();
                lldb::SBFrame f = thread.GetSelectedFrame();
                if (!f.IsValid()) {
                    f = thread.GetFrameAtIndex(0);
                }
                Size3 wg_id;
                if (dbg->impl_->abi && dbg->impl_->abi->read_workgroup_id(f, wg_id)) {
                    dbg->impl_->wg_tracker.record_wg(thread.GetThreadID(), wg_id);
                }
            }
            return false;
        },
        this);
}

static Size3 get_local_dims(const std::optional<KernelLaunchInfo> &info) {
    if (info.has_value()) {
        const auto &ls = info->local_size;
        return Size3{
            .x = (ls.x > 0) ? ls.x : 1, .y = (ls.y > 0) ? ls.y : 1, .z = (ls.z > 0) ? ls.z : 1};
    }
    return Size3{.x = 4, .y = 1, .z = 1};
}

static bool match_wg(const Size3 &wg_id, const Size3 &target_wg) {
    return wg_id.x == target_wg.x && (target_wg.y == 0 || wg_id.y == target_wg.y) &&
           (target_wg.z == 0 || wg_id.z == target_wg.z);
}

static lldb::SBThread find_thread_for_target_wg(lldb::SBProcess &proc, CPUABI *abi,
                                                WorkGroupTracker &tracker, const Size3 &target_wg,
                                                const Size3 &global_id) {
    uint32_t num_threads = proc.GetNumThreads();
    for (uint32_t i = 0; i < num_threads; ++i) {
        lldb::SBThread t = proc.GetThreadAtIndex(i);
        if (!t.IsValid() || t.GetStopReason() == lldb::eStopReasonNone) {
            continue;
        }
        lldb::SBFrame f = t.GetFrameAtIndex(0);
        if (!f.IsValid()) {
            continue;
        }

        Size3 wg_id;
        if (abi != nullptr && abi->read_workgroup_id(f, wg_id)) {
            tracker.record_wg(t.GetThreadID(), wg_id);
            if (match_wg(wg_id, target_wg)) {
                return t;
            }
        }
    }

    uint64_t tid = tracker.host_thread_for_wi(global_id);
    if (tid != 0) {
        for (uint32_t i = 0; i < num_threads; ++i) {
            lldb::SBThread t = proc.GetThreadAtIndex(i);
            if (t.IsValid() && t.GetThreadID() == tid &&
                t.GetStopReason() != lldb::eStopReasonNone) {
                return t;
            }
        }
    }

    return {};
}

bool DebuggerContext::select_work_item(const Size3 &global_id) {
    if (!impl_->target.IsValid()) {
        impl_->target = impl_->debugger.GetSelectedTarget();
    }
    if (!impl_->target.IsValid()) {
        return false;
    }
    impl_->process = impl_->target.GetProcess();
    if (!impl_->process.IsValid() || impl_->process.GetState() != lldb::eStateStopped) {
        return false;
    }

    Size3 local_dims = get_local_dims(impl_->launch_info);
    Size3 target_wg{.x = global_id.x / local_dims.x,
                    .y = global_id.y / local_dims.y,
                    .z = global_id.z / local_dims.z};
    Size3 target_local{.x = global_id.x % local_dims.x,
                       .y = global_id.y % local_dims.y,
                       .z = global_id.z % local_dims.z};

    lldb::SBThread matched_thread = find_thread_for_target_wg(
        impl_->process, impl_->abi.get(), impl_->wg_tracker, target_wg, global_id);

    if (!matched_thread.IsValid()) {
        return false;
    }

    impl_->process.SetSelectedThread(matched_thread);
    lldb::SBFrame frame = matched_thread.GetSelectedFrame();
    if (!frame.IsValid()) {
        frame = matched_thread.GetFrameAtIndex(0);
    }

    OCLWorkItem wi;
    if (impl_->launch_info.has_value() && impl_->launch_info->work_dim > 0) {
        wi.work_dim = impl_->launch_info->work_dim;
    }
    wi.global_id = global_id;
    wi.group_id = target_wg;
    wi.local_id = target_local;
    auto ctx = std::make_shared<CPUExecContext>();
    ctx->host_thread_id = matched_thread.GetThreadID();
    ctx->thread = matched_thread;
    ctx->frame = frame;
    wi.exec_ctx_storage = ctx;
    wi.exec_ctx = ctx.get();

    impl_->selected_work_item = wi;
    return true;
}

std::optional<OCLWorkItem> DebuggerContext::get_selected_work_item() const {
    return impl_->selected_work_item;
}

std::vector<OCLWorkItem> DebuggerContext::list_stopped_work_items() const {
    std::vector<OCLWorkItem> result;
    if (!impl_->target.IsValid()) {
        impl_->target = impl_->debugger.GetSelectedTarget();
    }
    if (!impl_->target.IsValid()) {
        return result;
    }
    impl_->process = impl_->target.GetProcess();
    if (!impl_->process.IsValid() || impl_->process.GetState() != lldb::eStateStopped) {
        return result;
    }

    size_t lx = 1;
    size_t work_dim = 1;
    if (impl_->launch_info.has_value() && impl_->launch_info->local_size.x > 0) {
        lx = impl_->launch_info->local_size.x;
        work_dim = impl_->launch_info->work_dim;
    } else {
        lx = 4;
    }

    uint32_t num_threads = impl_->process.GetNumThreads();
    for (uint32_t i = 0; i < num_threads; ++i) {
        lldb::SBThread t = impl_->process.GetThreadAtIndex(i);
        if (!t.IsValid() || t.GetStopReason() == lldb::eStopReasonNone) {
            continue;
        }
        lldb::SBFrame f = t.GetFrameAtIndex(0);
        if (!f.IsValid()) {
            continue;
        }

        OCLWorkItem wi;
        wi.work_dim = work_dim;
        Size3 wg_id;
        bool has_wg = (impl_->abi != nullptr) && impl_->abi->read_workgroup_id(f, wg_id);
        if (has_wg) {
            impl_->wg_tracker.record_wg(t.GetThreadID(), wg_id);
        } else {
            wg_id = impl_->wg_tracker.wg_for_thread(t.GetThreadID());
        }

        wi.group_id = Size3{.x = wg_id.x, .y = (wg_id.y < 0x100000) ? wg_id.y : 0, .z = 0};
        Size3 loc_id;
        if (impl_->abi != nullptr &&
            impl_->abi->read_local_id(f, Size3{.x = lx, .y = 1, .z = 1}, loc_id)) {
            wi.local_id = loc_id;
        } else {
            wi.local_id = Size3{.x = 0, .y = 0, .z = 0};
        }
        wi.global_id = Size3{.x = (wi.group_id.x * lx) + wi.local_id.x,
                             .y = (wi.group_id.y * 1) + wi.local_id.y,
                             .z = (wi.group_id.z * 1) + wi.local_id.z};

        auto ctx = std::make_shared<CPUExecContext>();
        ctx->host_thread_id = t.GetThreadID();
        ctx->thread = t;
        ctx->frame = f;
        wi.exec_ctx_storage = ctx;
        wi.exec_ctx = ctx.get();

        result.push_back(wi);
    }
    return result;
}

size_t DebuggerContext::add_ocl_breakpoint(const std::string &file, unsigned line) {
    if (!impl_->target.IsValid()) {
        impl_->target = impl_->debugger.GetSelectedTarget();
    }
    size_t id = impl_->next_ocl_bp_id++;
    impl_->ocl_breakpoints.push_back(InternalOCLBreakpoint{
        .id = id,
        .file = file,
        .line = line,
        .resolved = false,
        .address = 0,
        .sb_bp = {},
    });

    resolve_pending_ocl_breakpoints();
    ensure_ocl_trampoline();
    return id;
}

bool DebuggerContext::delete_ocl_breakpoint(size_t id) {
    auto it = std::find_if(impl_->ocl_breakpoints.begin(), impl_->ocl_breakpoints.end(),
                           [id](const auto &bp) { return bp.id == id; });
    if (it == impl_->ocl_breakpoints.end()) {
        return false;
    }
    if (it->sb_bp.IsValid() && impl_->target.IsValid()) {
        impl_->target.BreakpointDelete(it->sb_bp.GetID());
    }
    impl_->ocl_breakpoints.erase(it);
    return true;
}

std::vector<OCLBreakpoint> DebuggerContext::list_ocl_breakpoints() const {
    std::vector<OCLBreakpoint> result;
    result.reserve(impl_->ocl_breakpoints.size());
    for (const auto &bp : impl_->ocl_breakpoints) {
        result.push_back(OCLBreakpoint{
            .id = bp.id,
            .file = bp.file,
            .line = bp.line,
            .resolved = bp.resolved,
            .address = bp.address,
        });
    }
    return result;
}

static bool resolve_host_breakpoint(InternalOCLBreakpoint &bp, lldb::SBTarget &target,
                                    bool redirect_stderr) {
    lldb::SBBreakpoint sb_bp = target.BreakpointCreateByLocation(bp.file.c_str(), bp.line);
    if (!sb_bp.IsValid() || sb_bp.GetNumLocations() == 0) {
        return false;
    }
    bp.sb_bp = sb_bp;
    bp.resolved = true;
    bp.address = sb_bp.GetLocationAtIndex(0).GetAddress().GetLoadAddress(target);
    if (!redirect_stderr) {
        std::cout << std::format("[ocldbg] Breakpoint #{}: resolved at {}:{}\n", bp.id, bp.file,
                                 bp.line);
    }
    return true;
}

bool DebuggerContext::resolve_pending_ocl_breakpoints() {
    if (!impl_->target.IsValid()) {
        impl_->target = impl_->debugger.GetSelectedTarget();
    }
    if (!impl_->target.IsValid()) {
        return false;
    }

    bool any_resolved = false;
    for (auto &bp : impl_->ocl_breakpoints) {
        if (bp.resolved) {
            continue;
        }
        bool is_kernel = bp.file.empty() || bp.file.ends_with(".cl");
        if (!is_kernel && resolve_host_breakpoint(bp, impl_->target, impl_->redirect_stderr)) {
            any_resolved = true;
        }
    }

    if (!impl_->dwarf_model.loaded()) {
        load_kernel_dwarf();
    }
    if (!impl_->dwarf_model.loaded()) {
        return any_resolved;
    }

    lldb::SBModule kernel_mod = find_kernel_module(impl_->target, impl_->kernel_name);
    if (!kernel_mod.IsValid()) {
        return any_resolved;
    }

    for (auto &bp : impl_->ocl_breakpoints) {
        if (bp.resolved) {
            continue;
        }
        bool is_kernel = bp.file.empty() || bp.file.ends_with(".cl");
        if (is_kernel && resolve_one_breakpoint(bp, kernel_mod, impl_->target, impl_->dwarf_model,
                                                impl_->redirect_stderr)) {
            any_resolved = true;
        }
    }
    return any_resolved;
}

std::vector<VarValue> DebuggerContext::inspect_variables(const OCLWorkItem &wi) {
    load_kernel_dwarf();
    PoclCPUBackend backend;
    OCLVariableResolver resolver(impl_->dwarf_model, impl_->address_spaces);
    return resolver.resolve(wi, backend);
}

std::vector<VarValue> DebuggerContext::inspect_current_frame_variables() {
    if (!impl_->target.IsValid()) {
        impl_->target = impl_->debugger.GetSelectedTarget();
    }
    if (!impl_->target.IsValid()) {
        return {};
    }
    impl_->process = impl_->target.GetProcess();
    if (!impl_->process.IsValid() || impl_->process.GetState() != lldb::eStateStopped) {
        return {};
    }
    if (impl_->selected_work_item.has_value() && impl_->selected_work_item->exec_ctx != nullptr) {
        return inspect_variables(*impl_->selected_work_item);
    }

    lldb::SBThread thread = impl_->process.GetSelectedThread();
    if (!thread.IsValid() || thread.GetStopReason() == lldb::eStopReasonNone) {
        uint32_t nthreads = impl_->process.GetNumThreads();
        for (uint32_t i = 0; i < nthreads; ++i) {
            lldb::SBThread t = impl_->process.GetThreadAtIndex(i);
            if (t.IsValid() && t.GetStopReason() != lldb::eStopReasonNone) {
                thread = t;
                break;
            }
        }
    }
    if (!thread.IsValid()) {
        return {};
    }

    lldb::SBFrame frame = thread.GetSelectedFrame();
    if (!frame.IsValid()) {
        frame = thread.GetFrameAtIndex(0);
    }
    if (!frame.IsValid()) {
        return {};
    }

    OCLWorkItem wi;
    auto resolved = resolve_stopped_work_item(thread.GetThreadID());
    if (resolved.has_value()) {
        wi = *resolved;
    } else {
        auto ctx = std::make_shared<CPUExecContext>();
        ctx->host_thread_id = thread.GetThreadID();
        ctx->frame = frame;
        ctx->thread = thread;
        wi.exec_ctx_storage = ctx;
        wi.exec_ctx = ctx.get();
    }

    return inspect_variables(wi);
}

std::optional<VarValue> DebuggerContext::get_variable_value(const std::string &name) {
    auto vars = inspect_current_frame_variables();
    for (const auto &v : vars) {
        if (v.name == name) {
            return v;
        }
    }
    return std::nullopt;
}

std::optional<OCLWorkItem> DebuggerContext::resolve_stopped_work_item(uint64_t thread_id) const {
    if (!impl_->launch_info || !impl_->abi) {
        return std::nullopt;
    }

    const Size3 &local_size = impl_->launch_info->local_size;
    Size3 wg_id = impl_->wg_tracker.wg_for_thread(thread_id);

    // Find the thread in the process.
    uint32_t num_threads = impl_->process.GetNumThreads();
    for (uint32_t i = 0; i < num_threads; ++i) {
        lldb::SBThread t = impl_->process.GetThreadAtIndex(i);
        if (!t.IsValid() || t.GetThreadID() != thread_id) {
            continue;
        }
        WIContextExtractor extractor;
        OCLWorkItem item;
        if (extractor.extract_from_thread(t, wg_id, local_size, item)) {
            item.work_dim = impl_->launch_info->work_dim;
            return item;
        }
        break;
    }
    return std::nullopt;
}

const std::optional<KernelLaunchInfo> &DebuggerContext::kernel_launch_info() const {
    return impl_->launch_info;
}

static void record_stopped_workgroups(lldb::SBProcess &process, CPUABI *abi,
                                      WorkGroupTracker &wg_tracker) {
    if (abi == nullptr) {
        return;
    }
    uint32_t num_threads = process.GetNumThreads();
    for (uint32_t i = 0; i < num_threads; ++i) {
        lldb::SBThread t = process.GetThreadAtIndex(i);
        if (t.IsValid() && t.GetStopReason() != lldb::eStopReasonNone) {
            lldb::SBFrame f = t.GetFrameAtIndex(0);
            Size3 wg_id;
            if (abi->read_workgroup_id(f, wg_id)) {
                wg_tracker.record_wg(t.GetThreadID(), wg_id);
            }
        }
    }
}

static bool check_enqueue_ndrange(lldb::SBProcess &process, CPUABI *abi) {
    if (abi == nullptr) {
        return false;
    }
    uint32_t num_threads = process.GetNumThreads();
    for (uint32_t i = 0; i < num_threads; ++i) {
        lldb::SBThread t = process.GetThreadAtIndex(i);
        if (t.IsValid() && t.GetStopReason() != lldb::eStopReasonNone) {
            lldb::SBFrame f = t.GetFrameAtIndex(0);
            Size3 g_sz;
            Size3 l_sz;
            if (abi->read_enqueue_ndrange(process, f, g_sz, l_sz)) {
                return true;
            }
        }
    }
    return false;
}

static bool is_breakpoint_hit(lldb::break_id_t hit_id,
                              const std::vector<InternalOCLBreakpoint> &breakpoints) {
    return std::ranges::any_of(breakpoints, [hit_id](const auto &bp) {
        return bp.sb_bp.IsValid() && bp.sb_bp.GetID() == hit_id;
    });
}

static bool check_user_stop(lldb::SBProcess &process,
                            const std::vector<InternalOCLBreakpoint> &breakpoints) {
    uint32_t num_threads = process.GetNumThreads();
    for (uint32_t i = 0; i < num_threads; ++i) {
        lldb::SBThread t = process.GetThreadAtIndex(i);
        if (!t.IsValid() || t.GetStopReason() == lldb::eStopReasonNone) {
            continue;
        }
        lldb::StopReason reason = t.GetStopReason();
        if (reason == lldb::eStopReasonPlanComplete || reason == lldb::eStopReasonSignal ||
            reason == lldb::eStopReasonException) {
            return true;
        }
        if (reason == lldb::eStopReasonBreakpoint) {
            auto hit_bp_id = static_cast<lldb::break_id_t>(t.GetStopReasonDataAtIndex(0));
            if (is_breakpoint_hit(hit_bp_id, breakpoints)) {
                return true;
            }
        }
    }
    return false;
}

bool DebuggerContext::continue_execution() {
    if (!impl_->target.IsValid()) {
        impl_->target = impl_->debugger.GetSelectedTarget();
    }
    if (!impl_->target.IsValid()) {
        return false;
    }
    impl_->process = impl_->target.GetProcess();
    if (!impl_->process.IsValid()) {
        return false;
    }

    ensure_ocl_trampoline();

    while (true) {
        impl_->process.Continue();
        lldb::StateType state = impl_->process.GetState();
        if (state == lldb::eStateExited || state == lldb::eStateCrashed) {
            return false;
        }
        if (state != lldb::eStateStopped) {
            continue;
        }

        record_stopped_workgroups(impl_->process, impl_->abi.get(), impl_->wg_tracker);

        if (!impl_->launch_info.has_value() &&
            check_enqueue_ndrange(impl_->process, impl_->abi.get())) {
            infer_kernel_launch();
            if (!impl_->kernel_name.empty()) {
                set_workgroup_breakpoint(impl_->kernel_name);
            }
            ensure_ocl_trampoline();
        }

        resolve_pending_ocl_breakpoints();

        if (check_user_stop(impl_->process, impl_->ocl_breakpoints)) {
            auto stopped = list_stopped_work_items();
            if (!stopped.empty()) {
                return true;
            }
        }
    }
}

size_t DebuggerContext::read_memory(HostAddress addr, void *buf, size_t length) {
    if (!impl_->process.IsValid()) {
        return 0;
    }
    lldb::SBError error;
    return impl_->process.ReadMemory(addr, buf, length, error);
}

void DebuggerContext::terminate_process() {
    if (impl_->process.IsValid()) {
        impl_->process.Kill();
    }
}

void DebuggerContext::redirect_output_to_stderr() {
    impl_->redirect_stderr = true;
    if (impl_->debugger.IsValid()) {
        impl_->debugger.SetOutputFileHandle(stderr, false);
        impl_->debugger.SetErrorFileHandle(stderr, false);
    }
}

int DebuggerContext::run_dap(const std::string &backend_name, uint16_t dap_port) {
    std::unique_ptr<Backend> backend;
    if (backend_name == "oclgrind") {
        backend = std::make_unique<OclgrindBackend>();
    } else {
        backend = std::make_unique<PoclCPUBackend>();
    }

    DWARFSourceModel dwarf;
    OCLAddressSpaces address_spaces;
    OCLVariableResolver resolver(dwarf, address_spaces);
    DAPServer dap(*backend, dwarf, resolver);

    if (dap_port > 0) {
        dap.run_tcp(dap_port);
    } else {
        dap.run_stdio();
    }
    return 0;
}

} // namespace ocldbg
