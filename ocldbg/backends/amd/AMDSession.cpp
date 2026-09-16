#include "AMDSession.h"

#include "AMDBackend.h"

#include <format>
#include <iostream>

namespace ocldbg {

namespace {

// AMD_DBGAPI_WAVE_STOP_REASON_* bit values (amd-dbgapi.h); decoded here
// rather than included as a dependency of the (SDK-free) AMDSession.h.
std::string describe_stop_reason(uint32_t reason) {
    if (reason == 0) {
        return "requested (ocldbg halted a running wave)";
    }
    std::string out;
    auto add = [&](uint32_t bit, const char *name) {
        if ((reason & bit) != 0) {
            if (!out.empty()) {
                out += " | ";
            }
            out += name;
        }
    };
    add(1U << 0, "BREAKPOINT");
    add(1U << 1, "WATCHPOINT");
    add(1U << 2, "SINGLE_STEP");
    add(1U << 13, "MEMORY_VIOLATION");
    add(1U << 14, "ADDRESS_ERROR");
    add(1U << 15, "ILLEGAL_INSTRUCTION");
    add(1U << 16, "ECC_ERROR");
    add(1U << 17, "FATAL_HALT");
    return out.empty() ? std::format("unrecognized (0x{:x})", reason) : out;
}

} // namespace

int run_amd_session(const AMDSessionConfig &config) {
    AMDBackend backend;

    bool got_stop = false;
    OCLWorkItem stopped_wi;
    backend.on_stop([&](const OCLStopContext &stop) {
        got_stop = true;
        stopped_wi = stop.stopped;
    });

    if (!backend.launch(config.host_binary, config.host_args)) {
        std::cerr << "[ocldbg] Failed to launch/attach on the AMD backend.\n";
        return 1;
    }

    backend.resume();

    if (!got_stop) {
        std::cout << "[ocldbg] AMD session: no wave was observed halted (process likely ran to "
                     "completion before/without a dispatch, or attach failed silently).\n";
        backend.detach();
        return 1;
    }

    std::cout << std::format("[ocldbg] AMD session: halted a wave in work-group {}\n",
                             stopped_wi.group_id.str());
    std::cout << std::format("[ocldbg] Stop reason: {}\n",
                             describe_stop_reason(backend.last_stop_reason()));

    backend.detach();
    std::cout << "[ocldbg] AMD session completed.\n";
    return 0;
}

} // namespace ocldbg
