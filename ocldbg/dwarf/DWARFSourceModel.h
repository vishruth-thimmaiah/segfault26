#pragma once
#include "ocldbg/LocationBackend.h"
#include "ocldbg/Types.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ocldbg {

/// Source-level debug information extracted from DWARF.
/// This is the TARGET-INDEPENDENT half of variable resolution.
///
/// Owner: Person B
///
/// What this does NOT do:
///   - Read CPU registers (that's CPULocationBackend)
///   - Read GPU registers (that's AMDLocationBackend, etc.)
///
/// This reads only the DWARF section of the binary to answer questions like:
///   "which variables are in scope at PC 0x12345?"
///   "what is the type of variable 'acc'?"
///   "which source line corresponds to PC 0x12345?"
class DWARFSourceModel {
public:
    DWARFSourceModel();
    ~DWARFSourceModel();

    /// Load DWARF from an ELF object or binary.
    /// @return true if DWARF was found and parsed successfully.
    bool load(const std::string &binary_path);

    /// Map a host PC to a source location (file + line).
    /// Returns a location with line==0 if the PC has no debug info.
    SourceLocation pc_to_source(HostAddress pc) const;

    /// Map a source location to the set of PCs that correspond to it.
    /// A single source line can map to multiple addresses (loop bodies,
    /// inlined code, etc.).
    std::vector<HostAddress> source_to_pcs(const SourceLocation &loc) const;

    /// Return all variables in scope at the given PC.
    /// Does NOT evaluate their values — only returns VarInfo descriptors
    /// with DWARF location expressions for the LocationBackend to consume.
    std::vector<VarInfo> variables_in_scope(HostAddress pc) const;

    /// Look up the type name for a DWARF type offset.
    std::string type_name(uint64_t dwarf_type_offset) const;

    bool loaded() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ocldbg
