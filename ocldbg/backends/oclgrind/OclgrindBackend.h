#pragma once
#include "OclgrindLocationBackend.h"
#include "ocldbg/Backend.h"

#include <memory>
#include <string>
#include <vector>

// Forward-declare Oclgrind types to avoid pulling in all Oclgrind headers
// in our public interface. Include oclgrind/src/core/Plugin.h in the .cpp.
namespace oclgrind {
class WorkItem;
}

namespace ocldbg {

/// Execution context for a stopped Oclgrind work-item.
/// The ExecCtxHandle in OCLWorkItem points to one of these.
struct OclgrindExecContext {
    const oclgrind::WorkItem *wi = nullptr; ///< Oclgrind interpreter work-item
    // TODO (Person D): add any additional state needed to read variables
};

/// Backend implementation for Oclgrind.
///
/// Owner: Person D
///
/// Strategy: adapt Oclgrind's existing interactive debugger rather than
/// building from raw plugin callbacks. Read src/plugins/InteractiveDebugger.cpp
/// to understand how it halts execution and reads state, then expose the
/// same state through the Backend/LocationBackend interfaces.
///
/// Integration options (start with in-process):
///   In-process:  OclgrindBackend runs as an Oclgrind Plugin in the same
///                process as the OpenCL host program.
///   Out-of-process: Plugin communicates over a Unix socket to a separate
///                   ocldbg DAP server process.
///
/// Open questions (PLANNING.md §9 items 5-8):
///   - Does instructionExecuted fire at IR instruction granularity?
///   - Can execution be suspended mid-work-group?
///   - Is sequential WI scheduling order stable?
class OclgrindBackend final : public Backend {
public:
    OclgrindBackend();
    ~OclgrindBackend() override;

    bool launch(const std::string &host_binary, const std::vector<std::string> &args) override;
    void detach() override;

    uint64_t set_breakpoint(const SourceLocation &loc) override;
    void remove_breakpoint(uint64_t bp_id) override;

    void on_stop(StopCallback cb) override;
    void resume() override;
    void step_over(const OCLWorkItem &wi) override;
    void step_in(const OCLWorkItem &wi) override;

    bool select_work_item(const Size3 &global_id, OCLWorkItem &out) override;
    LocationBackend &location_backend() override;

    size_t read_global_memory(HostAddress addr, void *buf, size_t length) override;

private:
    OclgrindLocationBackend loc_backend_;
    StopCallback stop_cb_;

    struct Impl; // holds Oclgrind Runtime / Plugin references
    std::unique_ptr<Impl> impl_;
};

} // namespace ocldbg
