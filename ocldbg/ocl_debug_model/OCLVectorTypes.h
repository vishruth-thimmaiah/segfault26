#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ocldbg {

/// Describes an OpenCL built-in vector type (float4, uchar16, ...). Clang
/// always spells these as `<elem><N>` (opencl-c-base.h), N in {2,3,4,8,16}.
///
/// Backend-agnostic: any backend that reads a vector-typed variable back as
/// raw bytes (from a register, from memory, from a DWARF implicit-value
/// expression, ...) can use parse_ocl_vector_type()/format_ocl_vector_bytes()
/// to decode them, rather than each backend re-implementing this. The CPU
/// backend (PoclCPUBackend.cpp) needs this; Oclgrind does not, since its
/// values are already formatted by Oclgrind's own interpreter before ocldbg
/// ever sees them, and LLDB formats the values of the accelerator backend.
struct OCLVectorType {
    size_t elem_size = 0;
    size_t count = 0; ///< logical lane count, e.g. 3 for float3
    bool is_float = false;
    bool is_signed = false;
};

/// Per the OpenCL spec (6.1.2), a 3-lane vector occupies the same storage as
/// a 4-lane one; callers must read this many lanes' worth of bytes even
/// though only `count` are displayed.
size_t ocl_vector_storage_count(const OCLVectorType &vt);

/// Recognizes an OpenCL vector type name (e.g. "float4", "uchar16"), or
/// returns nullopt for a scalar/non-vector type name.
std::optional<OCLVectorType> parse_ocl_vector_type(std::string_view type_name);

/// Formats `ocl_vector_storage_count(vt) * vt.elem_size` raw bytes (as read
/// from a register, memory, or DWARF implicit-value expression) as
/// "(v0, v1, ...)", showing only `vt.count` lanes even when more were read
/// (the float3-in-float4-storage case).
std::string format_ocl_vector_bytes(const std::vector<uint8_t> &bytes, const OCLVectorType &vt);

} // namespace ocldbg
