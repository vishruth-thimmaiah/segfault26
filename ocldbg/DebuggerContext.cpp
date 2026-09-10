#include "ocldbg/DebuggerContext.h"

#include "backends/cpu/CPUABI.h"
#include "backends/cpu/PoclCPUBackend.h"
#include "backends/oclgrind/OclgrindBackend.h"
#include "dap/DAPServer.h"
#include "dwarf/DWARFSourceModel.h"
#include "ocl_debug_model/OCLVariableResolver.h"

#include <algorithm>
#include <iostream>
#include <lldb/API/SBBreakpoint.h>
#include <lldb/API/SBDebugger.h>
#include <lldb/API/SBError.h>
#include <lldb/API/SBFrame.h>
#include <lldb/API/SBLaunchInfo.h>
#include <lldb/API/SBProcess.h>
#include <lldb/API/SBTarget.h>
#include <lldb/API/SBThread.h>
#include <memory>
#include <string>
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

} // namespace

struct DebuggerContext::Impl {
    lldb::SBDebugger debugger;
    lldb::SBTarget target;
    lldb::SBProcess process;
    std::optional<KernelLaunchInfo> launch_info;
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

    lldb::SBLaunchInfo launch_info(c_args.data());
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

    std::unique_ptr<CPUABI> abi = CPUABI::create_host_abi();
    Size3 global_size{.x = 1, .y = 1, .z = 1};
    Size3 local_size{.x = 1, .y = 1, .z = 1};

    if (!abi || !abi->read_enqueue_ndrange(impl_->process, frame, global_size, local_size)) {
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
    return true;
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
