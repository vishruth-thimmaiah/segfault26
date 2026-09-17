#pragma once
#include "OclgrindLocationBackend.h"
#include "OclgrindProtocol.h"
#include "ocldbg/Backend.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ocldbg {

/// Oclgrind's WorkItem object lives in the host program's address space, so a
/// work-item is addressed by global ID over the channel rather than by pointer.
struct OclgrindExecContext {
    Size3 global_id;
    oclgrind_proto::LineChannel *channel = nullptr;
};

/// Backend for the Oclgrind OpenCL simulator.
///
/// Oclgrind interprets kernels inside the host program, so there is nothing to
/// attach to. The backend launches the host program with Oclgrind's runtime
/// preloaded and plugin/DebugPlugin.h injected, then drives it over a socket.
///
/// Work-items run one at a time in a deterministic order because the plugin
/// declares itself thread-unsafe. Breakpoints match on source line only, and
/// only work-items of running or pending work-groups can be selected.
class OclgrindBackend final : public Backend {
public:
    OclgrindBackend();
    ~OclgrindBackend() override;

    OclgrindBackend(const OclgrindBackend &) = delete;
    OclgrindBackend &operator=(const OclgrindBackend &) = delete;

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

    /// True while the host program is halted and able to answer queries.
    [[nodiscard]] bool halted() const;

private:
    void resume_with(std::string_view command);

    OclgrindLocationBackend loc_backend_;
    StopCallback stop_cb_;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ocldbg
