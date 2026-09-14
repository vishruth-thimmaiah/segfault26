#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>

/// Newline-terminated protocol between ocldbg and the plugin injected into the
/// host program. Exchanges are synchronous and always begin with the plugin
/// halting the interpreter:
///
///   plugin -> ocldbg   ready
///                      stopped <line> <gx> <gy> <gz> <lx> <ly> <lz> <bx> <by> <bz>
///   ocldbg -> plugin   break <id> <line>       -> ok
///                      delete <id>             -> ok
///                      select <gx> <gy> <gz>   -> ok <lx> <ly> <lz> <bx> <by> <bz>
///                      print <expr>            -> value <text> | err <reason>
///                      read <addr> <len>       -> data <hex> | err <reason>
///                      continue                -> (no reply; plugin resumes)
///                      step in | step over     -> (no reply; plugin resumes)
///
/// Breakpoint ids come from the debugger, so neither side has to reconcile a
/// second id space. After a resume the next line is a stopped event, or
/// end-of-stream once the host program exits.
namespace ocldbg::oclgrind_proto {

inline constexpr const char *kSocketEnvVar = "OCLDBG_OCLGRIND_SOCKET";

inline constexpr std::string_view kEventReady = "ready";
inline constexpr std::string_view kEventStopped = "stopped";
inline constexpr std::string_view kReplyOk = "ok";
inline constexpr std::string_view kReplyErr = "err";
inline constexpr std::string_view kReplyValue = "value";
inline constexpr std::string_view kReplyData = "data";

inline constexpr std::string_view kCmdBreak = "break";
inline constexpr std::string_view kCmdDelete = "delete";
inline constexpr std::string_view kCmdSelect = "select";
inline constexpr std::string_view kCmdPrint = "print";
inline constexpr std::string_view kCmdRead = "read";
inline constexpr std::string_view kCmdContinue = "continue";
inline constexpr std::string_view kCmdStep = "step";
inline constexpr std::string_view kStepIn = "in";
inline constexpr std::string_view kStepOver = "over";

/// Blocking recv() on this channel is what suspends the interpreter thread
/// while the debugger inspects a halted work-item.
class LineChannel {
public:
    LineChannel() = default;
    explicit LineChannel(int fd) : fd_(fd) {}

    ~LineChannel() { close(); }

    LineChannel(const LineChannel &) = delete;
    LineChannel &operator=(const LineChannel &) = delete;

    LineChannel(LineChannel &&other) noexcept
        : fd_(std::exchange(other.fd_, -1)), pending_(std::move(other.pending_)) {}

    LineChannel &operator=(LineChannel &&other) noexcept {
        if (this != &other) {
            close();
            fd_ = std::exchange(other.fd_, -1);
            pending_ = std::move(other.pending_);
        }
        return *this;
    }

    [[nodiscard]] bool valid() const { return fd_ >= 0; }

    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        pending_.clear();
    }

    bool send(std::string_view line) {
        if (fd_ < 0) {
            return false;
        }
        std::string framed(line);
        framed += '\n';
        size_t sent = 0;
        while (sent < framed.size()) {
            ssize_t n = ::write(fd_, framed.data() + sent, framed.size() - sent);
            if (n <= 0) {
                return false;
            }
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    /// Returns false at end-of-stream, which is how each side learns the peer
    /// has gone away.
    bool recv(std::string &line) {
        if (fd_ < 0) {
            return false;
        }
        for (;;) {
            size_t nl = pending_.find('\n');
            if (nl != std::string::npos) {
                line.assign(pending_, 0, nl);
                pending_.erase(0, nl + 1);
                return true;
            }

            std::string chunk(4096, '\0');
            ssize_t n = ::read(fd_, chunk.data(), chunk.size());
            if (n <= 0) {
                return false;
            }
            pending_.append(chunk, 0, static_cast<size_t>(n));
        }
    }

private:
    int fd_ = -1;
    std::string pending_;
};

} // namespace ocldbg::oclgrind_proto
