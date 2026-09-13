#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ocldbg {

struct ProgramArgs {
    std::string backend_name = "cpu";
    std::string host_binary;
    std::vector<std::string> host_args;
    uint16_t dap_port = 0;
    bool dry_run = false;
    bool show_wg_bounds = false;
    bool track_wg = false;
    bool inspect_vars = false;
    unsigned break_at = 0;
    size_t break_for = 0;
    bool show_help = false;
    bool show_version = false;

    // LLDB CLI flags
    std::vector<std::string> one_line_before; // -O, --one-line-before-file
    std::vector<std::string> source_before;   // -S, --source-before-file
    std::vector<std::string> source_after;    // -s, --source
    std::vector<std::string> one_line_after;  // -o, --one-line
    bool batch_mode = false;                  // -b, --batch

    [[nodiscard]] bool has_ocldbg_specific_args() const {
        return show_wg_bounds || track_wg || inspect_vars || break_at > 0 || break_for > 0;
    }
};

ProgramArgs parse_arguments(int argc, char **argv);
void print_version();
void print_help();

} // namespace ocldbg
