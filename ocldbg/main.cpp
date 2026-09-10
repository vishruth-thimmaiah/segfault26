/**
 * ocldbg — Source-Level OpenCL Debugger
 * CLI entry point and DAP bootstrap.
 *
 * Owner: Person E
 *
 * Usage:
 *   ocldbg [options] <host_binary> [args...]
 *
 * Options:
 *   --dry-run          [TEMP] Test launch and NDRange inference without full execution
 *   --show-wg-bounds   Display inferred work-group bounds and coordinate mapping
 *   --backend <name>   Execution backend (default: cpu)
 *   --port <port>      Listen on TCP port for DAP client (default: stdio)
 */

#include "ocldbg/DebuggerContext.h"
#include "ocldbg/Types.h"

#include <format>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct ProgramArgs {
    std::string backend_name = "cpu";
    std::string host_binary;
    std::vector<std::string> host_args;
    uint16_t dap_port = 0;
    bool dry_run = false;
    bool show_wg_bounds = false;
    bool show_help = false;
    bool show_version = false;
};

ProgramArgs parse_arguments(int argc, char **argv) {
    ProgramArgs args;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--dry-run") {
            args.dry_run = true;
        } else if (arg == "--show-wg-bounds") {
            args.show_wg_bounds = true;
        } else if (arg == "--backend" && (i + 1 < argc)) {
            args.backend_name = argv[++i];
        } else if (arg == "--port" && (i + 1 < argc)) {
            args.dap_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "-v" || arg == "--version") {
            args.show_version = true;
        } else if (arg == "-h" || arg == "--help") {
            args.show_help = true;
        } else if (!arg.empty() && arg[0] != '-') {
            if (args.host_binary.empty()) {
                args.host_binary = arg;
            } else {
                args.host_args.push_back(arg);
            }
        }
    }
    return args;
}

void print_version() {
    std::string lldb_version = ocldbg::DebuggerContext::init();
#ifndef OCLDBG_GIT_COMMIT
#define OCLDBG_GIT_COMMIT "unknown"
#endif
#ifndef OCLDBG_VERSION
#define OCLDBG_VERSION "0.1.0"
#endif
    std::cout << std::format("ocldbg version {} (commit {})\nUsing {}\n", OCLDBG_VERSION,
                             OCLDBG_GIT_COMMIT, lldb_version);
    ocldbg::DebuggerContext::terminate();
}

void print_help() {
    std::cout << std::format(
        "Usage: ocldbg [options] <host_binary> [args...]\n"
        "Options:\n"
        "  --dry-run          [TEMP] Test launch and NDRange inference without full execution\n"
        "  --show-wg-bounds   Display inferred work-group bounds and coordinate mapping\n"
        "  --backend <name>   Execution backend (default: cpu)\n"
        "  --port <port>      Listen on TCP port for DAP client (default: stdio)\n"
        "  -v, --version      Display version and build information\n"
        "  -h, --help         Display this help message\n");
}

void print_inferred_bounds(const ocldbg::KernelLaunchInfo &info) {
    std::cout << std::format("[ocldbg] Inferred NDRange from call site:\n"
                             "  Global Size: {}\n"
                             "  Local Size:  {}\n"
                             "  Work-Groups: {}\n"
                             "[ocldbg] Work-Group Bounds:\n",
                             info.global_size, info.local_size, info.num_groups);

    for (const auto &wg : info.work_groups) {
        std::cout << std::format("  WG {}: global range [{} - {}] ({} work-items)\n", wg.group_id,
                                 wg.min_wi, wg.max_wi, wg.item_count);
    }

    std::cout << "[ocldbg] Work-Item Context Mapping:\n";
    for (const auto &wi : info.sample_work_items) {
        std::cout << std::format("  WI {} -> WG {} Local {}\n", wi.global_id, wi.group_id,
                                 wi.local_id);
    }
}

} // namespace

int main(int argc, char **argv) {
    ProgramArgs args = parse_arguments(argc, argv);

    if (args.show_version) {
        print_version();
        return 0;
    }

    if (args.show_help) {
        print_help();
        return 0;
    }

    if (args.host_binary.empty()) {
        std::cerr << "Usage: ocldbg [options] <host_binary> [args...]\n";
        return 1;
    }

    ocldbg::DebuggerContext::init();

    ocldbg::DebuggerContext dbg;
    if (!dbg.launch(args.host_binary, args.host_args)) {
        ocldbg::DebuggerContext::terminate();
        return 1;
    }

    if (dbg.infer_kernel_launch()) {
        const auto &launch_info = dbg.kernel_launch_info();
        if (args.show_wg_bounds && launch_info.has_value()) {
            print_inferred_bounds(*launch_info);
        }
    } else {
        std::cerr << "[ocldbg] Warning: Could not infer NDRange from kernel call site\n";
    }

    // NOTE: --dry-run is a temporary test/validation flag used during milestone verification.
    if (args.dry_run) {
        std::cout << "[ocldbg] Dry run completed successfully.\n";
        dbg.terminate_process();
        ocldbg::DebuggerContext::terminate();
        return 0;
    }

    int ret = dbg.run_dap(args.backend_name, args.dap_port);
    ocldbg::DebuggerContext::terminate();
    return ret;
}
