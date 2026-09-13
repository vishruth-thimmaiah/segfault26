#include "ocldbg/DebuggerContext.h"

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

} // namespace

struct DebuggerContext::Impl {
    lldb::SBDebugger debugger;
    lldb::SBTarget target;
    lldb::SBProcess process;
    lldb::SBBreakpoint wg_breakpoint;
    std::optional<KernelLaunchInfo> launch_info;
    std::string kernel_name;
    WorkGroupTracker wg_tracker;
    std::unique_ptr<CPUABI> abi{CPUABI::create_host_abi()};
    DWARFSourceModel dwarf_model;
};

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

bool DebuggerContext::launch(const std::string &host_binary, const std::vector<std::string> &args) {
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

    if (!impl_->abi ||
        !impl_->abi->read_enqueue_ndrange(impl_->process, frame, global_size, local_size)) {
        return false;
    }

    size_t lx = (local_size.x > 0) ? local_size.x : 1;
    size_t ly = (local_size.y > 0) ? local_size.y : 1;
    size_t lz = (local_size.z > 0) ? local_size.z : 1;

    Size3 num_wg{.x = ((global_size.x + lx) - 1) / lx,
                 .y = ((global_size.y + ly) - 1) / ly,
                 .z = ((global_size.z + lz) - 1) / lz};

    KernelLaunchInfo info{
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
    if (kernel_name.empty() || !impl_->target.IsValid()) {
        return false;
    }
    impl_->kernel_name = kernel_name;

    // Clean up any previously set workgroup breakpoint.
    if (impl_->wg_breakpoint.IsValid()) {
        impl_->target.BreakpointDelete(impl_->wg_breakpoint.GetID());
    }

    // First try locating the PoCL work-group dispatch call in libpocl-devices-pthread.so.
    if (auto addr = find_pocl_dispatch_address(impl_->target)) {
        impl_->wg_breakpoint = impl_->target.BreakpointCreateByAddress(*addr);
        return impl_->wg_breakpoint.IsValid() && impl_->wg_breakpoint.GetNumLocations() > 0;
    }

    // Fall back to direct symbol name if libpocl-devices-pthread is not in use.
    std::string sym = std::format("_pocl_kernel_{}_workgroup", kernel_name);
    impl_->wg_breakpoint = impl_->target.BreakpointCreateByName(sym.c_str());
    return impl_->wg_breakpoint.IsValid() && impl_->wg_breakpoint.GetNumLocations() > 0;
}

size_t DebuggerContext::track_workgroup_dispatches(bool inspect_vars) {
    if (!impl_->process.IsValid() || !impl_->abi) {
        return 0;
    }

    // Ensure work-group breakpoint is set.
    if (!impl_->wg_breakpoint.IsValid() || impl_->wg_breakpoint.GetNumLocations() == 0) {
        if (!set_workgroup_breakpoint(impl_->kernel_name)) {
            return 0;
        }
    }

    impl_->wg_tracker.clear();

    size_t dispatch_count = 0;
    bool inspected = false;

    while (true) {
        impl_->process.Continue();

        lldb::StateType state = impl_->process.GetState();
        if (state == lldb::eStateExited || state == lldb::eStateCrashed) {
            break;
        }
        if (state != lldb::eStateStopped) {
            continue;
        }

        uint64_t hit_thread_id = 0;
        if (!record_stopped_workgroups(impl_->process, impl_->abi.get(), impl_->wg_tracker,
                                       dispatch_count, hit_thread_id)) {
            break;
        }

        if (inspect_vars && !inspected && hit_thread_id != 0) {
            uint32_t nthreads = impl_->process.GetNumThreads();
            lldb::SBThread target_thread;
            for (uint32_t i = 0; i < nthreads; ++i) {
                lldb::SBThread t = impl_->process.GetThreadAtIndex(i);
                if (t.IsValid() && t.GetThreadID() == hit_thread_id) {
                    target_thread = t;
                    break;
                }
            }
            if (target_thread.IsValid()) {
                target_thread.StepInto();
                // Step past workgroup wrapper prologue into the inlined kernel body
                target_thread.StepInto();
            }

            auto wi = resolve_stopped_work_item(hit_thread_id);
            if (wi) {
                auto vars = inspect_variables(*wi);
                std::cout << std::format("[ocldbg] Inspecting variables for stopped {}:\n",
                                         wi->str());
                for (const auto &v : vars) {
                    std::string addr_sp = v.address_space.empty() ? "" : " " + v.address_space;
                    std::cout << std::format("  {} ({}{}) = {}\n", v.name, v.type_name, addr_sp,
                                             v.available ? v.value_str : "<unavailable>");
                }
            }
            inspected = true;
        }
    }

    if (impl_->wg_breakpoint.IsValid()) {
        impl_->target.BreakpointDelete(impl_->wg_breakpoint.GetID());
    }

    return dispatch_count;
}

bool DebuggerContext::load_kernel_dwarf() {
    if (impl_->dwarf_model.loaded()) {
        return true;
    }
    uint32_t num_mods = impl_->target.GetNumModules();
    for (uint32_t i = 0; i < num_mods; ++i) {
        lldb::SBModule m = impl_->target.GetModuleAtIndex(i);
        const char *fn = m.GetFileSpec().GetFilename();
        if (fn != nullptr) {
            std::string_view fn_sv(fn);
            if (fn_sv.find(impl_->kernel_name) != std::string_view::npos) {
                std::array<char, 1024> path{};
                if (m.GetFileSpec().GetPath(path.data(), path.size()) > 0) {
                    bool loaded = impl_->dwarf_model.load(path.data());
                    return loaded;
                }
            }
        }
    }
    return false;
}

std::vector<VarValue> DebuggerContext::inspect_variables(const OCLWorkItem &wi) {
    load_kernel_dwarf();
    PoclCPUBackend backend;
    OCLVariableResolver resolver(impl_->dwarf_model);
    return resolver.resolve(wi, backend);
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
            return item;
        }
        break;
    }
    return std::nullopt;
}

const std::optional<KernelLaunchInfo> &DebuggerContext::kernel_launch_info() const {
    return impl_->launch_info;
}

void DebuggerContext::terminate_process() {
    if (impl_->process.IsValid()) {
        impl_->process.Kill();
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
    OCLVariableResolver resolver(dwarf);
    DAPServer dap(*backend, dwarf, resolver);

    if (dap_port > 0) {
        dap.run_tcp(dap_port);
    } else {
        dap.run_stdio();
    }
    return 0;
}

} // namespace ocldbg
