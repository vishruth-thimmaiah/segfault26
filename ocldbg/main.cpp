/**
 * ocldbg — Source-Level OpenCL Debugger
 * CLI entry point and DAP bootstrap.
 *
 * Owner: Person E
 */

#include "args.h"
#include "ocldbg/DebuggerContext.h"
#include "ocldbg/Types.h"

#include <format>
#include <iostream>
#include <string>
#include <vector>

namespace {

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

void print_tracking_summary(size_t dispatches, size_t expected) {
    std::cout << std::format("[ocldbg] Work-Group Dispatch Tracking:\n"
                             "  Dispatches: {}\n"
                             "  Expected:   {}\n",
                             dispatches, expected);
}

void handle_workgroup_tracking(ocldbg::DebuggerContext &dbg, const ocldbg::ProgramArgs &args,
                               const ocldbg::KernelLaunchInfo &launch_info) {
    // Infer kernel name from the last host arg if it doesn't look like a file path.
    // host_runner.c convention: argv[1]=<cl_file> argv[2]=<kernel_name>
    std::string kernel_name;
    if (!args.host_args.empty()) {
        const std::string &last = args.host_args.back();
        if (last.size() < 3 || last.substr(last.size() - 3) != ".cl") {
            kernel_name = last;
        }
    }

    if (kernel_name.empty()) {
        std::cerr << "[ocldbg] Warning: Kernel name required as last host arg\n";
        return;
    }

    dbg.set_workgroup_breakpoint(kernel_name);
    size_t dispatches =
        dbg.track_workgroup_dispatches(args.inspect_vars, args.break_at, args.break_for);
    if (args.track_wg) {
        size_t expected =
            launch_info.num_groups.x * launch_info.num_groups.y * launch_info.num_groups.z;
        print_tracking_summary(dispatches, expected);
    }
}

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char **argv) {
    ocldbg::ProgramArgs args = ocldbg::parse_arguments(argc, argv);

    if (args.show_version) {
        ocldbg::print_version();
        return 0;
    }

    if (args.show_help) {
        ocldbg::print_help();
        return 0;
    }

    if (args.has_ocldbg_specific_args() && !args.dry_run) {
        std::cerr
            << "error: ocldbg-specific options (--track-wg, --inspect-vars, --show-wg-bounds, "
               "--break-at, --break-for) are only supported with --dry-run\n";
        return 1;
    }

    // Dry-run milestone verification pipeline
    if (args.dry_run) {
        if (args.host_binary.empty()) {
            std::cerr << "Usage: ocldbg --dry-run [options] <host_binary> [args...]\n";
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

            if ((args.track_wg || args.break_at > 0) && launch_info.has_value()) {
                handle_workgroup_tracking(dbg, args, *launch_info);
            }
        } else {
            std::cerr << "[ocldbg] Warning: Could not infer NDRange from kernel call site\n";
        }

        std::cout << "[ocldbg] Dry run completed successfully.\n";
        if (!args.track_wg && args.break_at == 0) {
            dbg.terminate_process();
        }
        ocldbg::DebuggerContext::terminate();
        return 0;
    }

    // DAP server session
    if (args.dap_port > 0) {
        ocldbg::DebuggerContext::init();
        ocldbg::DebuggerContext dbg;
        int ret = dbg.run_dap(args.backend_name, args.dap_port);
        ocldbg::DebuggerContext::terminate();
        return ret;
    }

    // LLDB-compatible CLI session (interactive or batch)
    ocldbg::DebuggerContext::init();
    ocldbg::DebuggerContext dbg;
    ocldbg::CLIConfig cli_config{
        .host_binary = args.host_binary,
        .host_args = args.host_args,
        .one_line_before = args.one_line_before,
        .source_before = args.source_before,
        .source_after = args.source_after,
        .one_line_after = args.one_line_after,
        .batch = args.batch_mode,
    };
    int ret = dbg.run_cli(cli_config);
    ocldbg::DebuggerContext::terminate();
    return ret;
}
