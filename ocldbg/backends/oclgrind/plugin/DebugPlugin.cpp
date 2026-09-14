#include "DebugPlugin.h"

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Instruction.h>
#include <oclgrind/Context.h>
#include <oclgrind/KernelInvocation.h>
#include <oclgrind/Memory.h>
#include <oclgrind/WorkGroup.h>
#include <oclgrind/WorkItem.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/un.h>

namespace ocldbg {

namespace {

using namespace oclgrind_proto;

/// Returns a closed channel when the environment variable is absent, which
/// leaves the plugin inert.
LineChannel connect_to_debugger() {
    const char *path = std::getenv(kSocketEnvVar);
    if (path == nullptr) {
        return {};
    }

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return {};
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::string_view path_view(path);
    if (path_view.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        return {};
    }
    path_view.copy(static_cast<char *>(addr.sun_path), path_view.size());

    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return {};
    }
    return LineChannel(fd);
}

std::vector<std::string> split_words(const std::string &line) {
    std::istringstream stream(line);
    std::vector<std::string> words;
    std::string word;
    while (stream >> word) {
        words.push_back(word);
    }
    return words;
}

size_t line_of(const llvm::Instruction *instruction) {
    const llvm::DebugLoc &loc = instruction->getDebugLoc();
    return loc ? loc.getLine() : 0;
}

std::string to_hex(const std::vector<unsigned char> &bytes) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (unsigned char byte : bytes) {
        out << std::setw(2) << static_cast<unsigned>(byte);
    }
    return out.str();
}

} // namespace

DebugPlugin::DebugPlugin(const oclgrind::Context *context)
    : oclgrind::Plugin(context), channel_(connect_to_debugger()) {
    if (!channel_.valid()) {
        return;
    }
    // The debugger registers its breakpoints before letting the host run.
    channel_.send(kEventReady);
    serve();
}

void DebugPlugin::kernelBegin(const oclgrind::KernelInvocation *invocation) {
    invocation_ = invocation;
    last_line_.clear();
}

void DebugPlugin::kernelEnd(const oclgrind::KernelInvocation * /*invocation*/) {
    invocation_ = nullptr;
    last_line_.clear();
}

void DebugPlugin::instructionExecuted(const oclgrind::WorkItem *work_item,
                                      const llvm::Instruction *instruction,
                                      const oclgrind::TypedValue & /*result*/) {
    if (!channel_.valid()) {
        return;
    }

    size_t line = line_of(instruction);
    if (line == 0 || !should_halt(work_item, line)) {
        return;
    }

    last_line_[work_item->getGlobalIndex()] = line;
    report_stop(work_item, line);
    serve();
}

bool DebugPlugin::should_halt(const oclgrind::WorkItem *work_item, size_t line) const {
    size_t index = work_item->getGlobalIndex();
    auto previous = last_line_.find(index);
    if (previous != last_line_.end() && previous->second == line) {
        return false;
    }

    switch (mode_) {
    case RunMode::kRunning:
        return breakpoint_lines_.contains(line);
    case RunMode::kStepIn:
        return index == stepping_index_;
    case RunMode::kStepOver:
        return index == stepping_index_ && work_item->getCallStack().size() <= stepping_depth_;
    }
    return false;
}

void DebugPlugin::report_stop(const oclgrind::WorkItem *work_item, size_t line) {
    oclgrind::Size3 global = work_item->getGlobalID();
    oclgrind::Size3 local = work_item->getLocalID();
    oclgrind::Size3 group = work_item->getWorkGroup()->getGroupID();

    std::ostringstream event;
    event << kEventStopped << ' ' << line << ' ' << global.x << ' ' << global.y << ' ' << global.z
          << ' ' << local.x << ' ' << local.y << ' ' << local.z << ' ' << group.x << ' ' << group.y
          << ' ' << group.z;
    channel_.send(event.str());
}

const oclgrind::WorkItem *DebugPlugin::active_work_item() const {
    return invocation_ != nullptr ? invocation_->getCurrentWorkItem() : nullptr;
}

void DebugPlugin::serve() {
    std::string command;
    while (channel_.recv(command)) {
        bool resume = false;
        if (!handle_command(command, resume)) {
            channel_.close();
            return;
        }
        if (resume) {
            return;
        }
    }
    // The debugger went away; let the host program run to completion.
    channel_.close();
}

bool DebugPlugin::handle_command(const std::string &command, bool &resume) {
    std::vector<std::string> words = split_words(command);
    if (words.empty()) {
        return channel_.send(std::string(kReplyErr) + " empty command");
    }

    const std::string &verb = words[0];
    if (verb == kCmdContinue || verb == kCmdStep) {
        return handle_execution(words, resume);
    }
    if (verb == kCmdBreak || verb == kCmdDelete) {
        return handle_breakpoint(words);
    }
    return handle_query(words);
}

bool DebugPlugin::handle_execution(const std::vector<std::string> &words, bool &resume) {
    if (words[0] == kCmdContinue) {
        mode_ = RunMode::kRunning;
        resume = true;
        return true;
    }

    const oclgrind::WorkItem *work_item = active_work_item();
    if (work_item == nullptr) {
        return channel_.send(std::string(kReplyErr) + " no work-item is halted");
    }
    bool over = words.size() > 1 && words[1] == kStepOver;
    mode_ = over ? RunMode::kStepOver : RunMode::kStepIn;
    stepping_index_ = work_item->getGlobalIndex();
    stepping_depth_ = work_item->getCallStack().size();
    resume = true;
    return true;
}

bool DebugPlugin::handle_breakpoint(const std::vector<std::string> &words) {
    if (words[0] == kCmdBreak) {
        if (words.size() < 3) {
            return channel_.send(std::string(kReplyErr) + " break needs an id and a line");
        }
        size_t id = std::strtoul(words[1].c_str(), nullptr, 10);
        size_t line = std::strtoul(words[2].c_str(), nullptr, 10);
        if (line == 0) {
            return channel_.send(std::string(kReplyErr) + " invalid line");
        }
        breakpoint_lines_.insert(line);
        breakpoint_ids_[id] = line;
        return channel_.send(std::string(kReplyOk));
    }

    if (words.size() < 2) {
        return channel_.send(std::string(kReplyErr) + " delete needs an id");
    }
    auto entry = breakpoint_ids_.find(std::strtoul(words[1].c_str(), nullptr, 10));
    if (entry == breakpoint_ids_.end()) {
        return channel_.send(std::string(kReplyErr) + " unknown breakpoint");
    }
    size_t line = entry->second;
    breakpoint_ids_.erase(entry);

    // Several ids may share one line; the line stays armed until the last goes.
    bool still_used = false;
    for (const auto &[other_id, other_line] : breakpoint_ids_) {
        still_used = still_used || other_line == line;
    }
    if (!still_used) {
        breakpoint_lines_.erase(line);
    }
    return channel_.send(std::string(kReplyOk));
}

bool DebugPlugin::handle_query(const std::vector<std::string> &words) {
    const std::string &verb = words[0];

    if (verb == kCmdSelect && words.size() > 3) {
        std::vector<size_t> global_id;
        global_id.reserve(3);
        for (size_t i = 1; i <= 3; ++i) {
            global_id.push_back(std::strtoul(words[i].c_str(), nullptr, 10));
        }
        if (!select_work_item(global_id)) {
            return channel_.send(std::string(kReplyErr) + " work-item state unavailable");
        }
        const oclgrind::WorkItem *selected = invocation_->getCurrentWorkItem();
        oclgrind::Size3 local = selected->getLocalID();
        oclgrind::Size3 group = selected->getWorkGroup()->getGroupID();
        std::ostringstream reply;
        reply << kReplyOk << ' ' << local.x << ' ' << local.y << ' ' << local.z << ' ' << group.x
              << ' ' << group.y << ' ' << group.z;
        return channel_.send(reply.str());
    }

    if (verb == kCmdPrint && words.size() > 1) {
        const oclgrind::WorkItem *work_item = active_work_item();
        if (work_item == nullptr) {
            return channel_.send(std::string(kReplyErr) + " no work-item is halted");
        }
        std::string value = read_expression(work_item, words[1]);
        if (value.empty()) {
            return channel_.send(std::string(kReplyErr) + " not in scope");
        }
        return channel_.send(std::string(kReplyValue) + ' ' + value);
    }

    if (verb == kCmdRead && words.size() > 2) {
        size_t address = std::strtoull(words[1].c_str(), nullptr, 10);
        size_t length = std::strtoul(words[2].c_str(), nullptr, 10);
        std::string hex = read_memory(address, length);
        if (hex.empty() && length > 0) {
            return channel_.send(std::string(kReplyErr) + " invalid address");
        }
        return channel_.send(std::string(kReplyData) + ' ' + hex);
    }

    return channel_.send(std::string(kReplyErr) + " unsupported command");
}

std::string DebugPlugin::read_expression(const oclgrind::WorkItem *work_item,
                                         const std::string &expr) const {
    // Oclgrind formats values straight to stdout and offers no way to obtain
    // them as data, so borrow its formatting by capturing the stream.
    std::ostringstream captured;
    std::streambuf *previous = std::cout.rdbuf(captured.rdbuf());
    work_item->printExpression(expr);
    std::cout.rdbuf(previous);

    std::string value = captured.str();
    // Oclgrind prints this for an unknown name; report it as unresolved rather
    // than passing the text off as a value.
    if (value.empty() || value == "not found") {
        return {};
    }
    // Keep the reply on one line; the protocol is newline-framed.
    for (char &character : value) {
        if (character == '\n' || character == '\r') {
            character = ' ';
        }
    }
    return value;
}

std::string DebugPlugin::read_memory(size_t address, size_t length) const {
    oclgrind::Memory *memory = m_context->getGlobalMemory();
    if (memory == nullptr || !memory->isAddressValid(address, length)) {
        return {};
    }
    std::vector<unsigned char> bytes(length);
    if (!memory->load(bytes.data(), address, length)) {
        return {};
    }
    return to_hex(bytes);
}

bool DebugPlugin::select_work_item(const std::vector<size_t> &global_id) {
    if (invocation_ == nullptr) {
        return false;
    }
    oclgrind::Size3 target(global_id[0], global_id[1], global_id[2]);
    oclgrind::Size3 global_size = invocation_->getGlobalSize();
    if (target.x >= global_size.x || target.y >= global_size.y || target.z >= global_size.z) {
        return false;
    }
    // Switching mutates the simulation, which the plugin interface otherwise
    // treats as read-only; Oclgrind's own debugger casts here too.
    return const_cast<oclgrind::KernelInvocation *>(invocation_)->switchWorkItem(target);
}

} // namespace ocldbg

namespace {
/// registerPlugin() records dynamically loaded plugins as unowned, so this
/// library is responsible for the instance it creates.
ocldbg::DebugPlugin *g_plugin = nullptr;
} // namespace

void initializePlugins(oclgrind::Context *context) {
    g_plugin = new ocldbg::DebugPlugin(context);
    context->registerPlugin(g_plugin);
}

void releasePlugins(oclgrind::Context *context) {
    if (g_plugin != nullptr) {
        context->unregisterPlugin(g_plugin);
        delete g_plugin;
        g_plugin = nullptr;
    }
}
