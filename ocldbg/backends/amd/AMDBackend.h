#pragma once
#include "AMDDbgApiSession.h"
#include "AMDLocationBackend.h"
#include "ocldbg/Backend.h"

#include <optional>

namespace ocldbg {

/// Execution context for a halted AMD GPU wavefront. The ExecCtxHandle in
/// OCLWorkItem points to one of these.
struct AMDExecContext {
    amd_dbgapi_wave_id_t wave{};
    AMDDbgApiSession *session = nullptr;
};

/// Backend for AMD GPUs via amd-dbgapi (librocm-dbgapi), attaching to the
/// host process running the OpenCL/HIP kernel and driving the GPU debug
/// state directly -- no ptrace-visible host thread per work-item the way
/// PoclCPUBackend has, and no cooperating in-process plugin the way
/// OclgrindBackend has. This is the closest analog to a real hardware GPU
/// backend PLANNING.md describes (§5, §8e).
///
/// Segfault26 issue #12. Built from a standalone proof-of-concept verified
/// end-to-end against a real AMD Radeon RX 9060 XT (gfx1200) -- see
/// AMDDbgApiSession.h for exactly what that proved.
///
/// Implementation status (be precise about this -- do not assume more works
/// than is documented here):
///   launch/detach/resume/on_stop/read_global_memory   -- real, backed by
///     the verified AMDDbgApiSession. resume() halts *whatever wave is
///     currently running* (via amd_dbgapi_wave_stop), not a specific
///     source line -- there is no targeted breakpoint yet (see below).
///   set_breakpoint/remove_breakpoint                  -- NOT implemented.
///     amd-dbgapi's insert_breakpoint/remove_breakpoint callbacks are for
///     the *client's own host-side* breakpoints (how the library tracks
///     HSA runtime / code-object load internally); they are not a "set a
///     breakpoint at this GPU instruction" API. A real source-line
///     breakpoint on the GPU appears to require patching a trap opcode
///     directly into the code object's instruction stream at the target PC
///     (mirroring the host int3 dance, but for AMDGPU ISA, over the same
///     xfer_global_memory-style path) -- unverified, needs investigation
///     before this can do more than return failure.
///   step_over/step_in                                 -- NOT implemented;
///     blocked on the same open question as breakpoints.
///   select_work_item                                  -- partial: selects
///     the currently halted wave regardless of the requested global_id (no
///     verified lane-precise global/local-id mapping yet; see
///     AMD_DBGAPI_WAVE_INFO_WORKGROUP_COORD / WAVE_NUMBER_IN_WORKGROUP as
///     the likely next step). Returns false if no wave is halted.
class AMDBackend final : public Backend {
public:
    AMDBackend();
    ~AMDBackend() override;

    AMDBackend(const AMDBackend &) = delete;
    AMDBackend &operator=(const AMDBackend &) = delete;

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

    /// Raw AMD_DBGAPI_WAVE_STOP_REASON_* bitmask from the most recent
    /// resume()'s halt -- 0 if resume() proactively halted a running wave
    /// itself, nonzero if the *hardware* stopped it (e.g. a memory
    /// violation from an out-of-bounds access). Mirrors
    /// OclgrindBackend::last_stop_line() in spirit: backend-specific detail
    /// the generic Backend interface has no slot for.
    [[nodiscard]] uint32_t last_stop_reason() const { return last_stop_reason_; }

private:
    AMDDbgApiSession session_;
    AMDLocationBackend loc_backend_;
    StopCallback stop_cb_;
    std::optional<AMDExecContext> current_ctx_;
    uint32_t last_stop_reason_ = 0;
};

} // namespace ocldbg
