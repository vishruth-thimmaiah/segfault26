#include "PoclCPUBackend.h"

#include "WIContextExtractor.h"
#include "WorkGroupTracker.h"

#include <stdexcept>

// TODO (Person C): implement all methods below.
//
// Prerequisites:
//   1. Resolve the LD_PRELOAD shim question (PLANNING.md §9 item 3).
//      Read pocl source: lib/CL/devices/cpu/ to find the WG dispatch site.
//   2. Establish LLDB control: link against liblldb, create SBDebugger,
//      set POCL_EXTRA_BUILD_FLAGS="-g -cl-opt-disable" in the environment
//      before launching so the kernel carries DWARF.
//
// Stepping semantics note:
//   In pocl's `loops` strategy, step_over() steps the host thread one
//   source line — which may advance the loop induction variable (WI index)
//   by more than one iteration. Document the exact observed behavior.

namespace ocldbg {

struct PoclCPUBackend::Impl {
    // TODO: LLDB SBDebugger debugger;
    // TODO: LLDB SBTarget   target;
    // TODO: LLDB SBProcess  process;
};

PoclCPUBackend::PoclCPUBackend()
    : wg_tracker_(std::make_unique<WorkGroupTracker>()),
      wi_extractor_(std::make_unique<WIContextExtractor>()), impl_(std::make_unique<Impl>()) {}

PoclCPUBackend::~PoclCPUBackend() = default;

bool PoclCPUBackend::launch(const std::string & /*host_binary*/,
                            const std::vector<std::string> & /*args*/) {
    // TODO (Person C):
    //   1. Set env: POCL_EXTRA_BUILD_FLAGS="-g -cl-opt-disable",
    //               POCL_LEAVE_KERNEL_COMPILER_TEMP_FILES=1,
    //               LD_PRELOAD=path/to/libocldbg_rt.so
    //   2. SBDebugger::Create() -> impl_->debugger
    //   3. debugger.CreateTarget(host_binary) -> impl_->target
    //   4. target.Launch(...) -> impl_->process
    //   5. Set LLDB breakpoint listener
    return false;
}

void PoclCPUBackend::detach() {
    // TODO (Person C): process.Detach()
}

uint64_t PoclCPUBackend::set_breakpoint(const SourceLocation & /*loc*/) {
    // TODO (Person C):
    //   Use DWARFSourceModel::source_to_pcs() to get PCs, then:
    //   impl_->target.BreakpointCreateByAddress(pc) for each PC.
    return 0;
}

void PoclCPUBackend::remove_breakpoint(uint64_t /*bp_id*/) {
    // TODO (Person C): impl_->target.BreakpointDelete(bp_id)
}

void PoclCPUBackend::on_stop(StopCallback cb) {
    stop_cb_ = std::move(cb);
}

void PoclCPUBackend::resume() {
    // TODO (Person C): impl_->process.Continue()
}

void PoclCPUBackend::step_over(const OCLWorkItem & /*wi*/) {
    // TODO (Person C): SBThread::StepOver() on the WI's host thread.
    // Document observed stepping semantics with loops strategy.
}

void PoclCPUBackend::step_in(const OCLWorkItem & /*wi*/) {
    // TODO (Person C): SBThread::StepInto()
}

bool PoclCPUBackend::select_work_item(const Size3 & /*global_id*/, OCLWorkItem & /*out*/) {
    // TODO (Person C):
    //   1. wg_tracker_->find_wg_for_wi(global_id) -> host_thread_id
    //   2. wi_extractor_->extract(host_thread_id, global_id) -> OCLWorkItem
    return false;
}

LocationBackend &PoclCPUBackend::location_backend() {
    return loc_backend_;
}

size_t PoclCPUBackend::read_global_memory(HostAddress /*addr*/, void * /*buf*/, size_t /*length*/) {
    // TODO (Person C): impl_->process.ReadMemory(addr, buf, length, error)
    return 0;
}

VarValue CPULocationBackend::evaluate(const VarInfo & /*var*/, ExecCtxHandle /*exec_ctx*/) {
    // TODO (Person C): use LLDB SBFrame / SBValue to evaluate DWARF location expression
    return {};
}

} // namespace ocldbg
