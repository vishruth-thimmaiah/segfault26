#include "WIContextExtractor.h"

// TODO (Person C): implement extract().
// Read the loop induction variable from the LLDB SBFrame of host_thread_id.
// Validate the variable name by inspecting pocl's generated IR first.

namespace ocldbg {

bool WIContextExtractor::extract(uint64_t /*host_thread_id*/,
                                  const Size3 & /*wg_id*/,
                                  const Size3 & /*local_size*/,
                                  OCLWorkItem & /*out*/) {
    // TODO (Person C)
    return false;
}

} // namespace ocldbg
