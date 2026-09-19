#include "LLDBAcceleratorSession.h"

#include "LLDBAcceleratorBackend.h"

#include <charconv>
#include <cstdint>
#include <format>
#include <iostream>
#include <optional>
#include <string_view>

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

struct MemoryRead {
    uint64_t address = 0;
    size_t length = 16;
};

/// Parses "<hex address>[:<decimal byte count>]", with an optional 0x prefix.
std::optional<MemoryRead> parse_memory_read(std::string_view spec) {
    MemoryRead read;
    const size_t colon = spec.find(':');
    std::string_view address = spec.substr(0, colon);
    address.remove_prefix(address.starts_with("0x") ? 2 : 0);
    auto parsed =
        std::from_chars(address.data(), address.data() + address.size(), read.address, 16);
    if (address.empty() || parsed.ec != std::errc() ||
        parsed.ptr != address.data() + address.size()) {
        return std::nullopt;
    }
    if (colon != std::string_view::npos) {
        const std::string_view length = spec.substr(colon + 1);
        parsed = std::from_chars(length.data(), length.data() + length.size(), read.length);
        if (parsed.ec != std::errc() || parsed.ptr != length.data() + length.size() ||
            read.length == 0) {
            return std::nullopt;
        }
    }
    return read;
}

void print_memory(LLDBAcceleratorBackend &backend, const MemoryRead &read) {
    std::vector<uint8_t> bytes(read.length);
    const size_t count = backend.read_global_memory(read.address, bytes.data(), bytes.size());
    if (count == 0) {
        std::cout << std::format("[ocldbg] Read of {} byte(s) at {:#x} failed\n", read.length,
                                 read.address);
        return;
    }
    std::string hex;
    for (size_t i = 0; i < count; ++i) {
        hex += std::format("{}{:02x}", i == 0 ? "" : " ", bytes[i]);
    }
    std::cout << std::format("[ocldbg] Read {} byte(s) at {:#x}: {}\n", count, read.address, hex);
}

} // namespace

int run_accelerator_session(const AcceleratorSessionConfig &config) {
    // LLDB writes to the same stdout, so flush each line to keep the order.
    std::cout << std::unitbuf;
    std::optional<MemoryRead> memory_read;
    if (!config.read_memory.empty()) {
        memory_read = parse_memory_read(config.read_memory);
        if (!memory_read) {
            std::cerr << "error: --read-memory expects <hex address>[:<bytes>]\n";
            return 1;
        }
    }

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
        if (memory_read) {
            print_memory(backend, *memory_read);
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
    if (memory_read) {
        print_memory(backend, *memory_read);
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
