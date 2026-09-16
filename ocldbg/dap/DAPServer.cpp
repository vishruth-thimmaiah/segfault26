#include "DAPServer.h"

#include "backends/cpu/CPUExecContext.h"
#include "dwarf/DWARFSourceModel.h"
#include "ocl_debug_model/OCLVariableResolver.h"

#include <arpa/inet.h>
#include <array>
#include <cctype>
#include <iostream>
#include <lldb/API/SBFileSpec.h>
#include <lldb/API/SBLineEntry.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/raw_ostream.h>
#include <map>
#include <netinet/in.h>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace ocldbg {

namespace {

void send_raw_message(const std::string &json_payload, std::ostream &out) {
    out << "Content-Length: " << json_payload.size() << "\r\n\r\n" << json_payload;
    out.flush();
}

} // namespace

static void apply_environment(const llvm::json::Object *args) {
    if (args == nullptr) {
        return;
    }
    if (const auto *env_arr = args->getArray("environment")) {
        for (const auto &entry : *env_arr) {
            if (const auto *obj = entry.getAsObject()) {
                auto n = obj->getString("name");
                auto v = obj->getString("value");
                if (n && v) {
                    setenv(n->str().c_str(), v->str().c_str(), 1);
                }
            }
        }
    }
    if (const auto *env_obj = args->getObject("env")) {
        for (const auto &[k, v] : *env_obj) {
            if (auto s = v.getAsString()) {
                setenv(k.str().c_str(), s->str().c_str(), 1);
            }
        }
    }
    if (auto cwd = args->getString("cwd")) {
        (void)chdir(cwd->str().c_str());
    }
}

static std::string extract_source_path(const llvm::json::Object *args) {
    if (args == nullptr) {
        return "";
    }
    const auto *src = args->getObject("source");
    if (src == nullptr) {
        return "";
    }
    if (auto p = src->getString("path")) {
        return p->str();
    }
    if (auto n = src->getString("name")) {
        return n->str();
    }
    return "";
}

static llvm::json::Object create_bp_response(Backend &backend, const std::string &file_path,
                                             const llvm::json::Value &bp_val) {
    const auto *bp_obj = bp_val.getAsObject();
    if (bp_obj == nullptr) {
        return {};
    }
    int64_t line = bp_obj->getInteger("line").value_or(0);
    uint64_t bp_id = 0;
    if (line > 0) {
        bp_id = backend.set_breakpoint(
            SourceLocation{.file = file_path, .line = static_cast<unsigned>(line)});
    }
    llvm::json::Object bp_resp;
    bp_resp["id"] = static_cast<int64_t>(bp_id);
    bp_resp["verified"] = (bp_id > 0);
    bp_resp["line"] = line;
    return bp_resp;
}

static void vars_from_lldb_frame(lldb::SBFrame &frame, llvm::json::Array &vars_arr) {
    lldb::SBValueList val_list = frame.GetVariables(true, true, true, false);
    uint32_t num_vals = val_list.GetSize();
    for (uint32_t i = 0; i < num_vals; ++i) {
        lldb::SBValue val = val_list.GetValueAtIndex(i);
        if (!val.IsValid()) {
            continue;
        }
        const char *name = val.GetName();
        if (name == nullptr) {
            continue;
        }
        const char *v_str = val.GetValue();
        if (v_str == nullptr) {
            v_str = val.GetSummary();
        }
        const char *t_str = val.GetTypeName();
        llvm::json::Object var_obj;
        var_obj["name"] = name;
        var_obj["value"] = (v_str != nullptr) ? v_str : "<unavailable>";
        var_obj["type"] = (t_str != nullptr) ? t_str : "";
        var_obj["variablesReference"] = 0;
        vars_arr.push_back(std::move(var_obj));
    }
}

class DAPServer::Impl {
    friend class DAPServer;

public:
    Impl(DAPServer &s, Backend &b, DWARFSourceModel &d, OCLVariableResolver &r)
        : server(s), backend(b), dwarf(d), resolver(r) {}

    void send_response(int req_seq, const std::string &cmd, const std::string &body_json) {
        if (out_stream != nullptr) {
            std::string raw = server.make_response(req_seq, cmd, body_json);
            send_raw_message(raw, *out_stream);
        }
    }

    void send_event(const std::string &event, const std::string &body_json) {
        if (out_stream != nullptr) {
            std::string raw = server.make_event(event, body_json);
            send_raw_message(raw, *out_stream);
        }
    }

    void handle_initialize(int req_seq) {
        llvm::json::Object caps;
        caps["supportsConfigurationDoneRequest"] = true;
        caps["supportsFunctionBreakpoints"] = false;
        caps["supportsConditionalBreakpoints"] = false;
        caps["supportsEvaluateForHovers"] = true;
        caps["supportsStepBack"] = false;
        caps["supportsSetVariable"] = false;

        std::string body_str;
        llvm::raw_string_ostream os(body_str);
        os << llvm::json::Value(std::move(caps));

        send_response(req_seq, "initialize", body_str);
        send_event("initialized", "{}");
    }

    void handle_launch(int req_seq, const llvm::json::Object *args) {
        std::string program;
        std::vector<std::string> host_args;
        if (args != nullptr) {
            if (auto p = args->getString("program")) {
                program = p->str();
            }
            if (const auto *a = args->getArray("args")) {
                for (const auto &val : *a) {
                    if (auto s = val.getAsString()) {
                        host_args.push_back(s->str());
                    }
                }
            }
            if (auto k = args->getString("kernel")) {
                launch_kernel_file = k->str();
            }
            if (launch_kernel_file.empty()) {
                for (const auto &arg : host_args) {
                    if (arg.ends_with(".cl")) {
                        launch_kernel_file = arg;
                        break;
                    }
                }
            }
            if (!launch_kernel_file.empty()) {
                current_source_file = launch_kernel_file;
                user_source_file = launch_kernel_file;
            }
            apply_environment(args);
        }
        backend.launch(program, host_args);
        send_response(req_seq, "launch", "{}");
    }

    void handle_set_breakpoints(int req_seq, const llvm::json::Object *args) {
        std::string file_path = extract_source_path(args);
        if (!file_path.empty()) {
            current_source_file = file_path;
            if (user_source_file.empty() || launch_kernel_file.empty()) {
                user_source_file = file_path;
            }
        }

        llvm::json::Array bp_resps;
        if (args != nullptr) {
            if (const auto *bps = args->getArray("breakpoints")) {
                for (const auto &bp_val : *bps) {
                    bp_resps.push_back(create_bp_response(backend, file_path, bp_val));
                }
            }
        }

        llvm::json::Object body;
        body["breakpoints"] = std::move(bp_resps);

        std::string body_str;
        llvm::raw_string_ostream os(body_str);
        os << llvm::json::Value(std::move(body));

        send_response(req_seq, "setBreakpoints", body_str);
    }

    void handle_configuration_done(int req_seq) {
        send_response(req_seq, "configurationDone", "{}");
        backend.resume();
    }

    void handle_threads(int req_seq) {
        llvm::json::Array threads_arr;
        for (const auto &[tid, wi] : threads) {
            llvm::json::Object t;
            t["id"] = tid;
            std::string name = wi.str();
            if (wi.exec_ctx != nullptr) {
                const auto *ctx = static_cast<const CPUExecContext *>(wi.exec_ctx);
                if (ctx->frame.IsValid()) {
                    if (const char *fn = ctx->frame.GetFunctionName()) {
                        if (std::string_view(fn).find("workgroup") == std::string_view::npos) {
                            name = std::string("Host Thread #") + std::to_string(tid) + " (" + fn +
                                   ")";
                        }
                    }
                }
            }
            t["name"] = name;
            threads_arr.push_back(std::move(t));
        }
        llvm::json::Object body;
        body["threads"] = std::move(threads_arr);

        std::string body_str;
        llvm::raw_string_ostream os(body_str);
        os << llvm::json::Value(std::move(body));

        send_response(req_seq, "threads", body_str);
    }

    struct StackFrameInfo {
        std::string file;
        std::string path;
        unsigned line = 1;
        std::string fn_name = "kernel";
    };

    static StackFrameInfo extract_frame_info(const OCLWorkItem &target_wi,
                                             const std::string &default_file,
                                             unsigned default_line) {
        StackFrameInfo info;
        info.file = default_file.empty() ? "kernel.cl" : default_file;
        info.path = info.file;
        info.line = default_line > 0 ? default_line : 1;

        if (target_wi.exec_ctx != nullptr) {
            const auto *ctx = static_cast<const CPUExecContext *>(target_wi.exec_ctx);
            if (ctx->frame.IsValid()) {
                lldb::SBLineEntry le = ctx->frame.GetLineEntry();
                if (le.IsValid()) {
                    info.line = le.GetLine();
                    if (const char *f = le.GetFileSpec().GetFilename()) {
                        info.file = f;
                    }
                    std::array<char, 1024> pbuf{};
                    if (le.GetFileSpec().GetPath(pbuf.data(), pbuf.size()) > 0) {
                        info.path = pbuf.data();
                    }
                }
                if (const char *fn = ctx->frame.GetFunctionName()) {
                    info.fn_name = fn;
                }
            }
        }
        return info;
    }

    static void resolve_temp_source(StackFrameInfo &info, const std::string &preferred_path) {
        if (!info.file.starts_with("tempfile_") || !info.file.ends_with(".cl") ||
            preferred_path.empty()) {
            return;
        }
        info.path = preferred_path;
        auto pos = preferred_path.find_last_of("/\\");
        info.file = (pos != std::string::npos) ? preferred_path.substr(pos + 1) : preferred_path;
    }

    void handle_stack_trace(const OCLStopContext &current_stop, int req_seq,
                            const llvm::json::Object *args) {
        int tid = (args != nullptr)
                      ? static_cast<int>(args->getInteger("threadId").value_or(stopped_thread_id))
                      : stopped_thread_id;
        OCLWorkItem target_wi = current_stop.stopped;
        auto it = threads.find(tid);
        if (it != threads.end()) {
            target_wi = it->second;
        }

        StackFrameInfo frame_info =
            extract_frame_info(target_wi, current_source_file, current_line);
        std::string fallback_path =
            !launch_kernel_file.empty() ? launch_kernel_file : user_source_file;
        resolve_temp_source(frame_info, fallback_path);

        llvm::json::Object frame;
        frame["id"] = static_cast<int64_t>((tid * 10) + 1);
        frame["name"] = frame_info.fn_name;
        frame["line"] = static_cast<int64_t>(frame_info.line);
        frame["column"] = 1;

        llvm::json::Object src;
        src["name"] = frame_info.file;
        src["path"] = frame_info.path;
        frame["source"] = std::move(src);

        llvm::json::Array frames;
        frames.push_back(std::move(frame));

        llvm::json::Object body;
        body["stackFrames"] = std::move(frames);
        body["totalFrames"] = 1;

        std::string body_str;
        llvm::raw_string_ostream os(body_str);
        os << llvm::json::Value(std::move(body));

        send_response(req_seq, "stackTrace", body_str);
    }

    void handle_scopes(int req_seq, const llvm::json::Object *args) {
        int64_t frame_id = (args != nullptr) ? args->getInteger("frameId").value_or(1) : 1;
        int64_t base_ref = frame_id * 100;

        llvm::json::Object locals_scope;
        locals_scope["name"] = "__local";
        locals_scope["variablesReference"] = base_ref;
        locals_scope["expensive"] = false;

        llvm::json::Object globals_scope;
        globals_scope["name"] = "__global";
        globals_scope["variablesReference"] = base_ref + 1;
        globals_scope["expensive"] = false;

        llvm::json::Array scopes_arr;
        scopes_arr.push_back(std::move(locals_scope));
        scopes_arr.push_back(std::move(globals_scope));

        llvm::json::Object body;
        body["scopes"] = std::move(scopes_arr);

        std::string body_str;
        llvm::raw_string_ostream os(body_str);
        os << llvm::json::Value(std::move(body));

        send_response(req_seq, "scopes", body_str);
    }

    void handle_variables(const OCLStopContext &current_stop, int req_seq,
                          const llvm::json::Object *args) {
        int64_t var_ref =
            (args != nullptr) ? args->getInteger("variablesReference").value_or(0) : 0;
        llvm::json::Array vars_arr;

        if (var_ref > 0) {
            bool is_globals = (var_ref % 100) == 1;
            int64_t base_ref = is_globals ? (var_ref - 1) : var_ref;
            int tid = static_cast<int>((base_ref / 100) / 10);
            if (tid == 0) {
                tid = stopped_thread_id;
            }
            OCLWorkItem target_wi = current_stop.stopped;
            auto it = threads.find(tid);
            if (it != threads.end()) {
                target_wi = it->second;
            } else if (selected_wi.has_value()) {
                target_wi = *selected_wi;
            }
            std::vector<VarValue> resolved = resolver.resolve(target_wi, backend);
            for (const auto &v : resolved) {
                bool is_global_var =
                    (v.address_space == "__global" || v.address_space == "__constant");
                if (is_globals != is_global_var) {
                    continue;
                }
                llvm::json::Object var_obj;
                var_obj["name"] = v.name;
                var_obj["value"] = v.available ? v.value_str : "<unavailable>";
                std::string type_str = v.type_name;
                if (!v.address_space.empty()) {
                    type_str += " " + v.address_space;
                }
                var_obj["type"] = type_str;
                var_obj["variablesReference"] = 0;
                vars_arr.push_back(std::move(var_obj));
            }

            if (vars_arr.empty() && !is_globals && target_wi.exec_ctx != nullptr) {
                const auto *ctx = static_cast<const CPUExecContext *>(target_wi.exec_ctx);
                if (ctx->frame.IsValid()) {
                    lldb::SBFrame frame = ctx->frame;
                    vars_from_lldb_frame(frame, vars_arr);
                }
            }
        }

        llvm::json::Object body;
        body["variables"] = std::move(vars_arr);

        std::string body_str;
        llvm::raw_string_ostream os(body_str);
        os << llvm::json::Value(std::move(body));

        send_response(req_seq, "variables", body_str);
    }

    void handle_select_work_item(int req_seq, const llvm::json::Object *args) {
        int64_t gx = 0;
        int64_t gy = 0;
        int64_t gz = 0;
        if (args != nullptr) {
            if (auto x = args->getInteger("gx")) {
                gx = *x;
            }
            if (auto y = args->getInteger("gy")) {
                gy = *y;
            }
            if (auto z = args->getInteger("gz")) {
                gz = *z;
            }
        }
        OCLWorkItem out_wi;
        bool ok = backend.select_work_item(Size3{.x = static_cast<size_t>(gx),
                                                 .y = static_cast<size_t>(gy),
                                                 .z = static_cast<size_t>(gz)},
                                           out_wi);
        if (ok) {
            selected_wi = out_wi;
            llvm::json::Object body;
            body["selected"] = true;
            body["workItem"] = out_wi.str();

            std::string body_str;
            llvm::raw_string_ostream os(body_str);
            os << llvm::json::Value(std::move(body));
            send_response(req_seq, "ocldbg/selectWorkItem", body_str);
        } else {
            send_response(req_seq, "ocldbg/selectWorkItem", "{\"selected\":false}");
        }
    }

    void handle_step(const OCLStopContext &current_stop, int req_seq, const std::string &cmd,
                     const llvm::json::Object *args) {
        int tid = (args != nullptr)
                      ? static_cast<int>(args->getInteger("threadId").value_or(stopped_thread_id))
                      : stopped_thread_id;
        OCLWorkItem target_wi = current_stop.stopped;
        auto it = threads.find(tid);
        if (it != threads.end()) {
            target_wi = it->second;
        }

        send_response(req_seq, cmd, "{}");
        if (cmd == "next") {
            backend.step_over(target_wi);
        } else if (cmd == "stepIn") {
            backend.step_in(target_wi);
        }
    }

    void handle_continue(int req_seq) {
        send_response(req_seq, "continue", "{\"allThreadsContinued\":true}");
        backend.resume();
    }

    void handle_disconnect(int req_seq) {
        send_response(req_seq, "disconnect", "{}");
        backend.detach();
        running = false;
    }

private:
    DAPServer &server;
    Backend &backend;
    DWARFSourceModel &dwarf;
    OCLVariableResolver &resolver;

    std::ostream *out_stream = nullptr;
    int next_seq = 1;
    bool running = true;
    std::map<int, OCLWorkItem> threads;
    int stopped_thread_id = 1;
    std::optional<OCLWorkItem> selected_wi;
    std::string current_source_file;
    std::string user_source_file;
    std::string launch_kernel_file;
    unsigned current_line = 0;
};

DAPServer::DAPServer(Backend &backend, DWARFSourceModel &dwarf, OCLVariableResolver &resolver)
    : backend_(backend), dwarf_(dwarf), resolver_(resolver),
      impl_(std::make_unique<Impl>(*this, backend, dwarf, resolver)) {
    backend_.on_stop([this](OCLStopContext stop_ctx) {
        current_stop_ = std::move(stop_ctx);
        impl_->threads.clear();
        next_thread_id_ = 1;
        int stopped_id = next_thread_id_++;
        impl_->threads[stopped_id] = current_stop_.stopped;
        for (const auto &wi : current_stop_.visible) {
            impl_->threads[next_thread_id_++] = wi;
        }
        impl_->stopped_thread_id = stopped_id;
        impl_->selected_wi.reset();

        if (current_stop_.stopped.exec_ctx != nullptr) {
            const auto *ctx = static_cast<const CPUExecContext *>(current_stop_.stopped.exec_ctx);
            if (ctx->frame.IsValid()) {
                lldb::SBLineEntry le = ctx->frame.GetLineEntry();
                if (le.IsValid()) {
                    impl_->current_line = le.GetLine();
                    if (const char *f = le.GetFileSpec().GetFilename()) {
                        impl_->current_source_file = f;
                    }
                }
            }
        }

        llvm::json::Object body;
        body["reason"] = "breakpoint";
        body["threadId"] = stopped_id;
        body["allThreadsStopped"] = true;

        std::string body_str;
        llvm::raw_string_ostream os(body_str);
        os << llvm::json::Value(std::move(body));
        impl_->send_event("stopped", body_str);
    });
}

DAPServer::~DAPServer() = default;

static std::optional<size_t> read_dap_header(std::istream &in) {
    std::string line;
    bool got_header = false;
    size_t content_length = 0;

    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        if (line.empty()) {
            if (got_header) {
                return content_length;
            }
            continue;
        }
        if (line.starts_with("Content-Length:")) {
            got_header = true;
            std::string len_str = line.substr(15);
            while (!len_str.empty() &&
                   (std::isspace(static_cast<unsigned char>(len_str.front())) != 0)) {
                len_str.erase(len_str.begin());
            }
            try {
                content_length = std::stoull(len_str);
            } catch (...) {
                content_length = 0;
            }
        }
    }
    return std::nullopt;
}

void DAPServer::run_stdio() {
    impl_->out_stream = &std::cout;
    impl_->running = true;

    while (impl_->running && std::cin.good()) {
        auto content_length = read_dap_header(std::cin);
        if (!content_length.has_value() || *content_length == 0 || std::cin.eof()) {
            break;
        }

        std::string json_msg(*content_length, '\0');
        std::cin.read(json_msg.data(), static_cast<std::streamsize>(*content_length));
        if (std::cmp_not_equal(std::cin.gcount(), *content_length)) {
            break;
        }

        handle_message(json_msg);
    }
}

void DAPServer::run_tcp(uint16_t port) {
    int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        return;
    }
    int opt = 1;
    ::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (::bind(server_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
        ::listen(server_fd, 1) != 0) {
        ::close(server_fd);
        return;
    }

    int client_fd = ::accept(server_fd, nullptr, nullptr);
    ::close(server_fd);
    if (client_fd < 0) {
        return;
    }

    int saved_stdin = ::dup(STDIN_FILENO);
    int saved_stdout = ::dup(STDOUT_FILENO);
    ::dup2(client_fd, STDIN_FILENO);
    ::dup2(client_fd, STDOUT_FILENO);
    ::close(client_fd);

    run_stdio();

    if (saved_stdin >= 0) {
        ::dup2(saved_stdin, STDIN_FILENO);
        ::close(saved_stdin);
    }
    if (saved_stdout >= 0) {
        ::dup2(saved_stdout, STDOUT_FILENO);
        ::close(saved_stdout);
    }
}

void DAPServer::handle_message(const std::string &json_msg) {
    auto parsed = llvm::json::parse(json_msg);
    if (!parsed) {
        return;
    }
    const auto *obj = parsed->getAsObject();
    if (obj == nullptr) {
        return;
    }
    auto type = obj->getString("type");
    if (!type || *type != "request") {
        return;
    }
    auto cmd = obj->getString("command");
    if (!cmd) {
        return;
    }
    int64_t req_seq = obj->getInteger("seq").value_or(0);
    const auto *args = obj->getObject("arguments");
    std::string command = cmd->str();

    if (command == "initialize") {
        impl_->handle_initialize(static_cast<int>(req_seq));
    } else if (command == "launch") {
        impl_->handle_launch(static_cast<int>(req_seq), args);
    } else if (command == "setBreakpoints") {
        impl_->handle_set_breakpoints(static_cast<int>(req_seq), args);
    } else if (command == "setExceptionBreakpoints") {
        impl_->send_response(static_cast<int>(req_seq), "setExceptionBreakpoints", "{}");
    } else if (command == "configurationDone") {
        impl_->handle_configuration_done(static_cast<int>(req_seq));
    } else if (command == "threads") {
        impl_->handle_threads(static_cast<int>(req_seq));
    } else if (command == "stackTrace") {
        impl_->handle_stack_trace(current_stop_, static_cast<int>(req_seq), args);
    } else if (command == "scopes") {
        impl_->handle_scopes(static_cast<int>(req_seq), args);
    } else if (command == "variables") {
        impl_->handle_variables(current_stop_, static_cast<int>(req_seq), args);
    } else if (command == "continue") {
        impl_->handle_continue(static_cast<int>(req_seq));
    } else if (command == "next" || command == "stepIn") {
        impl_->handle_step(current_stop_, static_cast<int>(req_seq), command, args);
    } else if (command == "ocldbg/selectWorkItem") {
        impl_->handle_select_work_item(static_cast<int>(req_seq), args);
    } else if (command == "disconnect") {
        impl_->handle_disconnect(static_cast<int>(req_seq));
    } else {
        impl_->send_response(static_cast<int>(req_seq), command, "{}");
    }
}

std::string DAPServer::make_response(int seq, const std::string &command,
                                     const std::string &body_json) {
    llvm::json::Object resp;
    resp["seq"] = impl_->next_seq++;
    resp["type"] = "response";
    resp["request_seq"] = seq;
    resp["command"] = command;
    resp["success"] = true;
    if (!body_json.empty() && body_json != "{}") {
        auto parsed = llvm::json::parse(body_json);
        if (parsed) {
            resp["body"] = *parsed;
        } else {
            resp["body"] = llvm::json::Object{};
        }
    } else {
        resp["body"] = llvm::json::Object{};
    }
    std::string out;
    llvm::raw_string_ostream os(out);
    os << llvm::json::Value(std::move(resp));
    return out;
}

std::string DAPServer::make_event(const std::string &event, const std::string &body_json) {
    llvm::json::Object evt;
    evt["seq"] = impl_->next_seq++;
    evt["type"] = "event";
    evt["event"] = event;
    if (!body_json.empty() && body_json != "{}") {
        auto parsed = llvm::json::parse(body_json);
        if (parsed) {
            evt["body"] = *parsed;
        } else {
            evt["body"] = llvm::json::Object{};
        }
    } else {
        evt["body"] = llvm::json::Object{};
    }
    std::string out;
    llvm::raw_string_ostream os(out);
    os << llvm::json::Value(std::move(evt));
    return out;
}

} // namespace ocldbg
