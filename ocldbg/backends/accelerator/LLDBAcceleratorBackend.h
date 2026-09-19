#pragma once
#include "AcceleratorLocationBackend.h"
#include "ocldbg/Backend.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ocldbg {

struct AcceleratorState;

/// Debugs a hardware accelerator through LLVM's upstream LLDB accelerator
/// plugin framework instead of a hand-written driver.
///
/// An lldb-server plugin asks the LLDB client, over gdb-remote packets
/// (jAcceleratorPlugin* upstream, jGPUPlugin* in the older protocol of the AMD
/// plugin branch), to set internal breakpoints in the host process and finally
/// to create a second target connected to the accelerator's own gdb-server. The accelerator then
/// appears as a second SBTarget/SBProcess in the same SBDebugger; its threads become work-items
/// here.
///
/// Needs an lldb-server built with an accelerator plugin, selected with
/// LLDB_DEBUGSERVER_PATH. Verified with upstream LLDB 23's mock plugin and with
/// the AMD GPU plugin from the llvm-server-plugins branch on a gfx1200 GPU.
///
/// resume() continues the accelerator and the host, and returns when the
/// accelerator stops again (reported through on_stop) or the host is gone.
/// Stepping is not supported.
class LLDBAcceleratorBackend final : public Backend {
public:
    LLDBAcceleratorBackend();
    ~LLDBAcceleratorBackend() override;

    LLDBAcceleratorBackend(const LLDBAcceleratorBackend &) = delete;
    LLDBAcceleratorBackend &operator=(const LLDBAcceleratorBackend &) = delete;

    /// Launches the host and runs it past every stop the plugin asks for, up to
    /// the point where the accelerator target is connected. Returns false if
    /// the host exits or stops for another reason first.
    bool launch(const std::string &host_binary, const std::vector<std::string> &args) override;
    void detach() override;

    /// Breakpoints set before the accelerator connects are applied on connect.
    uint64_t set_breakpoint(const SourceLocation &loc) override;
    void remove_breakpoint(uint64_t bp_id) override;

    void on_stop(StopCallback cb) override;
    void resume() override;
    void step_over(const OCLWorkItem &wi) override;
    void step_in(const OCLWorkItem &wi) override;

    /// Selects accelerator thread `global_id.x`; y and z must be 0.
    bool select_work_item(const Size3 &global_id, OCLWorkItem &out_wi) override;
    LocationBackend &location_backend() override { return location_backend_; }

    size_t read_global_memory(HostAddress addr, void *buf, size_t length) override;

    bool accelerator_connected() const;

    /// True while the accelerator is stopped and the host is still alive.
    bool halted() const;

    /// True if the last resume ended with the accelerator neither stopped nor
    /// the host gone (it timed out, or the host stopped for another reason).
    bool stalled() const;

    /// Target triple of the accelerator, e.g. amdgcn-amd-amdhsa--gfx1200.
    std::string accelerator_triple() const;

    /// One work-item per accelerator thread. The mapping to NDRange
    /// coordinates is a placeholder (thread index becomes global_id.x): the
    /// upstream framework does not define how a plugin reports coordinates.
    std::vector<OCLWorkItem> accelerator_work_items() const;

    /// Host functions the plugin stopped the host in, in order.
    const std::vector<std::string> &plugin_stops() const;

    /// Exit status of the host, once it has exited.
    std::optional<int> host_exit_status() const;

private:
    AcceleratorLocationBackend location_backend_;
    std::unique_ptr<AcceleratorState> state_;
};

} // namespace ocldbg
