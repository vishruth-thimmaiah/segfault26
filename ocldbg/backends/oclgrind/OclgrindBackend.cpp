#include "OclgrindBackend.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace ocldbg {

namespace {

using namespace oclgrind_proto;

/// Overridable so an ocldbg built against one Oclgrind install can use another.
std::string plugin_library() {
    if (const char *override_path = std::getenv("OCLDBG_OCLGRIND_PLUGIN")) {
        return override_path;
    }
    return OCLDBG_OCLGRIND_PLUGIN_PATH;
}

std::string oclgrind_lib_dir() {
    if (const char *override_path = std::getenv("OCLDBG_OCLGRIND_LIBDIR")) {
        return override_path;
    }
    return OCLDBG_OCLGRIND_LIB_DIR;
}

std::string prepend_path(const std::string &head, const char *existing) {
    if (existing == nullptr || *existing == '\0') {
        return head;
    }
    return head + ":" + existing;
}

std::filesystem::path make_socket_path() {
    std::string name = "ocldbg-oclgrind-" + std::to_string(::getpid()) + ".sock";
    return std::filesystem::temp_directory_path() / name;
}

int listen_on(const std::filesystem::path &path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::string path_str = path.string();
    if (path_str.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        return -1;
    }
    path_str.copy(static_cast<char *>(addr.sun_path), path_str.size());

    std::filesystem::remove(path);
    if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
        ::listen(fd, 1) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

/// Gives up if the host program dies before connecting.
int accept_plugin(int listen_fd, pid_t child) {
    for (;;) {
        pollfd poll_fd{.fd = listen_fd, .events = POLLIN, .revents = 0};
        int ready = ::poll(&poll_fd, 1, 100);
        if (ready > 0) {
            return ::accept(listen_fd, nullptr, nullptr);
        }
        if (ready < 0) {
            return -1;
        }
        int status = 0;
        if (::waitpid(child, &status, WNOHANG) == child) {
            return -1;
        }
    }
}

[[noreturn]] void exec_host(const std::string &host_binary, const std::vector<std::string> &args,
                            const std::filesystem::path &socket_path) {
    std::string lib_dir = oclgrind_lib_dir();
    ::setenv(kSocketEnvVar, socket_path.c_str(), 1);
    ::setenv("OCLGRIND_PLUGINS", plugin_library().c_str(), 1);
    // Replaces the OpenCL API outright, so the kernel runs on the simulator
    // whatever the system ICD configuration says.
    ::setenv("LD_PRELOAD",
             prepend_path(lib_dir + "/liboclgrind-rt.so", std::getenv("LD_PRELOAD")).c_str(), 1);
    ::setenv("LD_LIBRARY_PATH", prepend_path(lib_dir, std::getenv("LD_LIBRARY_PATH")).c_str(), 1);

    std::vector<char *> argv;
    argv.reserve(args.size() + 2);
    std::string binary = host_binary;
    argv.push_back(binary.data());
    std::vector<std::string> owned(args);
    for (auto &arg : owned) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    ::execvp(binary.c_str(), argv.data());
    std::cerr << "[ocldbg] Failed to exec host binary: " << host_binary << "\n";
    ::_exit(127);
}

std::vector<size_t> parse_numbers(const std::string &line, size_t skip_words) {
    std::istringstream stream(line);
    std::string word;
    for (size_t i = 0; i < skip_words; ++i) {
        stream >> word;
    }
    std::vector<size_t> values;
    size_t value = 0;
    while (stream >> value) {
        values.push_back(value);
    }
    return values;
}

std::string first_word(const std::string &line) {
    std::istringstream stream(line);
    std::string word;
    stream >> word;
    return word;
}

bool request(LineChannel &channel, std::string_view command, std::string &reply) {
    return channel.send(command) && channel.recv(reply);
}

size_t decode_hex(const std::string &hex, void *buf, size_t capacity) {
    auto *out = static_cast<unsigned char *>(buf);
    size_t count = std::min(hex.size() / 2, capacity);
    for (size_t i = 0; i < count; ++i) {
        out[i] = static_cast<unsigned char>(std::stoul(hex.substr(i * 2, 2), nullptr, 16));
    }
    return count;
}

} // namespace

struct OclgrindBackend::Impl {
    LineChannel channel;
    int listen_fd = -1;
    std::filesystem::path socket_path;
    pid_t child = -1;

    bool halted = false;
    unsigned stop_line = 0;
    OCLWorkItem stopped;
    OclgrindExecContext exec_ctx;

    std::map<uint64_t, unsigned> breakpoints; ///< id -> source line
    uint64_t next_breakpoint_id = 1;
};

OclgrindBackend::OclgrindBackend() : impl_(std::make_unique<Impl>()) {}

OclgrindBackend::~OclgrindBackend() {
    detach();
}

bool OclgrindBackend::launch(const std::string &host_binary, const std::vector<std::string> &args) {
    if (plugin_library().empty()) {
        std::cerr << "[ocldbg] Built without Oclgrind support; rebuild with Oclgrind installed\n";
        return false;
    }

    impl_->socket_path = make_socket_path();
    impl_->listen_fd = listen_on(impl_->socket_path);
    if (impl_->listen_fd < 0) {
        std::cerr << "[ocldbg] Could not listen on " << impl_->socket_path << "\n";
        return false;
    }

    impl_->child = ::fork();
    if (impl_->child < 0) {
        std::cerr << "[ocldbg] fork failed\n";
        return false;
    }
    if (impl_->child == 0) {
        ::close(impl_->listen_fd);
        exec_host(host_binary, args, impl_->socket_path);
    }

    int fd = accept_plugin(impl_->listen_fd, impl_->child);
    if (fd < 0) {
        std::cerr << "[ocldbg] Host program exited before the Oclgrind plugin connected\n";
        detach();
        return false;
    }
    impl_->channel = LineChannel(fd);

    std::string event;
    if (!impl_->channel.recv(event) || first_word(event) != kEventReady) {
        std::cerr << "[ocldbg] Unexpected handshake from Oclgrind plugin\n";
        detach();
        return false;
    }
    impl_->halted = true;

    // Breakpoints registered before launch only reach the plugin now.
    for (const auto &[id, line] : impl_->breakpoints) {
        std::string reply;
        std::ostringstream command;
        command << kCmdBreak << ' ' << id << ' ' << line;
        request(impl_->channel, command.str(), reply);
    }
    return true;
}

void OclgrindBackend::detach() {
    impl_->channel.close();
    if (impl_->listen_fd >= 0) {
        ::close(impl_->listen_fd);
        impl_->listen_fd = -1;
    }
    if (!impl_->socket_path.empty()) {
        std::filesystem::remove(impl_->socket_path);
        impl_->socket_path.clear();
    }
    if (impl_->child > 0) {
        int status = 0;
        ::waitpid(impl_->child, &status, 0);
        impl_->child = -1;
    }
    impl_->halted = false;
}

uint64_t OclgrindBackend::set_breakpoint(const SourceLocation &loc) {
    if (loc.line == 0) {
        return 0;
    }
    // loc.file is ignored: an Oclgrind Program is built from a single source.
    uint64_t id = impl_->next_breakpoint_id++;
    impl_->breakpoints[id] = loc.line;

    if (impl_->halted) {
        std::ostringstream command;
        command << kCmdBreak << ' ' << id << ' ' << loc.line;
        std::string reply;
        if (!request(impl_->channel, command.str(), reply) || first_word(reply) != kReplyOk) {
            impl_->breakpoints.erase(id);
            return 0;
        }
    }
    return id;
}

void OclgrindBackend::remove_breakpoint(uint64_t bp_id) {
    if (impl_->breakpoints.erase(bp_id) == 0) {
        return;
    }
    if (impl_->halted) {
        std::string reply;
        request(impl_->channel, std::string(kCmdDelete) + ' ' + std::to_string(bp_id), reply);
    }
}

void OclgrindBackend::on_stop(StopCallback cb) {
    stop_cb_ = std::move(cb);
}

void OclgrindBackend::resume() {
    resume_with(kCmdContinue);
}

void OclgrindBackend::step_over(const OCLWorkItem &wi) {
    OCLWorkItem selected;
    if (select_work_item(wi.global_id, selected)) {
        resume_with(std::string(kCmdStep) + ' ' + std::string(kStepOver));
    }
}

void OclgrindBackend::step_in(const OCLWorkItem &wi) {
    OCLWorkItem selected;
    if (select_work_item(wi.global_id, selected)) {
        resume_with(std::string(kCmdStep) + ' ' + std::string(kStepIn));
    }
}

void OclgrindBackend::resume_with(std::string_view command) {
    if (!impl_->halted) {
        return;
    }
    impl_->halted = false;
    if (!impl_->channel.send(command)) {
        return;
    }

    std::string event;
    if (!impl_->channel.recv(event)) {
        // End of stream: the kernel finished and the host program is exiting.
        impl_->channel.close();
        return;
    }
    if (first_word(event) != kEventStopped) {
        return;
    }

    std::vector<size_t> fields = parse_numbers(event, 1);
    if (fields.size() < 10) {
        return;
    }
    impl_->stop_line = static_cast<unsigned>(fields[0]);
    impl_->exec_ctx.global_id = {.x = fields[1], .y = fields[2], .z = fields[3]};
    impl_->exec_ctx.channel = &impl_->channel;
    impl_->stopped = OCLWorkItem{
        .global_id = impl_->exec_ctx.global_id,
        .local_id = {.x = fields[4], .y = fields[5], .z = fields[6]},
        .group_id = {.x = fields[7], .y = fields[8], .z = fields[9]},
        .exec_ctx = &impl_->exec_ctx,
    };
    impl_->halted = true;

    if (stop_cb_) {
        // Peers are reachable through select_work_item() rather than listed.
        stop_cb_(OCLStopContext{.stopped = impl_->stopped, .visible = {impl_->stopped}});
    }
}

bool OclgrindBackend::select_work_item(const Size3 &global_id, OCLWorkItem &out) {
    if (!impl_->halted) {
        return false;
    }
    std::ostringstream command;
    command << kCmdSelect << ' ' << global_id.x << ' ' << global_id.y << ' ' << global_id.z;

    std::string reply;
    if (!request(impl_->channel, command.str(), reply) || first_word(reply) != kReplyOk) {
        return false;
    }
    std::vector<size_t> fields = parse_numbers(reply, 1);
    if (fields.size() < 6) {
        return false;
    }

    impl_->exec_ctx.global_id = global_id;
    impl_->exec_ctx.channel = &impl_->channel;
    out = OCLWorkItem{
        .global_id = global_id,
        .local_id = {.x = fields[0], .y = fields[1], .z = fields[2]},
        .group_id = {.x = fields[3], .y = fields[4], .z = fields[5]},
        .exec_ctx = &impl_->exec_ctx,
    };
    impl_->stopped = out;
    return true;
}

LocationBackend &OclgrindBackend::location_backend() {
    return loc_backend_;
}

size_t OclgrindBackend::read_global_memory(HostAddress addr, void *buf, size_t length) {
    if (!impl_->halted || length == 0) {
        return 0;
    }
    std::ostringstream command;
    command << kCmdRead << ' ' << addr << ' ' << length;

    std::string reply;
    if (!request(impl_->channel, command.str(), reply)) {
        return 0;
    }
    std::istringstream stream(reply);
    std::string tag;
    std::string hex;
    stream >> tag >> hex;
    if (tag != kReplyData) {
        return 0;
    }
    return decode_hex(hex, buf, length);
}

bool OclgrindBackend::halted() const {
    return impl_->halted;
}

unsigned OclgrindBackend::last_stop_line() const {
    return impl_->stop_line;
}

VarValue OclgrindLocationBackend::evaluate(const VarInfo &var, ExecCtxHandle exec_ctx) {
    auto *ctx = static_cast<OclgrindExecContext *>(exec_ctx);
    if (ctx == nullptr || ctx->channel == nullptr || var.name.empty()) {
        return {};
    }

    std::string reply;
    if (!ctx->channel->send(std::string(kCmdPrint) + ' ' + var.name) ||
        !ctx->channel->recv(reply)) {
        return {};
    }
    if (first_word(reply) != kReplyValue) {
        return {.name = var.name, .type_name = var.type_name, .address_space = var.address_space};
    }

    std::string value = reply.substr(std::string(kReplyValue).size() + 1);
    return {
        .name = var.name,
        .type_name = var.type_name,
        .value_str = value,
        .address_space = var.address_space,
        .available = true,
    };
}

} // namespace ocldbg
