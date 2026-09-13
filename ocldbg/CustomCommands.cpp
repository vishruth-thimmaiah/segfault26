#include "CustomCommands.h"

#include "ocldbg/DebuggerContext.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <iostream>
#include <string>
#include <string_view>

namespace ocldbg {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (std::isspace(static_cast<unsigned char>(s.front())) != 0)) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (std::isspace(static_cast<unsigned char>(s.back())) != 0)) {
        s.remove_suffix(1);
    }
    return s;
}

void handle_break_list(const DebuggerContext &dbg) {
    auto bps = dbg.list_ocl_breakpoints();
    if (bps.empty()) {
        std::cout << "[ocldbg] No OpenCL breakpoints set.\n";
        return;
    }
    std::cout << "[ocldbg] OpenCL Breakpoints:\n";
    for (const auto &bp : bps) {
        std::string fname = bp.file.empty() ? "<kernel>" : bp.file;
        if (bp.resolved) {
            std::cout << std::format("  #{}: {}:{} (resolved at {:#x})\n", bp.id, fname, bp.line,
                                     bp.address);
        } else {
            std::cout << std::format("  #{}: {}:{} (pending)\n", bp.id, fname, bp.line);
        }
    }
}

void handle_break_delete(DebuggerContext &dbg, std::string_view arg) {
    arg = trim(arg);
    if (arg.empty()) {
        std::cerr << "[ocldbg] Error: Missing breakpoint ID to delete\n";
        return;
    }
    try {
        size_t id = std::stoull(std::string(arg));
        if (dbg.delete_ocl_breakpoint(id)) {
            std::cout << std::format("[ocldbg] Deleted OpenCL breakpoint #{}\n", id);
        } else {
            std::cerr << std::format("[ocldbg] Error: OpenCL breakpoint #{} not found\n", id);
        }
    } catch (const std::exception &) {
        std::cerr << std::format("[ocldbg] Error: Invalid breakpoint ID '{}'\n", arg);
    }
}

void handle_break_add(DebuggerContext &dbg, std::string_view spec) {
    spec = trim(spec);
    if (spec.empty()) {
        std::cerr
            << "[ocldbg] Error: Missing breakpoint location (usage: ocl break [<file>:]<line>)\n";
        return;
    }

    std::string file;
    unsigned line = 0;
    auto colon = spec.rfind(':');
    if (colon != std::string_view::npos) {
        file = std::string(spec.substr(0, colon));
        std::string line_part = std::string(spec.substr(colon + 1));
        try {
            line = static_cast<unsigned>(std::stoul(line_part));
        } catch (const std::exception &) {
            std::cerr << std::format("[ocldbg] Error: Invalid line number '{}'\n", line_part);
            return;
        }
    } else {
        try {
            line = static_cast<unsigned>(std::stoul(std::string(spec)));
        } catch (const std::exception &) {
            std::cerr << std::format("[ocldbg] Error: Invalid breakpoint specification '{}'\n",
                                     spec);
            return;
        }
    }

    size_t id = dbg.add_ocl_breakpoint(file, line);
    auto bps = dbg.list_ocl_breakpoints();
    bool resolved = false;
    uint64_t addr = 0;
    for (const auto &bp : bps) {
        if (bp.id == id) {
            resolved = bp.resolved;
            addr = bp.address;
            break;
        }
    }

    std::string fname = file.empty() ? "<kernel>" : file;
    if (resolved) {
        std::cout << std::format("[ocldbg] Breakpoint #{}: resolved at address {:#x} ({}:{})\n", id,
                                 addr, fname, line);
    } else {
        std::cout << std::format("[ocldbg] Breakpoint #{} (pending) set at {}:{}\n", id, fname,
                                 line);
    }
}

void handle_break_command(DebuggerContext &dbg, std::string_view subcmd) {
    subcmd = trim(subcmd);
    if (subcmd.empty() || subcmd == "list") {
        handle_break_list(dbg);
        return;
    }

    if (subcmd.starts_with("delete ") || subcmd.starts_with("del ")) {
        auto space = subcmd.find(' ');
        handle_break_delete(dbg, subcmd.substr(space + 1));
        return;
    }

    handle_break_add(dbg, subcmd);
}

} // namespace

bool handle_ocl_command(DebuggerContext &dbg, std::string_view cmd) {
    cmd = trim(cmd);
    if (!cmd.starts_with("ocl")) {
        return false;
    }
    cmd.remove_prefix(3);
    cmd = trim(cmd);

    if (cmd.starts_with("break ") || cmd == "break") {
        cmd.remove_prefix(std::min<size_t>(cmd.size(), 5));
        handle_break_command(dbg, cmd);
        return true;
    }

    if (cmd.starts_with("b ") || cmd == "b") {
        cmd.remove_prefix(std::min<size_t>(cmd.size(), 1));
        handle_break_command(dbg, cmd);
        return true;
    }

    std::cout << "(unsupported)\n";
    return true;
}

} // namespace ocldbg
