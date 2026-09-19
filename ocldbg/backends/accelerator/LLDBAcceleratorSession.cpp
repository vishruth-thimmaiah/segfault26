#include "LLDBAcceleratorSession.h"

#include "LLDBAcceleratorBackend.h"

#include <format>
#include <iostream>

namespace ocldbg {

namespace {

/// Prints the plugin stops not reported yet.
void print_plugin_stops(const LLDBAcceleratorBackend &backend, size_t &reported) {
    const auto &stops = backend.plugin_stops();
    for (; reported < stops.size(); ++reported) {
        std::cout << std::format("[ocldbg] Accelerator plugin stopped the host in {}\n",
                                 stops[reported]);
    }
}

void print_work_item(LLDBAcceleratorBackend &backend, const OCLWorkItem &wi,
                     const std::vector<std::string> &exprs) {
    std::cout << std::format("[ocldbg]   {}", wi.str());
    if (!wi.function.empty()) {
        std::cout << std::format(" in {}", wi.function);
    }
    if (wi.location.line > 0) {
        std::cout << std::format(" at {}:{}", wi.location.file, wi.location.line);
    }
    std::cout << "\n";
    for (const auto &expr : exprs) {
        VarInfo info;
        info.name = expr;
        const VarValue value = backend.location_backend().evaluate(info, wi.exec_ctx);
        std::cout << std::format("[ocldbg]     {} = {}\n", expr,
                                 value.available ? value.value_str : "<unavailable>");
    }
}

} // namespace

int run_accelerator_session(const AcceleratorSessionConfig &config) {
    // LLDB writes to the same stdout, so flush each line to keep the order.
    std::cout << std::unitbuf;
    LLDBAcceleratorBackend backend;
    size_t reported_stops = 0;
    size_t hits = 0;

    if (config.break_at > 0 && backend.set_breakpoint(SourceLocation{
                                   .file = config.break_file, .line = config.break_at}) == 0) {
        std::cerr << "[ocldbg] Failed to register breakpoint\n";
        return 1;
    }

    backend.on_stop([&](const OCLStopContext &stop) {
        ++hits;
        print_plugin_stops(backend, reported_stops);
        std::cout << std::format("[ocldbg] Accelerator stopped (stop {}) with {} thread(s)\n", hits,
                                 stop.visible.size());
        for (const auto &wi : stop.visible) {
            print_work_item(backend, wi, config.print_exprs);
        }
    });

    std::cout << std::format("[ocldbg] Launching {} under LLDB with an accelerator plugin\n",
                             config.host_binary);
    if (!backend.launch(config.host_binary, config.host_args)) {
        print_plugin_stops(backend, reported_stops);
        return 1;
    }

    print_plugin_stops(backend, reported_stops);
    const auto connected = backend.accelerator_work_items();
    std::cout << std::format("[ocldbg] Accelerator target {} connected with {} thread(s)\n",
                             backend.accelerator_triple(), connected.size());
    for (const auto &wi : connected) {
        print_work_item(backend, wi, config.print_exprs);
    }

    backend.resume();
    while (backend.halted() && (config.break_for == 0 || hits < config.break_for)) {
        backend.resume();
    }

    print_plugin_stops(backend, reported_stops);
    if (const auto status = backend.host_exit_status()) {
        std::cout << std::format("[ocldbg] Host exited with status {}\n", *status);
    }
    const bool stalled = backend.stalled();
    backend.detach();
    if (stalled) {
        std::cerr << std::format("[ocldbg] Accelerator session ended early after {} stop(s)\n",
                                 hits);
        return 1;
    }
    std::cout << std::format("[ocldbg] Accelerator session completed with {} stop(s).\n", hits);
    return 0;
}

} // namespace ocldbg
