#include "WIContextExtractor.h"

#include "CPUExecContext.h"

namespace ocldbg {

WIContextExtractor::WIContextExtractor() : abi_(CPUABI::create_host_abi()) {}

WIContextExtractor::WIContextExtractor(std::unique_ptr<CPUABI> abi) : abi_(std::move(abi)) {
    if (!abi_) {
        abi_ = CPUABI::create_host_abi();
    }
}

WIContextExtractor::~WIContextExtractor() = default;

// NOLINTNEXTLINE(performance-unnecessary-value-param)
bool WIContextExtractor::extract_from_frame(lldb::SBFrame frame, const Size3 &wg_id,
                                            const Size3 &local_size, OCLWorkItem &out) {
    if (!frame.IsValid() || !abi_) {
        return false;
    }

    Size3 local_id{.x = 0, .y = 0, .z = 0};
    if (!abi_->read_local_id(frame, local_size, local_id)) {
        return false;
    }

    size_t lx = (local_size.x > 0) ? local_size.x : 1;
    size_t ly = (local_size.y > 0) ? local_size.y : 1;
    size_t lz = (local_size.z > 0) ? local_size.z : 1;

    out.local_id = local_id;
    out.group_id = wg_id;
    out.global_id = {.x = (wg_id.x * lx) + local_id.x,
                     .y = (wg_id.y * ly) + local_id.y,
                     .z = (wg_id.z * lz) + local_id.z};

    auto ctx = std::make_shared<CPUExecContext>();
    ctx->host_thread_id = frame.GetThread().GetThreadID();
    ctx->frame = frame;
    ctx->thread = frame.GetThread();
    out.exec_ctx_storage = ctx;
    out.exec_ctx = ctx.get();
    return true;
}

// NOLINTNEXTLINE(performance-unnecessary-value-param)
bool WIContextExtractor::extract_from_thread(lldb::SBThread thread, const Size3 &wg_id,
                                             const Size3 &local_size, OCLWorkItem &out) {
    if (!thread.IsValid()) {
        return false;
    }
    lldb::SBFrame frame = thread.GetSelectedFrame();
    if (!frame.IsValid()) {
        frame = thread.GetFrameAtIndex(0);
    }
    return extract_from_frame(frame, wg_id, local_size, out);
}

bool WIContextExtractor::extract(uint64_t host_thread_id, const Size3 &wg_id,
                                 const Size3 &local_size, OCLWorkItem &out) {
    size_t lx = (local_size.x > 0) ? local_size.x : 1;
    size_t ly = (local_size.y > 0) ? local_size.y : 1;
    size_t lz = (local_size.z > 0) ? local_size.z : 1;

    out.local_id = {.x = 0, .y = 0, .z = 0};
    out.group_id = wg_id;
    out.global_id = {.x = wg_id.x * lx, .y = wg_id.y * ly, .z = wg_id.z * lz};

    auto ctx = std::make_shared<CPUExecContext>();
    ctx->host_thread_id = host_thread_id;
    out.exec_ctx_storage = ctx;
    out.exec_ctx = ctx.get();
    return true;
}

} // namespace ocldbg
