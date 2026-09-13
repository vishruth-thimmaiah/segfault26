#include "CustomCommands.h"
#include "DebuggerContextImpl.h"
#include "ocldbg/DebuggerContext.h"

#include <cctype>
#include <iostream>
#include <lldb/API/SBCommandInterpreter.h>
#include <lldb/API/SBCommandReturnObject.h>
#include <lldb/API/SBDebugger.h>
#include <string>
#include <string_view>

namespace ocldbg {

namespace {

void run_cli_command(DebuggerContext &dbg, lldb::SBCommandInterpreter &interp,
                     const std::string &cmd, bool echo) {
    std::string_view trimmed = cmd;
    while (!trimmed.empty() && (std::isspace(static_cast<unsigned char>(trimmed.front())) != 0)) {
        trimmed.remove_prefix(1);
    }
    if (trimmed.starts_with("ocl")) {
        if (echo) {
            std::cout << "(ocldbg) " << cmd << "\n";
        }
        handle_ocl_command(dbg, trimmed);
        return;
    }
    if (trimmed == "r" || trimmed.starts_with("run") || trimmed == "c" ||
        trimmed.starts_with("continue") || trimmed.starts_with("process launch")) {
        dbg.ensure_ocl_trampoline();
    }
    if (echo) {
        std::cout << "(ocldbg) " << cmd << "\n";
    }
    lldb::SBCommandReturnObject res;
    interp.HandleCommand(cmd.c_str(), res);
    if (const char *out = res.GetOutput(); out != nullptr) {
        std::cout << out;
    }
    if (const char *err = res.GetError(); err != nullptr) {
        std::cerr << err;
    }
}

void execute_pre_session_commands(DebuggerContext &dbg, lldb::SBCommandInterpreter &interp,
                                  const CLIConfig &config) {
    for (const auto &src : config.source_before) {
        run_cli_command(dbg, interp, "command source \"" + src + "\"", false);
    }
    for (const auto &cmd : config.one_line_before) {
        run_cli_command(dbg, interp, cmd, true);
    }
    if (!config.host_binary.empty()) {
        run_cli_command(dbg, interp, "target create \"" + config.host_binary + "\"", false);
        if (!config.host_args.empty()) {
            std::string args_cmd = "settings set target.run-args";
            for (const auto &arg : config.host_args) {
                args_cmd += " \"" + arg + "\"";
            }
            run_cli_command(dbg, interp, args_cmd, false);
        }
    }
    for (const auto &src : config.source_after) {
        run_cli_command(dbg, interp, "command source \"" + src + "\"", false);
    }
    for (const auto &cmd : config.one_line_after) {
        run_cli_command(dbg, interp, cmd, true);
    }
}

} // namespace

int DebuggerContext::run_cli(const CLIConfig &config) {
    lldb::SBCommandInterpreter interp = impl_->debugger.GetCommandInterpreter();
    execute_pre_session_commands(*this, interp, config);

    if (config.batch) {
        return 0;
    }

    std::string line;
    while (true) {
        std::cout << "(ocldbg) " << std::flush;
        if (!std::getline(std::cin, line)) {
            std::cout << "\n";
            break;
        }

        std::string_view trimmed = line;
        while (!trimmed.empty() &&
               (std::isspace(static_cast<unsigned char>(trimmed.front())) != 0)) {
            trimmed.remove_prefix(1);
        }

        if (trimmed.empty()) {
            continue;
        }

        if (trimmed.starts_with("ocl")) {
            handle_ocl_command(*this, trimmed);
            continue;
        }

        if (trimmed == "quit" || trimmed == "q" || trimmed == "exit") {
            break;
        }

        if (trimmed == "r" || trimmed.starts_with("run") || trimmed == "c" ||
            trimmed.starts_with("continue") || trimmed.starts_with("process launch")) {
            ensure_ocl_trampoline();
        }

        lldb::SBCommandReturnObject result;
        interp.HandleCommand(line.c_str(), result);
        if (const char *out = result.GetOutput(); out != nullptr) {
            std::cout << out;
        }
        if (const char *err = result.GetError(); err != nullptr) {
            std::cerr << err;
        }

        if (result.GetStatus() == lldb::eReturnStatusQuit) {
            break;
        }
    }
    return 0;
}

} // namespace ocldbg
