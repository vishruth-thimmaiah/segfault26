#include "DAPServer.h"
#include <stdexcept>

// TODO (Person E): implement the full DAP message loop.
//
// Wire format: each DAP message is a Content-Length header + JSON body,
// identical to the Language Server Protocol:
//   Content-Length: <N>\r\n\r\n<N bytes of JSON>
//
// Recommended JSON library: nlohmann/json (header-only, Apache-2.0)
//   https://github.com/nlohmann/json
//
// Sequence:
//   1. Read Content-Length header from stdin
//   2. Read that many bytes as JSON
//   3. Dispatch to appropriate handler based on msg["command"]
//   4. Write response/event JSON to stdout with Content-Length prefix
//
// See: https://microsoft.github.io/debug-adapter-protocol/specification

namespace ocldbg {

struct DAPServer::Impl {
    // TODO: JSON parser state, socket fd if TCP mode
};

DAPServer::DAPServer(Backend &backend, DWARFSourceModel &dwarf,
                     OCLVariableResolver &resolver)
    : backend_(backend), dwarf_(dwarf), resolver_(resolver),
      impl_(std::make_unique<Impl>()) {}

DAPServer::~DAPServer() = default;

void DAPServer::run_stdio() {
    // TODO (Person E): message loop over stdin/stdout
    throw std::runtime_error("DAPServer::run_stdio not yet implemented");
}

void DAPServer::run_tcp(uint16_t /*port*/) {
    // TODO (Person E): listen on TCP port, accept one client, run message loop
    throw std::runtime_error("DAPServer::run_tcp not yet implemented");
}

void DAPServer::handle_message(const std::string & /*json_msg*/) {
    // TODO (Person E): parse JSON, dispatch on command field
}

std::string DAPServer::make_response(int /*seq*/, const std::string & /*cmd*/,
                                      const std::string & /*body*/) {
    // TODO (Person E): build DAP response JSON
    return "{}";
}

std::string DAPServer::make_event(const std::string & /*event*/,
                                   const std::string & /*body*/) {
    // TODO (Person E): build DAP event JSON
    return "{}";
}

} // namespace ocldbg
