#pragma once
#include "backends/oclgrind/OclgrindProtocol.h"

#include <cstddef>
#include <oclgrind/Plugin.h>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace ocldbg {

/// Halts the Oclgrind interpreter on behalf of ocldbg.
///
/// Oclgrind dlopens this library when OCLGRIND_PLUGINS names it and calls
/// initializePlugins() below. Blocking inside instructionExecuted() is what
/// freezes the interpreter while the debugger reads work-item state.
///
/// Breakpoints match on source line alone: an Oclgrind Program is compiled from
/// a single source string.
class DebugPlugin final : public oclgrind::Plugin {
public:
    explicit DebugPlugin(const oclgrind::Context *context);

    void kernelBegin(const oclgrind::KernelInvocation *invocation) override;
    void kernelEnd(const oclgrind::KernelInvocation *invocation) override;
    void instructionExecuted(const oclgrind::WorkItem *work_item,
                             const llvm::Instruction *instruction,
                             const oclgrind::TypedValue &result) override;

    /// False makes Oclgrind drop to one worker thread. Halting a work-item only
    /// means anything if no other runs concurrently, and single-worker
    /// scheduling is also what makes the order reproducible.
    bool isThreadSafe() const override { return false; }

private:
    enum class RunMode { kRunning, kStepIn, kStepOver };

    /// Serve debugger commands until told to resume.
    void serve();

    /// Returns false once the channel is gone.
    bool handle_command(const std::string &command, bool &resume);
    bool handle_execution(const std::vector<std::string> &words, bool &resume);
    bool handle_breakpoint(const std::vector<std::string> &words);
    bool handle_query(const std::vector<std::string> &words);

    /// The halted work-item, or whichever one a preceding `select` switched to.
    [[nodiscard]] const oclgrind::WorkItem *active_work_item() const;

    [[nodiscard]] bool should_halt(const oclgrind::WorkItem *work_item, size_t line) const;

    void report_stop(const oclgrind::WorkItem *work_item, size_t line);

    std::string read_expression(const oclgrind::WorkItem *work_item, const std::string &expr) const;
    std::string read_memory(size_t address, size_t length) const;
    bool select_work_item(const std::vector<size_t> &global_id);

    oclgrind_proto::LineChannel channel_;
    const oclgrind::KernelInvocation *invocation_ = nullptr;

    std::set<size_t> breakpoint_lines_;
    std::unordered_map<size_t, size_t> breakpoint_ids_; ///< id -> line

    RunMode mode_ = RunMode::kRunning;
    size_t stepping_index_ = 0;                    ///< global index of the work-item being stepped
    size_t stepping_depth_ = 0;                    ///< its call depth when the step started
    std::unordered_map<size_t, size_t> last_line_; ///< global index -> last halted line
};

} // namespace ocldbg

extern "C" {
/// Entry points Oclgrind looks up with dlsym() after loading this library.
void initializePlugins(oclgrind::Context *context);
void releasePlugins(oclgrind::Context *context);
}
