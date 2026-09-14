#include "OclgrindSession.h"

#include "OclgrindBackend.h"

#include <format>
#include <iostream>

namespace ocldbg {

namespace {

void print_stop(const OCLWorkItem &wi, unsigned line, size_t hit_count,
                const std::vector<VarValue> &vars) {
    std::cout << std::format("[ocldbg] Breakpoint hit at line {} for {} (hit {}):\n", line,
                             wi.str(), hit_count);
    for (const auto &v : vars) {
        std::cout << std::format("  {} = {}\n", v.name,
                                 v.available ? v.value_str : "<unavailable>");
    }
}

std::vector<VarValue> evaluate_all(OclgrindBackend &backend, const OCLWorkItem &wi,
                                   const std::vector<std::string> &exprs) {
    std::vector<VarValue> values;
    values.reserve(exprs.size());
    for (const auto &expr : exprs) {
        VarInfo info;
        info.name = expr;
        values.push_back(backend.location_backend().evaluate(info, wi.exec_ctx));
    }
    return values;
}

} // namespace

int run_oclgrind_session(const OclgrindSessionConfig &config) {
    OclgrindBackend backend;

    if (config.break_at > 0 &&
        backend.set_breakpoint(SourceLocation{.file = {}, .line = config.break_at}) == 0) {
        std::cerr << "[ocldbg] Failed to register breakpoint\n";
        return 1;
    }

    size_t hits = 0;
    backend.on_stop([&](const OCLStopContext &stop) {
        ++hits;
        print_stop(stop.stopped, backend.last_stop_line(), hits,
                   evaluate_all(backend, stop.stopped, config.print_exprs));
    });

    if (!backend.launch(config.host_binary, config.host_args)) {
        return 1;
    }

    backend.resume();
    while (backend.halted() && (config.break_for == 0 || hits < config.break_for)) {
        backend.resume();
    }

    // Detaching lets the host program run the rest of the kernel to completion.
    backend.detach();
    std::cout << std::format("[ocldbg] Oclgrind session completed with {} breakpoint hit(s).\n",
                             hits);
    return 0;
}

} // namespace ocldbg
