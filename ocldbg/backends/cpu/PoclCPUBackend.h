#pragma once
#include "CPULocationBackend.h"
#include "ocldbg/Backend.h"
#include <memory>
#include <string>
#include <vector>

namespace ocldbg {

class WorkGroupTracker;
class WIContextExtractor;

/// Execution context for a stopped pocl CPU work-item.
/// The ExecCtxHandle in OCLWorkItem points to one of these.
struct CPUExecContext {
    uint64_t host_thread_id; ///< OS thread ID of the pocl worker thread
    // TODO (Person C): add LLDB SBThread / SBFrame wrapper here
};

/// Backend implementation for pocl's CPU device.
///
/// Owner: Person C
///
/// Targets pocl's `loops` work-group execution strategy at -O0 only.
/// Other strategies (loopvec, cbs) are explicitly out of scope for now.
///
/// Key open question (see PLANNING.md §9 item 3):
///   Can pocl's WG dispatch be hooked via LD_PRELOAD, or does this require
///   a minimal pocl source patch? Resolve before implementing WorkGroupTracker.
class PoclCPUBackend final : public Backend {
public:
    PoclCPUBackend();
    ~PoclCPUBackend() override;

    bool launch(const std::string &host_binary,
                const std::vector<std::string> &args) override;
    void detach() override;

    uint64_t set_breakpoint(const SourceLocation &loc) override;
    void     remove_breakpoint(uint64_t bp_id) override;

    void on_stop(StopCallback cb) override;
    void resume() override;
    void step_over(const OCLWorkItem &wi) override;
    void step_in(const OCLWorkItem &wi) override;

    bool select_work_item(const Size3 &global_id, OCLWorkItem &out) override;
    LocationBackend &location_backend() override;

    size_t read_global_memory(HostAddress addr, void *buf,
                              size_t length) override;

private:
    std::unique_ptr<WorkGroupTracker>    wg_tracker_;
    std::unique_ptr<WIContextExtractor> wi_extractor_;
    CPULocationBackend                   loc_backend_;
    StopCallback                         stop_cb_;

    struct Impl;
    std::unique_ptr<Impl> impl_; // holds LLDB SBDebugger, SBTarget, SBProcess
};

} // namespace ocldbg
