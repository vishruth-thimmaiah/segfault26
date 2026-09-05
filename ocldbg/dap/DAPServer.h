#pragma once
#include "ocldbg/Backend.h"
#include "ocldbg/OCLWorkItem.h"
#include <memory>
#include <string>

namespace ocldbg {

class DWARFSourceModel;
class OCLVariableResolver;

/// Debug Adapter Protocol server.
///
/// Owner: Person E
///
/// Speaks the DAP JSON wire format over stdin/stdout (VS Code default)
/// or a TCP socket. Translates DAP requests into Backend + OCLVariableResolver
/// calls, and Backend stop events into DAP responses/events.
///
/// DAP spec: https://microsoft.github.io/debug-adapter-protocol/
///
/// Key design decisions:
///   - `threads` response: returns OCLStopContext.stopped + visible, NOT the
///     full NDRange. See PLANNING.md §4 Milestone 5.
///   - Thread IDs in DAP are integers; map them to OCLWorkItem via a local table.
///
/// Custom DAP requests added for OpenCL:
///   "ocldbg/selectWorkItem": { gx, gy, gz }  -> select a different WI
class DAPServer {
public:
    DAPServer(Backend &backend,
              DWARFSourceModel &dwarf,
              OCLVariableResolver &resolver);
    ~DAPServer();

    /// Run the DAP message loop on stdin/stdout.
    /// Blocks until the debugger session ends.
    void run_stdio();

    /// Run the DAP message loop on a TCP socket.
    void run_tcp(uint16_t port);

private:
    // TODO (Person E): implement DAP message dispatch.
    // Recommended approach: use a JSON library (e.g. nlohmann/json).
    // Handle at minimum:
    //   initialize, launch, setBreakpoints, configurationDone,
    //   threads, stackTrace, scopes, variables,
    //   continue, next, stepIn,
    //   disconnect
    //
    // Custom: ocldbg/selectWorkItem

    void handle_message(const std::string &json_msg);
    std::string make_response(int seq, const std::string &command,
                              const std::string &body_json);
    std::string make_event(const std::string &event,
                           const std::string &body_json);

    Backend             &backend_;
    DWARFSourceModel    &dwarf_;
    OCLVariableResolver &resolver_;

    OCLStopContext current_stop_;
    int            next_thread_id_ = 1;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ocldbg
