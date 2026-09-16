#pragma once
#include "ocldbg/Types.h"

#include <amd-dbgapi/amd-dbgapi.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <sys/types.h>
#include <vector>

namespace ocldbg {

/// A GPU wavefront discovered and halted via amd-dbgapi.
struct AMDWaveInfo {
    amd_dbgapi_wave_id_t wave{};
    amd_dbgapi_global_address_t pc = 0;
    /// Raw AMD_DBGAPI_WAVE_STOP_REASON_* bitmask (0 = ocldbg's own
    /// wave_stop() halted it; nonzero means the hardware itself stopped the
    /// wave -- e.g. AMD_DBGAPI_WAVE_STOP_REASON_MEMORY_VIOLATION for an
    /// out-of-bounds access, the GPU analog of reduction_bug.cl's CPU bug.
    uint32_t stop_reason = 0;
};

/// Wraps the amd-dbgapi callback contract and the ptrace process-control
/// loop that backs it. amd-dbgapi does no process control of its own -- it
/// delegates all of that to the client via amd_dbgapi_callbacks_s, and this
/// class is that client.
///
/// Verified end-to-end against a real AMD Radeon RX 9060 XT (gfx1200) in a
/// standalone proof-of-concept (segfault26 issue #12) before being adapted
/// into this class:
///   - process attach against a real dispatched HIP kernel
///   - the full callback contract: real int3 breakpoint injection via
///     /proc/<pid>/mem (used by the library itself to track HSA runtime /
///     code-object load events -- see class comment on set_breakpoint()
///     below for why this is NOT the same thing as a kernel-source
///     breakpoint), memory read/write, OS-PID query, logging
///   - servicing the runtime-notification event protocol
///     (AMD_DBGAPI_EVENT_KIND_RUNTIME, CODE_OBJECT_LIST_UPDATED,
///     BREAKPOINT_RESUME)
///   - agent/queue/wave enumeration reflecting real live GPU state
///   - amd_dbgapi_wave_stop() on a running wave -> real WAVE_STOP event
///   - amd_dbgapi_wave_get_info(..., PC, ...) returning a real GPU program
///     counter off live hardware
///   - amd_dbgapi_read_register on VGPRs (correct sizes; clean
///     "not available" status for unallocated registers)
///
/// NOT yet implemented/verified: targeted GPU-instruction (source-line)
/// breakpoints, and DWARF->register resolution via
/// amd_dbgapi_dwarf_register_to_register (present in the API, not exercised
/// on real hardware yet). See AMDBackend::set_breakpoint().
class AMDDbgApiSession {
public:
    AMDDbgApiSession();
    ~AMDDbgApiSession();

    AMDDbgApiSession(const AMDDbgApiSession &) = delete;
    AMDDbgApiSession &operator=(const AMDDbgApiSession &) = delete;

    /// Fork, PTRACE_TRACEME + execv the host binary, then amd_dbgapi_initialize
    /// + amd_dbgapi_process_attach.
    bool launch(const std::string &host_binary, const std::vector<std::string> &args);

    void detach();

    /// Resume everything and block, servicing the ptrace wait-loop and
    /// draining amd-dbgapi events, until either a wave is discovered in the
    /// STOP state (filled into `out`, having actively requested
    /// amd_dbgapi_wave_stop() on the first RUN wave seen) or the debuggee
    /// process exits. Returns false on exit; `out` is unmodified then.
    bool resume_until_wave_stop(AMDWaveInfo &out);

    /// Read from the debuggee's unified (host + GPU) address space via
    /// /proc/<pid>/mem -- the same mechanism amd-dbgapi's own
    /// xfer_global_memory callback uses, proven working for both host and
    /// GPU-resident (e.g. __global buffer) addresses since HSA maps GPU
    /// memory into the process's address space.
    size_t read_memory(HostAddress addr, void *buf, size_t length) const;

    bool read_register(amd_dbgapi_wave_id_t wave, amd_dbgapi_register_id_t reg, size_t offset,
                       size_t size, void *out) const;

    /// Resolve a DWARF register number (as found in a DW_OP_reg*/DW_OP_breg*
    /// location expression) to this wave's dbgapi register id, via the
    /// wave's architecture and amd_dbgapi_dwarf_register_to_register.
    /// UNVERIFIED on real hardware -- the proof of concept only read
    /// architecture-enumerated registers directly, not via DWARF number.
    std::optional<amd_dbgapi_register_id_t> dwarf_register(amd_dbgapi_wave_id_t wave,
                                                           uint64_t dwarf_reg_num) const;

    [[nodiscard]] bool attached() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    // Helpers split out of resume_until_wave_stop() to keep its cognitive
    // complexity down; see their doc comments in the .cpp.
    void drain_pending_events();
    void refresh_known_waves();
    bool scan_waves_for_stop(AMDWaveInfo &out);

    // Impl is private, so this (used by the free-standing callbacks too)
    // must be a member to name it despite being defined outside any class.
    static int mem_fd_for(Impl &impl);

    // amd_dbgapi_callbacks_s fields are plain C function pointers, so these
    // must be static (no implicit `this`); they recover session state via
    // the client_process_id opaque handle, which we set to `impl_.get()`.
    static amd_dbgapi_status_t
    cb_client_process_get_info(amd_dbgapi_client_process_id_t client_process_id,
                               amd_dbgapi_client_process_info_t query, size_t value_size,
                               void *value);
    static amd_dbgapi_status_t
    cb_insert_breakpoint(amd_dbgapi_client_process_id_t client_process_id,
                         amd_dbgapi_global_address_t address,
                         amd_dbgapi_breakpoint_id_t breakpoint_id);
    static amd_dbgapi_status_t
    cb_remove_breakpoint(amd_dbgapi_client_process_id_t client_process_id,
                         amd_dbgapi_breakpoint_id_t breakpoint_id);
    static amd_dbgapi_status_t
    cb_xfer_global_memory(amd_dbgapi_client_process_id_t client_process_id,
                          amd_dbgapi_global_address_t global_address, amd_dbgapi_size_t *value_size,
                          void *read_buffer, const void *write_buffer);
    static bool handle_stop(Impl &impl, pid_t tid, int status);
};

} // namespace ocldbg
