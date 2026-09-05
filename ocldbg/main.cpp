/**
 * ocldbg — Source-Level OpenCL Debugger
 * CLI entry point and DAP bootstrap.
 *
 * Owner: Person E
 *
 * Usage:
 *   ocldbg [--backend cpu|oclgrind] [--port <N>] <host_binary> [args...]
 *
 * Without --port: runs DAP server on stdin/stdout (VS Code default).
 * With    --port: listens on TCP for a DAP client.
 */

#include "backends/cpu/PoclCPUBackend.h"
#include "backends/oclgrind/OclgrindBackend.h"
#include "dap/DAPServer.h"
#include "dwarf/DWARFSourceModel.h"
#include "ocl_debug_model/OCLVariableResolver.h"
#include <iostream>
#include <memory>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    // TODO (Person E): parse arguments (backend selection, port, binary path)
    // For now, print usage and exit.

    std::string backend_name = "cpu";
    std::string host_binary;
    std::vector<std::string> host_args;
    uint16_t dap_port = 0; // 0 = stdin/stdout mode

    if (argc < 2) {
        std::cerr << "Usage: ocldbg [--backend cpu|oclgrind] [--port <N>]"
                     " <host_binary> [args...]\n";
        return 1;
    }

    // TODO (Person E): proper arg parsing (consider using CLI11 or hand-rolled)
    host_binary = argv[1];

    // Select backend
    std::unique_ptr<ocldbg::Backend> backend;
    if (backend_name == "oclgrind") {
        backend = std::make_unique<ocldbg::OclgrindBackend>();
    } else {
        backend = std::make_unique<ocldbg::PoclCPUBackend>();
    }

    // Shared DWARF model and resolver
    ocldbg::DWARFSourceModel  dwarf;
    ocldbg::OCLVariableResolver resolver(dwarf);

    // DAP server
    ocldbg::DAPServer dap(*backend, dwarf, resolver);

    if (dap_port > 0) {
        dap.run_tcp(dap_port);
    } else {
        dap.run_stdio();
    }

    return 0;
}
