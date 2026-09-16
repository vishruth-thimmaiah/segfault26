#include "AMDDbgApiSession.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <poll.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

// Process-control layer for amd-dbgapi. amd-dbgapi is a passive library: it
// tells the client (via amd_dbgapi_callbacks_s) what to do -- insert this
// breakpoint, read this memory -- but performs no ptrace itself. This file
// is the ptrace-based client, adapted from a standalone proof-of-concept
// that validated this exact approach against a real AMD Radeon RX 9060 XT
// (gfx1200); see AMDDbgApiSession.h for what was verified.

namespace ocldbg {

namespace {

struct BpInfo {
    amd_dbgapi_global_address_t addr = 0;
    unsigned char orig_byte = 0;
};

void log_status(const char *what, amd_dbgapi_status_t st) {
    const char *msg = nullptr;
    amd_dbgapi_get_status_string(st, &msg);
    std::fprintf(stderr, "[ocldbg/amd] %s: %s (%d)\n", what, msg != nullptr ? msg : "?",
                 static_cast<int>(st));
}

void cb_log_message(amd_dbgapi_log_level_t /*level*/, const char *message) {
    std::fprintf(stderr, "[ocldbg/amd/dbgapi] %s\n", message);
}

void *cb_allocate_memory(size_t byte_size) {
    return byte_size == 0 ? nullptr : std::malloc(byte_size);
}

void cb_deallocate_memory(void *data) {
    std::free(data);
}

} // namespace

// A plain data aggregate (no member functions -- see mem_fd_for() below for
// why): misc-non-private-member-variables-in-classes only objects to public
// data mixed with behavior, not to a struct used purely as a data bag.
struct AMDDbgApiSession::Impl {
    pid_t pid = -1;
    int mem_fd = -1;
    amd_dbgapi_process_id_t process_id{};
    amd_dbgapi_notifier_t notifier = -1;

    std::map<uint64_t, BpInfo> breakpoints; // breakpoint_id.handle -> info
    std::map<pid_t, bool> known_tids;

    bool runtime_loaded = false;
    bool wave_stop_requested = false;

    // amd_dbgapi_process_*_list report count=0/array=NULL when the set is
    // unchanged since the caller's last query (see amd_dbgapi_changed_t) --
    // NOT "nothing exists". Retain the last known-good wave set and only
    // replace it when the library reports AMD_DBGAPI_CHANGED_YES; state
    // polling on cached handles happens independently every loop iteration.
    std::vector<amd_dbgapi_wave_id_t> known_waves;
};

int AMDDbgApiSession::mem_fd_for(Impl &impl) {
    if (impl.mem_fd < 0 && impl.pid > 0) {
        std::array<char, 64> path{};
        std::snprintf(path.data(), path.size(), "/proc/%d/mem", impl.pid);
        impl.mem_fd = ::open(path.data(), O_RDWR);
    }
    return impl.mem_fd;
}

// ---- amd_dbgapi_callbacks_s implementation. client_process_id is always
// the Impl* we pass to amd_dbgapi_process_attach(). These are private
// static members (see AMDDbgApiSession.h) so they can reach Impl's fields. ----

amd_dbgapi_status_t
AMDDbgApiSession::cb_client_process_get_info(amd_dbgapi_client_process_id_t client_process_id,
                                             amd_dbgapi_client_process_info_t query,
                                             size_t value_size, void *value) {
    auto *impl = reinterpret_cast<AMDDbgApiSession::Impl *>(client_process_id);
    if (query == AMD_DBGAPI_CLIENT_PROCESS_INFO_OS_PID) {
        if (value_size != sizeof(amd_dbgapi_os_process_id_t)) {
            return AMD_DBGAPI_STATUS_ERROR_INVALID_ARGUMENT_COMPATIBILITY;
        }
        std::memcpy(value, &impl->pid, sizeof(impl->pid));
        return AMD_DBGAPI_STATUS_SUCCESS;
    }
    if (query == AMD_DBGAPI_CLIENT_PROCESS_INFO_CORE_STATE) {
        return AMD_DBGAPI_STATUS_ERROR_NOT_AVAILABLE;
    }
    return AMD_DBGAPI_STATUS_ERROR_INVALID_ARGUMENT;
}

amd_dbgapi_status_t
AMDDbgApiSession::cb_insert_breakpoint(amd_dbgapi_client_process_id_t client_process_id,
                                       amd_dbgapi_global_address_t address,
                                       amd_dbgapi_breakpoint_id_t breakpoint_id) {
    auto *impl = reinterpret_cast<AMDDbgApiSession::Impl *>(client_process_id);
    int fd = mem_fd_for(*impl);
    if (fd < 0) {
        return AMD_DBGAPI_STATUS_ERROR;
    }
    unsigned char orig = 0;
    if (::pread(fd, &orig, 1, static_cast<off_t>(address)) != 1) {
        return AMD_DBGAPI_STATUS_ERROR;
    }
    unsigned char int3 = 0xCC;
    if (::pwrite(fd, &int3, 1, static_cast<off_t>(address)) != 1) {
        return AMD_DBGAPI_STATUS_ERROR;
    }
    impl->breakpoints[breakpoint_id.handle] = BpInfo{address, orig};
    return AMD_DBGAPI_STATUS_SUCCESS;
}

amd_dbgapi_status_t
AMDDbgApiSession::cb_remove_breakpoint(amd_dbgapi_client_process_id_t client_process_id,
                                       amd_dbgapi_breakpoint_id_t breakpoint_id) {
    auto *impl = reinterpret_cast<AMDDbgApiSession::Impl *>(client_process_id);
    auto it = impl->breakpoints.find(breakpoint_id.handle);
    if (it == impl->breakpoints.end()) {
        return AMD_DBGAPI_STATUS_ERROR_INVALID_BREAKPOINT_ID;
    }
    int fd = mem_fd_for(*impl);
    if (fd >= 0) {
        ::pwrite(fd, &it->second.orig_byte, 1, static_cast<off_t>(it->second.addr));
    }
    impl->breakpoints.erase(it);
    return AMD_DBGAPI_STATUS_SUCCESS;
}

amd_dbgapi_status_t AMDDbgApiSession::cb_xfer_global_memory(
    amd_dbgapi_client_process_id_t client_process_id, amd_dbgapi_global_address_t global_address,
    amd_dbgapi_size_t *value_size, void *read_buffer, const void *write_buffer) {
    auto *impl = reinterpret_cast<AMDDbgApiSession::Impl *>(client_process_id);
    int fd = mem_fd_for(*impl);
    if (fd < 0) {
        *value_size = 0;
        return AMD_DBGAPI_STATUS_ERROR_MEMORY_ACCESS;
    }
    ssize_t n = 0;
    if (read_buffer != nullptr) {
        n = ::pread(fd, read_buffer, *value_size, static_cast<off_t>(global_address));
    } else if (write_buffer != nullptr) {
        n = ::pwrite(fd, write_buffer, *value_size, static_cast<off_t>(global_address));
    }
    if (n <= 0) {
        *value_size = 0;
        return AMD_DBGAPI_STATUS_ERROR_MEMORY_ACCESS;
    }
    *value_size = static_cast<amd_dbgapi_size_t>(n);
    return AMD_DBGAPI_STATUS_SUCCESS;
}

namespace {
void resume_tid(pid_t tid, int sig = 0) {
    // ptrace()'s 4th argument is conventionally passed this way for the
    // signal-to-deliver; there is no pointer here to "optimize".
    ptrace(PTRACE_CONT, tid, nullptr,
           reinterpret_cast<void *>(static_cast<long>(sig))); // NOLINT(performance-no-int-to-ptr)
}

// amd_dbgapi_client_thread_id_t is an opaque, client-defined token, not a
// real pointer; encoding a pid_t into it is the intended usage.
amd_dbgapi_client_thread_id_t client_thread_for(pid_t tid) {
    // NOLINTBEGIN(performance-no-int-to-ptr)
    return reinterpret_cast<amd_dbgapi_client_thread_id_t>(static_cast<intptr_t>(tid));
    // NOLINTEND(performance-no-int-to-ptr)
}
} // namespace

/// Handles one waitpid()-reported stop. Returns false only when the traced
/// main process itself has exited.
bool AMDDbgApiSession::handle_stop(AMDDbgApiSession::Impl &impl, pid_t tid, int status) {
    if (WIFEXITED(status) || WIFSIGNALED(status)) {
        impl.known_tids.erase(tid);
        return tid != impl.pid;
    }
    if (!WIFSTOPPED(status)) {
        return true;
    }

    int sig = WSTOPSIG(status);
    unsigned long event = static_cast<unsigned long>(status) >> 16;

    if (event == PTRACE_EVENT_CLONE) {
        unsigned long new_tid = 0;
        ptrace(PTRACE_GETEVENTMSG, tid, nullptr, &new_tid);
        impl.known_tids[static_cast<pid_t>(new_tid)] = true;
        resume_tid(tid);
        return true;
    }

    if (sig != SIGTRAP) {
        resume_tid(tid, sig);
        return true;
    }

    user_regs_struct regs{};
    if (ptrace(PTRACE_GETREGS, tid, nullptr, &regs) != 0) {
        resume_tid(tid);
        return true;
    }
    auto bp_addr = static_cast<amd_dbgapi_global_address_t>(regs.rip - 1);

    uint64_t hit_id = 0;
    bool found = false;
    for (auto &kv : impl.breakpoints) {
        if (kv.second.addr == bp_addr) {
            hit_id = kv.first;
            found = true;
            break;
        }
    }

    if (!found) {
        // Unrelated SIGTRAP (e.g. our own single-step elsewhere).
        resume_tid(tid);
        return true;
    }

    // Standard software-breakpoint dance: rewind past the int3, restore the
    // original byte, single-step the real instruction, then re-arm.
    regs.rip = bp_addr;
    ptrace(PTRACE_SETREGS, tid, nullptr, &regs);

    BpInfo bp = impl.breakpoints[hit_id];
    int fd = mem_fd_for(impl);
    if (fd >= 0) {
        ::pwrite(fd, &bp.orig_byte, 1, static_cast<off_t>(bp.addr));
    }

    amd_dbgapi_breakpoint_id_t bp_id{hit_id};
    amd_dbgapi_breakpoint_action_t action = AMD_DBGAPI_BREAKPOINT_ACTION_RESUME;
    amd_dbgapi_client_thread_id_t client_thread = client_thread_for(tid);
    amd_dbgapi_status_t st = amd_dbgapi_report_breakpoint_hit(bp_id, client_thread, &action);
    if (st != AMD_DBGAPI_STATUS_SUCCESS) {
        log_status("report_breakpoint_hit", st);
    }

    ptrace(PTRACE_SINGLESTEP, tid, nullptr, nullptr);
    int step_status = 0;
    waitpid(tid, &step_status, 0);
    unsigned char int3 = 0xCC;
    if (fd >= 0) {
        ::pwrite(fd, &int3, 1, static_cast<off_t>(bp.addr));
    }

    if (action == AMD_DBGAPI_BREAKPOINT_ACTION_RESUME) {
        resume_tid(tid);
    }
    // Otherwise left halted pending a BREAKPOINT_RESUME event.
    return true;
}

AMDDbgApiSession::AMDDbgApiSession() : impl_(std::make_unique<Impl>()) {}
AMDDbgApiSession::~AMDDbgApiSession() {
    detach();
}

bool AMDDbgApiSession::attached() const {
    return impl_->process_id.handle != 0;
}

bool AMDDbgApiSession::launch(const std::string &host_binary,
                              const std::vector<std::string> &args) {
    amd_dbgapi_set_log_level(AMD_DBGAPI_LOG_LEVEL_WARNING);

    pid_t child = fork();
    if (child < 0) {
        return false;
    }
    if (child == 0) {
        ptrace(PTRACE_TRACEME, 0, nullptr, nullptr);
        std::vector<char *> argv;
        argv.reserve(args.size() + 2);
        std::string binary = host_binary;
        argv.push_back(binary.data());
        std::vector<std::string> owned(args);
        for (auto &arg : owned) {
            argv.push_back(arg.data());
        }
        argv.push_back(nullptr);
        execv(binary.c_str(), argv.data());
        std::perror("[ocldbg/amd] execv");
        _exit(127);
    }

    int status = 0;
    waitpid(child, &status, 0); // stop at exec
    if (!WIFSTOPPED(status)) {
        return false;
    }
    // Same ptrace() void*-as-flags-bag idiom as resume_tid().
    ptrace(PTRACE_SETOPTIONS, child, nullptr,
           reinterpret_cast<void *>( // NOLINT(performance-no-int-to-ptr)
               static_cast<long>(PTRACE_O_TRACECLONE | PTRACE_O_EXITKILL)));

    impl_->pid = child;
    impl_->known_tids[child] = true;

    static amd_dbgapi_callbacks_t callbacks{
        .allocate_memory = cb_allocate_memory,
        .deallocate_memory = cb_deallocate_memory,
        .client_process_get_info = cb_client_process_get_info,
        .insert_breakpoint = cb_insert_breakpoint,
        .remove_breakpoint = cb_remove_breakpoint,
        .xfer_global_memory = cb_xfer_global_memory,
        .log_message = cb_log_message,
    };

    amd_dbgapi_status_t init_st = amd_dbgapi_initialize(&callbacks);
    if (init_st != AMD_DBGAPI_STATUS_SUCCESS) {
        log_status("amd_dbgapi_initialize", init_st);
        ptrace(PTRACE_KILL, child, nullptr, nullptr);
        return false;
    }

    amd_dbgapi_status_t attach_st = amd_dbgapi_process_attach(
        reinterpret_cast<amd_dbgapi_client_process_id_t>(impl_.get()), &impl_->process_id);
    if (attach_st != AMD_DBGAPI_STATUS_SUCCESS) {
        log_status("amd_dbgapi_process_attach", attach_st);
        amd_dbgapi_finalize();
        ptrace(PTRACE_KILL, child, nullptr, nullptr);
        return false;
    }

    amd_dbgapi_process_get_info(impl_->process_id, AMD_DBGAPI_PROCESS_INFO_NOTIFIER,
                                sizeof(impl_->notifier), &impl_->notifier);

    resume_tid(child);
    return true;
}

void AMDDbgApiSession::detach() {
    if (impl_->process_id.handle != 0) {
        amd_dbgapi_process_detach(impl_->process_id);
        amd_dbgapi_finalize();
        impl_->process_id = amd_dbgapi_process_id_t{};
    }
    if (impl_->pid > 0) {
        int status = 0;
        waitpid(impl_->pid, &status, WNOHANG);
        impl_->pid = -1;
    }
    if (impl_->mem_fd >= 0) {
        close(impl_->mem_fd);
        impl_->mem_fd = -1;
    }
}

/// Drains amd-dbgapi's pending-event queue, updating impl_ state
/// (runtime_loaded) and resuming any thread a BREAKPOINT_RESUME event names.
/// Split out of resume_until_wave_stop() to keep its cognitive complexity
/// under the project's clang-tidy threshold.
void AMDDbgApiSession::drain_pending_events() {
    for (;;) {
        amd_dbgapi_event_id_t event_id{};
        amd_dbgapi_event_kind_t kind = AMD_DBGAPI_EVENT_KIND_NONE;
        amd_dbgapi_status_t st =
            amd_dbgapi_process_next_pending_event(impl_->process_id, &event_id, &kind);
        if (st != AMD_DBGAPI_STATUS_SUCCESS || kind == AMD_DBGAPI_EVENT_KIND_NONE) {
            break;
        }

        if (kind == AMD_DBGAPI_EVENT_KIND_RUNTIME) {
            uint32_t rstate = 0;
            amd_dbgapi_event_get_info(event_id, AMD_DBGAPI_EVENT_INFO_RUNTIME_STATE, sizeof(rstate),
                                      &rstate);
            if (rstate == AMD_DBGAPI_RUNTIME_STATE_LOADED_SUCCESS) {
                impl_->runtime_loaded = true;
                std::fprintf(stderr, "[ocldbg/amd] runtime loaded\n");
            }
        } else if (kind == AMD_DBGAPI_EVENT_KIND_BREAKPOINT_RESUME) {
            amd_dbgapi_client_thread_id_t client_thread{};
            // sizeof(client_thread) intentionally sizes the opaque
            // pointer-sized token itself, not a pointee, and it is already
            // explicitly cast to void* below.
            // NOLINTBEGIN(bugprone-sizeof-expression,bugprone-multi-level-implicit-pointer-conversion)
            amd_dbgapi_event_get_info(event_id, AMD_DBGAPI_EVENT_INFO_CLIENT_THREAD,
                                      sizeof(client_thread), static_cast<void *>(&client_thread));
            // NOLINTEND(bugprone-sizeof-expression,bugprone-multi-level-implicit-pointer-conversion)
            resume_tid(static_cast<pid_t>(reinterpret_cast<intptr_t>(client_thread)));
        }

        amd_dbgapi_event_processed(event_id);
    }
}

/// Refreshes impl_->known_waves from amd_dbgapi_process_wave_list(), only
/// replacing the cached set when the library reports a real change (see the
/// class comment on Impl::known_waves for why that check matters). Also
/// drives the agent/queue list queries, which appear to be part of how the
/// library's internal tracking progresses even though their results aren't
/// otherwise used here.
void AMDDbgApiSession::refresh_known_waves() {
    size_t agent_count = 0;
    amd_dbgapi_agent_id_t *agents = nullptr;
    amd_dbgapi_changed_t agent_changed = AMD_DBGAPI_CHANGED_NO;
    amd_dbgapi_process_agent_list(impl_->process_id, &agent_count, &agents, &agent_changed);
    free(agents);

    size_t queue_count = 0;
    amd_dbgapi_queue_id_t *queues = nullptr;
    amd_dbgapi_changed_t queue_changed = AMD_DBGAPI_CHANGED_NO;
    amd_dbgapi_process_queue_list(impl_->process_id, &queue_count, &queues, &queue_changed);
    free(queues);

    size_t wave_count = 0;
    amd_dbgapi_wave_id_t *waves = nullptr;
    amd_dbgapi_changed_t wave_changed = AMD_DBGAPI_CHANGED_NO;
    amd_dbgapi_status_t wave_st =
        amd_dbgapi_process_wave_list(impl_->process_id, &wave_count, &waves, &wave_changed);

    if (wave_st == AMD_DBGAPI_STATUS_SUCCESS && wave_changed == AMD_DBGAPI_CHANGED_YES) {
        impl_->known_waves.assign(waves, waves + wave_count);
    }
    free(waves);
}

/// Scans impl_->known_waves for one already in the STOP state (filling
/// `out` and returning true), proactively requesting amd_dbgapi_wave_stop()
/// on the first RUN wave found otherwise (gated to once per session via
/// Impl::wave_stop_requested).
bool AMDDbgApiSession::scan_waves_for_stop(AMDWaveInfo &out) {
    for (amd_dbgapi_wave_id_t wave : impl_->known_waves) {
        uint32_t wstate = 0;
        if (amd_dbgapi_wave_get_info(wave, AMD_DBGAPI_WAVE_INFO_STATE, sizeof(wstate), &wstate) !=
            AMD_DBGAPI_STATUS_SUCCESS) {
            continue;
        }
        if (wstate == AMD_DBGAPI_WAVE_STATE_STOP) {
            amd_dbgapi_global_address_t pc = 0;
            amd_dbgapi_wave_get_info(wave, AMD_DBGAPI_WAVE_INFO_PC, sizeof(pc), &pc);
            uint32_t stop_reason = 0;
            amd_dbgapi_wave_get_info(wave, AMD_DBGAPI_WAVE_INFO_STOP_REASON, sizeof(stop_reason),
                                     &stop_reason);
            out = AMDWaveInfo{wave, pc, stop_reason};
            return true;
        }
        if (!impl_->wave_stop_requested) {
            impl_->wave_stop_requested = true;
            amd_dbgapi_status_t sst = amd_dbgapi_wave_stop(wave);
            std::fprintf(stderr, "[ocldbg/amd] requested wave_stop on wave %lu -> status=%d\n",
                         static_cast<unsigned long>(wave.handle), static_cast<int>(sst));
        }
    }
    return false;
}

bool AMDDbgApiSession::resume_until_wave_stop(AMDWaveInfo &out) {
    if (!attached()) {
        return false;
    }

    auto t_start = std::chrono::steady_clock::now();
    while (true) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::steady_clock::now() - t_start)
                           .count();
        if (elapsed > 60) {
            return false; // give up rather than hang forever
        }

        int wstatus = 0;
        pid_t w = waitpid(-1, &wstatus, WNOHANG | __WALL);
        if (w > 0) {
            if (!handle_stop(*impl_, w, wstatus)) {
                return false; // main process exited
            }
        } else {
            pollfd pfd{.fd = impl_->notifier, .events = POLLIN, .revents = 0};
            poll(&pfd, 1, 50);
        }

        drain_pending_events();

        if (!impl_->runtime_loaded) {
            continue;
        }

        refresh_known_waves();

        if (scan_waves_for_stop(out)) {
            return true;
        }
    }
}

size_t AMDDbgApiSession::read_memory(HostAddress addr, void *buf, size_t length) const {
    int fd = mem_fd_for(*impl_);
    if (fd < 0) {
        return 0;
    }
    ssize_t n = ::pread(fd, buf, length, static_cast<off_t>(addr));
    return n > 0 ? static_cast<size_t>(n) : 0;
}

bool AMDDbgApiSession::read_register(amd_dbgapi_wave_id_t wave, amd_dbgapi_register_id_t reg,
                                     size_t offset, size_t size, void *out) const {
    return amd_dbgapi_read_register(wave, reg, offset, size, out) == AMD_DBGAPI_STATUS_SUCCESS;
}

std::optional<amd_dbgapi_register_id_t>
AMDDbgApiSession::dwarf_register(amd_dbgapi_wave_id_t wave, uint64_t dwarf_reg_num) const {
    amd_dbgapi_architecture_id_t arch{};
    if (amd_dbgapi_wave_get_info(wave, AMD_DBGAPI_WAVE_INFO_ARCHITECTURE, sizeof(arch), &arch) !=
        AMD_DBGAPI_STATUS_SUCCESS) {
        return std::nullopt;
    }
    amd_dbgapi_register_id_t reg{};
    if (amd_dbgapi_dwarf_register_to_register(arch, dwarf_reg_num, &reg) !=
        AMD_DBGAPI_STATUS_SUCCESS) {
        return std::nullopt;
    }
    return reg;
}

} // namespace ocldbg
