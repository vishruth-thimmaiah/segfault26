#include "LLDBAcceleratorBackend.h"

#include "AcceleratorExecContext.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <lldb/API/SBAddress.h>
#include <lldb/API/SBBreakpoint.h>
#include <lldb/API/SBDebugger.h>
#include <lldb/API/SBError.h>
#include <lldb/API/SBEvent.h>
#include <lldb/API/SBFileSpec.h>
#include <lldb/API/SBFrame.h>
#include <lldb/API/SBLaunchInfo.h>
#include <lldb/API/SBLineEntry.h>
#include <lldb/API/SBListener.h>
#include <lldb/API/SBProcess.h>
#include <lldb/API/SBSymbol.h>
#include <lldb/API/SBTarget.h>
#include <lldb/API/SBThread.h>
#include <lldb/API/SBValue.h>
#include <map>
#include <string_view>

namespace ocldbg {

struct AcceleratorState {
    struct RequestedBreakpoint {
        SourceLocation location;
        lldb::SBBreakpoint applied;
    };

    lldb::SBDebugger debugger;
    lldb::SBTarget host_target;
    lldb::SBProcess host_process;
    lldb::SBTarget accel_target;
    lldb::SBProcess accel_process;
    StopCallback on_stop;
    std::vector<std::string> plugin_stops;
    std::map<uint64_t, RequestedBreakpoint> breakpoints;
    uint64_t next_breakpoint_id = 1;
    std::optional<int> host_exit_status;
    bool stalled = false; ///< a resume ended without the accelerator stopping
};

namespace {

constexpr int kDefaultStopTimeoutSeconds = 120;

/// How long resume() waits for the accelerator to stop. A kernel may
/// legitimately run long, so this can be raised with
/// OCLDBG_ACCELERATOR_TIMEOUT_SECONDS.
std::chrono::seconds stop_timeout() {
    int seconds = 0;
    if (const char *value = std::getenv("OCLDBG_ACCELERATOR_TIMEOUT_SECONDS")) {
        const std::string_view text(value);
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), seconds);
        if (parsed.ec != std::errc()) {
            seconds = 0;
        }
    }
    return std::chrono::seconds(seconds > 0 ? seconds : kDefaultStopTimeoutSeconds);
}

/// A stop the accelerator plugin caused in the host, as opposed to one the host
/// program caused. A plugin either sets internal breakpoints, which have
/// negative ids, or halts the host with SIGSTOP.
bool stopped_by_plugin(lldb::SBThread thread) {
    if (thread.GetStopReasonDataCount() == 0) {
        return false;
    }
    const auto datum = thread.GetStopReasonDataAtIndex(0);
    switch (thread.GetStopReason()) {
    case lldb::eStopReasonBreakpoint:
        return static_cast<int64_t>(datum) < 0;
    case lldb::eStopReasonSignal:
        return datum == SIGSTOP;
    default:
        return false;
    }
}

std::string top_function(lldb::SBThread thread) {
    const char *name = thread.GetFrameAtIndex(0).GetFunctionName();
    return name != nullptr ? name : "";
}

/// The linker symbol, which unlike the function name does not depend on the
/// source language's display style.
std::string top_symbol(lldb::SBThread thread) {
    const char *name = thread.GetFrameAtIndex(0).GetSymbol().GetName();
    return name != nullptr ? name : top_function(thread);
}

OCLWorkItem make_work_item(lldb::SBThread thread, size_t index) {
    OCLWorkItem wi;
    wi.global_id = Size3{.x = index, .y = 0, .z = 0};
    wi.local_id = wi.global_id;
    wi.work_dim = 1;
    wi.function = top_function(thread);

    lldb::SBLineEntry line = thread.GetFrameAtIndex(0).GetLineEntry();
    if (line.IsValid()) {
        wi.location.file = line.GetFileSpec().GetFilename();
        wi.location.line = line.GetLine();
    }

    auto ctx = std::make_shared<AcceleratorExecContext>();
    ctx->thread = thread;
    wi.exec_ctx = ctx.get();
    wi.exec_ctx_storage = std::move(ctx);
    return wi;
}

std::vector<OCLWorkItem> work_items(AcceleratorState &state) {
    std::vector<OCLWorkItem> items;
    if (!state.accel_process.IsValid()) {
        return items;
    }
    const size_t count = state.accel_process.GetNumThreads();
    items.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        items.push_back(make_work_item(state.accel_process.GetThreadAtIndex(i), i));
    }
    return items;
}

void apply_breakpoints(AcceleratorState &state) {
    for (auto &[id, requested] : state.breakpoints) {
        if (!requested.applied.IsValid()) {
            requested.applied = state.accel_target.BreakpointCreateByLocation(
                requested.location.file.c_str(), requested.location.line);
        }
    }
}

/// The accelerator target is the SBTarget in the debugger that is not the host.
bool adopt_accelerator_target(AcceleratorState &state) {
    if (state.accel_target.IsValid()) {
        return true;
    }
    for (uint32_t i = 0; i < state.debugger.GetNumTargets(); ++i) {
        lldb::SBTarget candidate = state.debugger.GetTargetAtIndex(i);
        if (candidate != state.host_target && candidate.GetProcess().IsValid()) {
            state.accel_target = candidate;
            state.accel_process = candidate.GetProcess();
            return true;
        }
    }
    return false;
}

/// Index of the accelerator's selected thread in its thread list.
size_t selected_index(AcceleratorState &state) {
    const lldb::tid_t selected = state.accel_process.GetSelectedThread().GetThreadID();
    const size_t count = state.accel_process.GetNumThreads();
    for (size_t i = 0; i < count; ++i) {
        if (state.accel_process.GetThreadAtIndex(i).GetThreadID() == selected) {
            return i;
        }
    }
    return 0;
}

void notify_stop(AcceleratorState &state) {
    if (!state.on_stop) {
        return;
    }
    std::vector<OCLWorkItem> items = work_items(state);
    if (items.empty()) {
        return;
    }
    OCLStopContext stop;
    stop.stopped = items[std::min(selected_index(state), items.size() - 1)];
    stop.visible = std::move(items);
    state.on_stop(std::move(stop));
}

/// True once the host has gone, which is also when its exit status is noted.
bool host_gone(AcceleratorState &state) {
    switch (state.host_process.GetState()) {
    case lldb::eStateExited:
        state.host_exit_status = state.host_process.GetExitStatus();
        return true;
    case lldb::eStateDetached:
    case lldb::eStateInvalid:
        return true;
    default:
        return false;
    }
}

/// Records the plugin's stop of the host, if that is what the host is stopped at.
bool record_plugin_stop(AcceleratorState &state) {
    if (state.host_process.GetState() != lldb::eStateStopped) {
        return false;
    }
    lldb::SBThread thread = state.host_process.GetSelectedThread();
    if (!stopped_by_plugin(thread)) {
        return false;
    }
    state.plugin_stops.push_back(top_symbol(thread));
    return true;
}

/// Continues the host through the stops a plugin requests until the connect
/// action has created the accelerator target. The debugger must be synchronous.
bool advance_to_accelerator(AcceleratorState &state) {
    while (!host_gone(state)) {
        const bool by_plugin = record_plugin_stop(state);
        if (adopt_accelerator_target(state)) {
            return true;
        }
        if (!by_plugin) {
            std::cerr << "[ocldbg] Host stopped for a reason other than an accelerator plugin\n";
            return false;
        }
        state.host_process.Continue();
    }
    std::cerr << "[ocldbg] Host exited before an accelerator target was connected\n";
    return false;
}

/// Forwards what the debuggee wrote, which LLDB only hands over as events.
void forward_output(const lldb::SBProcess &process, uint32_t event_type) {
    std::array<char, 4096> buffer{};
    size_t count = 0;
    if ((event_type & lldb::SBProcess::eBroadcastBitSTDOUT) != 0) {
        while ((count = process.GetSTDOUT(buffer.data(), buffer.size())) > 0) {
            std::cout.write(buffer.data(), static_cast<std::streamsize>(count));
        }
    }
    if ((event_type & lldb::SBProcess::eBroadcastBitSTDERR) != 0) {
        while ((count = process.GetSTDERR(buffer.data(), buffer.size())) > 0) {
            std::cerr.write(buffer.data(), static_cast<std::streamsize>(count));
        }
    }
}

enum class WaitStep : uint8_t { KeepWaiting, AcceleratorStopped, Failed };

/// Reacts to one process event while waiting for the accelerator to stop.
WaitStep handle_process_event(AcceleratorState &state, const lldb::SBEvent &event) {
    lldb::SBProcess process = lldb::SBProcess::GetProcessFromEvent(event);
    const uint32_t type = event.GetType();
    if ((type & lldb::SBProcess::eBroadcastBitStateChanged) == 0) {
        forward_output(process, type);
        return WaitStep::KeepWaiting;
    }
    // A stop that LLDB is about to resume by itself is not a stop.
    if (lldb::SBProcess::GetRestartedFromEvent(event)) {
        return WaitStep::KeepWaiting;
    }

    const lldb::StateType event_state = lldb::SBProcess::GetStateFromEvent(event);
    if (process.GetUniqueID() == state.accel_process.GetUniqueID()) {
        return event_state == lldb::eStateStopped ? WaitStep::AcceleratorStopped
                                                  : WaitStep::KeepWaiting;
    }
    if (process.GetUniqueID() != state.host_process.GetUniqueID()) {
        return WaitStep::KeepWaiting;
    }
    if (host_gone(state)) {
        return WaitStep::Failed;
    }
    if (event_state != lldb::eStateStopped) {
        return WaitStep::KeepWaiting;
    }
    if (!record_plugin_stop(state)) {
        std::array<char, 256> description{};
        state.host_process.GetSelectedThread().GetStopDescription(description.data(),
                                                                  description.size());
        std::cerr << "[ocldbg] Host stopped while the accelerator was running: "
                  << description.data() << "\n";
        return WaitStep::Failed;
    }
    state.host_process.Continue();
    return WaitStep::KeepWaiting;
}

/// Waits, with the debugger asynchronous, until the accelerator stops or the
/// host is gone. Nothing else consumes the debugger's events, so this loop is
/// also what lets LLDB carry out its own resumes. Returns true for a stop.
bool wait_for_accelerator_stop(AcceleratorState &state) {
    lldb::SBListener listener = state.debugger.GetListener();
    const auto deadline = std::chrono::steady_clock::now() + stop_timeout();
    lldb::SBEvent event;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!listener.WaitForEvent(1, event)) {
            if (host_gone(state)) {
                return false;
            }
            continue;
        }
        if (!lldb::SBProcess::EventIsProcessEvent(event)) {
            continue;
        }
        switch (handle_process_event(state, event)) {
        case WaitStep::AcceleratorStopped:
            return true;
        case WaitStep::Failed:
            return false;
        case WaitStep::KeepWaiting:
            break;
        }
    }
    std::cerr << "[ocldbg] Timed out waiting for the accelerator to stop\n";
    return false;
}

/// Discards events queued while the debugger was synchronous, which would
/// otherwise look like stops that happen after the next resume.
void drain_events(AcceleratorState &state) {
    lldb::SBListener listener = state.debugger.GetListener();
    lldb::SBEvent event;
    while (listener.GetNextEvent(event)) {
    }
}

void shutdown(AcceleratorState &state) {
    state.debugger.SetAsync(false);
    if (state.host_process.IsValid()) {
        const lldb::StateType host_state = state.host_process.GetState();
        if (host_state == lldb::eStateStopped || host_state == lldb::eStateRunning) {
            state.host_process.Kill();
        }
    }
    if (state.debugger.IsValid()) {
        lldb::SBDebugger::Destroy(state.debugger);
    }
    state.accel_process = lldb::SBProcess();
    state.accel_target = lldb::SBTarget();
    state.host_process = lldb::SBProcess();
    state.host_target = lldb::SBTarget();
}

} // namespace

VarValue AcceleratorLocationBackend::evaluate(const VarInfo &var, ExecCtxHandle exec_ctx) {
    VarValue out;
    out.name = var.name;
    auto *ctx = static_cast<AcceleratorExecContext *>(exec_ctx);
    if (ctx == nullptr || !ctx->thread.IsValid()) {
        return out;
    }

    lldb::SBFrame frame = ctx->thread.GetSelectedFrame();
    lldb::SBValue value = frame.FindVariable(var.name.c_str());
    if (!value.IsValid()) {
        value = frame.FindRegister(var.name.c_str());
    }
    const char *text = value.IsValid() ? value.GetValue() : nullptr;
    if (text == nullptr) {
        return out;
    }

    const char *type = value.GetTypeName();
    out.type_name = type != nullptr ? type : "register";
    out.value_str = text;
    out.available = true;
    return out;
}

LLDBAcceleratorBackend::LLDBAcceleratorBackend() : state_(std::make_unique<AcceleratorState>()) {
    // LLDB writes to sockets whose other end (lldb-server) goes away when the host
    // is killed. Its own driver ignores SIGPIPE, and an embedder has to as well.
    std::signal(SIGPIPE, SIG_IGN);
    state_->debugger = lldb::SBDebugger::Create();
    state_->debugger.SetAsync(false);
}

LLDBAcceleratorBackend::~LLDBAcceleratorBackend() {
    shutdown(*state_);
}

bool LLDBAcceleratorBackend::launch(const std::string &host_binary,
                                    const std::vector<std::string> &args) {
    AcceleratorState &state = *state_;
    state.host_target = state.debugger.CreateTarget(host_binary.c_str());
    if (!state.host_target.IsValid()) {
        std::cerr << "[ocldbg] Failed to create target for: " << host_binary << "\n";
        return false;
    }

    std::vector<const char *> c_args;
    c_args.reserve(args.size() + 1);
    for (const auto &arg : args) {
        c_args.push_back(arg.c_str());
    }
    c_args.push_back(nullptr);

    lldb::SBLaunchInfo launch_info = state.host_target.GetLaunchInfo();
    launch_info.SetArguments(c_args.data(), true);
    lldb::SBError error;
    state.host_process = state.host_target.Launch(launch_info, error);
    if (!state.host_process.IsValid() || !error.Success()) {
        std::cerr << "[ocldbg] Failed to launch host binary: " << error.GetCString() << "\n";
        return false;
    }

    if (!advance_to_accelerator(state)) {
        return false;
    }
    apply_breakpoints(state);
    return true;
}

void LLDBAcceleratorBackend::detach() {
    shutdown(*state_);
}

uint64_t LLDBAcceleratorBackend::set_breakpoint(const SourceLocation &loc) {
    AcceleratorState &state = *state_;
    const uint64_t id = state.next_breakpoint_id++;
    state.breakpoints[id] = AcceleratorState::RequestedBreakpoint{.location = loc, .applied = {}};
    if (state.accel_target.IsValid()) {
        apply_breakpoints(state);
    }
    return id;
}

void LLDBAcceleratorBackend::remove_breakpoint(uint64_t bp_id) {
    AcceleratorState &state = *state_;
    auto it = state.breakpoints.find(bp_id);
    if (it == state.breakpoints.end()) {
        return;
    }
    if (it->second.applied.IsValid()) {
        state.accel_target.BreakpointDelete(it->second.applied.GetID());
    }
    state.breakpoints.erase(it);
}

void LLDBAcceleratorBackend::on_stop(StopCallback cb) {
    state_->on_stop = std::move(cb);
}

void LLDBAcceleratorBackend::resume() {
    AcceleratorState &state = *state_;
    if (!state.accel_process.IsValid() || host_gone(state)) {
        return;
    }

    // The accelerator can only stop while the host runs, so both are resumed and
    // the debugger must not block on either.
    drain_events(state);
    state.debugger.SetAsync(true);
    if (state.accel_process.GetState() == lldb::eStateStopped) {
        state.accel_process.Continue();
    }
    if (state.host_process.GetState() == lldb::eStateStopped) {
        state.host_process.Continue();
    }
    const bool stopped = wait_for_accelerator_stop(state);
    state.debugger.SetAsync(false);
    state.stalled = !stopped && !host_gone(state);
    if (stopped) {
        notify_stop(state);
    }
}

void LLDBAcceleratorBackend::step_over(const OCLWorkItem & /*wi*/) {
    std::cerr << "[ocldbg] Stepping is not supported by the accelerator backend\n";
}

void LLDBAcceleratorBackend::step_in(const OCLWorkItem & /*wi*/) {
    std::cerr << "[ocldbg] Stepping is not supported by the accelerator backend\n";
}

bool LLDBAcceleratorBackend::select_work_item(const Size3 &global_id, OCLWorkItem &out_wi) {
    AcceleratorState &state = *state_;
    if (!state.accel_process.IsValid() || global_id.y != 0 || global_id.z != 0 ||
        global_id.x >= state.accel_process.GetNumThreads()) {
        return false;
    }
    lldb::SBThread thread = state.accel_process.GetThreadAtIndex(global_id.x);
    state.accel_process.SetSelectedThread(thread);
    out_wi = make_work_item(thread, global_id.x);
    return true;
}

size_t LLDBAcceleratorBackend::read_global_memory(HostAddress addr, void *buf, size_t length) {
    AcceleratorState &state = *state_;
    if (!state.accel_process.IsValid()) {
        return 0;
    }
    lldb::SBError error;
#if defined(OCLDBG_HAVE_SB_PROCESS_ADDRESS)
    // A plain read goes to the host-style default address space. Use the plugin's
    // "global" space when it names one, and the default space otherwise.
    const lldb::addr_space_t space = state.accel_process.GetAddressSpaceID("global", error);
    if (error.Success() && space != LLDB_INVALID_ADDRESS_SPACE_ID) {
        return state.accel_process.ReadMemory(lldb::SBProcessAddress(addr, space), buf, length,
                                              error);
    }
    error.Clear();
    return state.accel_process.ReadMemory(addr, buf, length, error);
#elif defined(OCLDBG_HAVE_SB_ADDRESS_SPEC)
    // A plain read goes to the host process, which cannot see device memory. The
    // address space is taken relative to a thread; the client crashes without one.
    lldb::SBThread thread = state.accel_process.GetSelectedThread();
    if (!thread.IsValid()) {
        return 0;
    }
    return state.accel_process.ReadMemoryFromSpec(lldb::SBAddressSpec(addr, "generic", thread), buf,
                                                  length, error);
#else
    return state.accel_process.ReadMemory(addr, buf, length, error);
#endif
}

bool LLDBAcceleratorBackend::accelerator_connected() const {
    return state_->accel_target.IsValid();
}

bool LLDBAcceleratorBackend::halted() const {
    AcceleratorState &state = *state_;
    return state.accel_process.IsValid() && !state.stalled && !host_gone(state) &&
           state.accel_process.GetState() == lldb::eStateStopped;
}

bool LLDBAcceleratorBackend::stalled() const {
    return state_->stalled;
}

std::string LLDBAcceleratorBackend::accelerator_triple() const {
    const char *triple = state_->accel_target.GetTriple();
    return triple != nullptr ? triple : "";
}

std::vector<OCLWorkItem> LLDBAcceleratorBackend::accelerator_work_items() const {
    return work_items(*state_);
}

const std::vector<std::string> &LLDBAcceleratorBackend::plugin_stops() const {
    return state_->plugin_stops;
}

std::optional<int> LLDBAcceleratorBackend::host_exit_status() const {
    return state_->host_exit_status;
}

} // namespace ocldbg
