#include "OclgrindBackend.h"
#include <stdexcept>

// TODO (Person D): implement using Oclgrind's Plugin API.
//
// First steps:
//   1. Read oclgrind/src/plugins/InteractiveDebugger.cpp — this is your
//      primary reference for how Oclgrind halts execution and reads state.
//   2. Read oclgrind/src/core/Plugin.h — confirmed hook signatures:
//        instructionExecuted(const WorkItem*, const llvm::Instruction*, TypedValue)
//        workItemBegin(const WorkItem*)
//        workItemComplete(const WorkItem*)
//   3. Read oclgrind/src/core/WorkItem.h to understand available state.
//   4. Decide on in-process vs out-of-process integration (start in-process).
//
// Launch approach for in-process:
//   Set OCLGRIND_DEVICE=1 (or configure ICD) so the host program uses
//   Oclgrind's OpenCL device. Register our Plugin with Oclgrind's runtime.
//   The plugin callbacks fire during kernel execution.

namespace ocldbg {

struct OclgrindBackend::Impl {
    // TODO (Person D): hold Oclgrind Runtime* and plugin registration handle
    bool halted = false;
    OclgrindExecContext stopped_ctx;
};

OclgrindBackend::OclgrindBackend() : impl_(std::make_unique<Impl>()) {}
OclgrindBackend::~OclgrindBackend() = default;

bool OclgrindBackend::launch(const std::string & /*host_binary*/,
                              const std::vector<std::string> & /*args*/) {
    // TODO (Person D):
    //   Set OCLGRIND_DEVICE=1 in the environment.
    //   Create/register our Plugin with Oclgrind Runtime.
    //   Fork and exec host_binary, or run inline.
    return false;
}

void OclgrindBackend::detach() {
    // TODO (Person D)
}

uint64_t OclgrindBackend::set_breakpoint(const SourceLocation & /*loc*/) {
    // TODO (Person D):
    //   In the plugin's instructionExecuted callback, compare the instruction's
    //   !dbg metadata (file/line) against registered breakpoint locations.
    //   Store SourceLocation -> bp_id mapping here.
    return 0;
}

void OclgrindBackend::remove_breakpoint(uint64_t /*bp_id*/) {
    // TODO (Person D)
}

void OclgrindBackend::on_stop(StopCallback cb) { stop_cb_ = std::move(cb); }

void OclgrindBackend::resume() {
    // TODO (Person D): unset halt flag; let the Oclgrind interpreter continue
}

void OclgrindBackend::step_over(const OCLWorkItem & /*wi*/) {
    // TODO (Person D): run until next !dbg location change
    // Note: Oclgrind's sequential scheduling makes this deterministic —
    // document the observed semantics.
}

void OclgrindBackend::step_in(const OCLWorkItem & /*wi*/) {
    // TODO (Person D)
}

bool OclgrindBackend::select_work_item(const Size3 & /*global_id*/,
                                        OCLWorkItem & /*out*/) {
    // TODO (Person D): check if the requested WI is in visible set;
    // Oclgrind's sequential scheduling means only the current WI (and
    // completed WIs in the same WG) may be accessible.
    return false;
}

LocationBackend &OclgrindBackend::location_backend() { return loc_backend_; }

size_t OclgrindBackend::read_global_memory(HostAddress /*addr*/, void * /*buf*/,
                                            size_t /*length*/) {
    // TODO (Person D): use Oclgrind Memory API
    return 0;
}

VarValue OclgrindLocationBackend::evaluate(const VarInfo & /*var*/,
                                           ExecCtxHandle /*exec_ctx*/) {
    // TODO (Person D): implement after inspecting Oclgrind internals
    return {};
}

} // namespace ocldbg
