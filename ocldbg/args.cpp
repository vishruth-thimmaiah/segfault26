#include "args.h"

#include "ocldbg/DebuggerContext.h"

#include <format>
#include <iostream>
#include <string>
#include <string_view>

namespace ocldbg {

namespace {

bool parse_string_opt(std::string_view arg, std::string_view short_flag, std::string_view long_flag,
                      int &i, int argc, char **argv, std::string &out) {
    if ((arg == short_flag || arg == long_flag) && (i + 1 < argc)) {
        out = argv[++i];
        return true;
    }
    return false;
}

bool parse_vector_opt(std::string_view arg, std::string_view short_flag, std::string_view long_flag,
                      int &i, int argc, char **argv, std::vector<std::string> &out) {
    if ((arg == short_flag || arg == long_flag) && (i + 1 < argc)) {
        out.emplace_back(argv[++i]);
        return true;
    }
    return false;
}

bool parse_flag_opts(ProgramArgs &args, std::string_view arg, int &i, int argc, char **argv) {
    if (parse_vector_opt(arg, "-O", "--one-line-before-file", i, argc, argv,
                         args.one_line_before) ||
        parse_vector_opt(arg, "-S", "--source-before-file", i, argc, argv, args.source_before) ||
        parse_vector_opt(arg, "-s", "--source", i, argc, argv, args.source_after) ||
        parse_vector_opt(arg, "-o", "--one-line", i, argc, argv, args.one_line_after) ||
        parse_vector_opt(arg, "", "--print", i, argc, argv, args.print_exprs) ||
        parse_string_opt(arg, "", "--backend", i, argc, argv, args.backend_name)) {
        return true;
    }

    if (arg == "-b" || arg == "--batch") {
        args.batch_mode = true;
        return true;
    }
    if (arg == "--dry-run") {
        args.dry_run = true;
        return true;
    }
    if (arg == "--show-wg-bounds") {
        args.show_wg_bounds = true;
        return true;
    }
    if (arg == "--track-wg") {
        args.track_wg = true;
        return true;
    }
    if (arg == "--dap") {
        args.dap_mode = true;
        return true;
    }
    if (arg == "--inspect-vars") {
        args.inspect_vars = true;
        return true;
    }
    return false;
}

bool parse_numeric_opts(ProgramArgs &args, std::string_view arg, int &i, int argc, char **argv) {
    if (arg == "--break-at" && (i + 1 < argc)) {
        args.break_at = static_cast<unsigned>(std::stoul(argv[++i]));
        return true;
    }
    if (arg == "--break-for" && (i + 1 < argc)) {
        args.break_for = static_cast<size_t>(std::stoull(argv[++i]));
        return true;
    }
    if (arg == "--port" && (i + 1 < argc)) {
        args.dap_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        return true;
    }
    return false;
}

void parse_positional_arg(ProgramArgs &args, std::string_view arg) {
    if (args.host_binary.empty()) {
        if (!arg.empty() && arg.front() != '-') {
            args.host_binary = arg;
        }
    } else {
        args.host_args.emplace_back(arg);
    }
}

void parse_trailing_args(ProgramArgs &args, int &i, int argc, char **argv) {
    for (++i; i < argc; ++i) {
        if (args.host_binary.empty()) {
            args.host_binary = argv[i];
        } else {
            args.host_args.emplace_back(argv[i]);
        }
    }
}

} // namespace

ProgramArgs parse_arguments(int argc, char **argv) {
    ProgramArgs args;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (!args.host_binary.empty()) {
            args.host_args.emplace_back(arg);
            continue;
        }
        if (arg == "-v" || arg == "--version") {
            args.show_version = true;
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            args.show_help = true;
            continue;
        }
        if (parse_flag_opts(args, arg, i, argc, argv) ||
            parse_numeric_opts(args, arg, i, argc, argv)) {
            continue;
        }
        if (arg == "--") {
            parse_trailing_args(args, i, argc, argv);
            break;
        }
        parse_positional_arg(args, arg);
    }
    return args;
}

void print_version() {
    std::string lldb_version = DebuggerContext::init();
#ifndef OCLDBG_GIT_COMMIT
#define OCLDBG_GIT_COMMIT "unknown"
#endif
#ifndef OCLDBG_VERSION
#define OCLDBG_VERSION "0.1.0"
#endif
    std::cout << std::format("ocldbg version {} (commit {})\nUsing {}\n", OCLDBG_VERSION,
                             OCLDBG_GIT_COMMIT, lldb_version);
    DebuggerContext::terminate();
}

void print_help() {
    std::cout << std::format(
        "Usage: ocldbg [options] [host_binary] [args...]\n"
        "Options:\n"
        "  -o <command>       Execute the LLDB command after loading target\n"
        "  -O <command>       Execute the LLDB command before loading target\n"
        "  -s <file>          Source LLDB commands from file after loading target\n"
        "  -S <file>          Source LLDB commands from file before loading target\n"
        "  -b, --batch        Exit after running one-line and sourced commands\n"
        "  --dry-run          [TEMP] Test launch and NDRange inference without full execution\n"
        "  --show-wg-bounds   Display inferred work-group bounds and coordinate mapping\n"
        "  --track-wg         Track live work-group dispatches (implies kernel runs to "
        "completion)\n"
        "  --inspect-vars     Inspect variables at first work-group stop\n"
        "  --break-at <line>  Break at kernel source line (e.g. --break-at 17)\n"
        "  --break-for <x>    Stop at breakpoint x times (default: every time)\n"
        "  --print <expr>     Evaluate expression at each stop (oclgrind backend; repeatable)\n"
        "  --backend <name>   Execution backend: cpu|oclgrind|amd (default: cpu)\n"
        "  --dap              Run DAP server over stdio\n"
        "  --port <port>      Listen on TCP port for DAP client (default: stdio)\n"
        "  -v, --version      Display version and build information\n"
        "  -h, --help         Display this help message\n");
}

} // namespace ocldbg
