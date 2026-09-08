#pragma once
#include "ocldbg/OCLWorkItem.h"

#include <cstdint>

namespace ocldbg {

/// Extracts the current work-item identity from a stopped host thread.
///
/// Owner: Person C
///
/// For the `loops` strategy at -O0, pocl generates a loop over local IDs.
/// When stopped inside that loop, the loop induction variable is visible
/// as a DWARF local in the LLDB frame. This class reads it.
///
/// The `global_id` of the stopped WI is then:
///   global_id = wg_offset + local_id
/// where wg_offset = wg_coord * local_size.
class WIContextExtractor {
public:
    /// Given a stopped host thread and its work-group coordinate,
    /// construct the OCLWorkItem for the current loop iteration.
    ///
    /// @param host_thread_id  OS thread ID (from WorkGroupTracker)
    /// @param wg_id           Work-group coordinate (from WorkGroupTracker)
    /// @param local_size      NDRange local work-group size
    /// @param out             Filled on success
    /// @return false if the loop induction variable cannot be read
    bool extract(uint64_t host_thread_id, const Size3 &wg_id, const Size3 &local_size,
                 OCLWorkItem &out);

    // TODO (Person C): the actual variable name for the loop induction var
    // must be discovered empirically:
    //   POCL_EXTRA_BUILD_FLAGS="-g -cl-opt-disable" \
    //   POCL_LEAVE_KERNEL_COMPILER_TEMP_FILES=1 ./host
    //   llvm-dis /tmp/pocl-*/kernel*.bc | grep -A5 "for.*gid\|local_id"
    // Then confirm LLDB can read it:
    //   (lldb) frame variable   <- inside the kernel loop body
};

} // namespace ocldbg
