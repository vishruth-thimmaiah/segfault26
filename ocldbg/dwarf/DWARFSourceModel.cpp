#include "DWARFSourceModel.h"
#include <stdexcept>

// TODO (Person B): implement using LLVM's DWARF libraries.
//
// Recommended approach:
//   #include <llvm/DebugInfo/DWARF/DWARFContext.h>
//   #include <llvm/Object/ELFObjectFile.h>
//
// Key LLVM APIs:
//   DWARFContext::create()          - load DWARF from an object file
//   DWARFContext::getLineTableForUnit() - source line tables
//   DWARFContext::getDIEForOffset()  - type/variable DIEs
//   DWARFDie::getAttributeValueAsAddress() - extract PC ranges
//
// Reminder: examine pocl's DWARF output first via:
//   POCL_EXTRA_BUILD_FLAGS="-g -cl-opt-disable" \
//   POCL_LEAVE_KERNEL_COMPILER_TEMP_FILES=1 ./host
//   dwarfdump /tmp/pocl-*/kernel*.o
// to understand the structure before implementing.

namespace ocldbg {

struct DWARFSourceModel::Impl {
    // TODO: hold DWARFContext and parsed tables here
    bool loaded = false;
};

DWARFSourceModel::DWARFSourceModel() : impl_(std::make_unique<Impl>()) {}
DWARFSourceModel::~DWARFSourceModel() = default;

bool DWARFSourceModel::load(const std::string & /*binary_path*/) {
    // TODO (Person B): open binary, create DWARFContext, parse line tables
    // and compilation units. Set impl_->loaded = true on success.
    throw std::runtime_error("DWARFSourceModel::load not yet implemented");
}

SourceLocation DWARFSourceModel::pc_to_source(HostAddress /*pc*/) const {
    // TODO (Person B): walk line tables to find file/line for pc
    return {};
}

std::vector<HostAddress>
DWARFSourceModel::source_to_pcs(const SourceLocation & /*loc*/) const {
    // TODO (Person B): scan line table for all PCs matching file:line
    return {};
}

std::vector<VarInfo>
DWARFSourceModel::variables_in_scope(HostAddress /*pc*/) const {
    // TODO (Person B): find the innermost lexical scope containing pc,
    // collect DW_TAG_variable and DW_TAG_formal_parameter DIEs,
    // extract DW_AT_location expressions and DW_AT_type references.
    return {};
}

std::string DWARFSourceModel::type_name(uint64_t /*dwarf_type_offset*/) const {
    // TODO (Person B): resolve DW_TAG_base_type / DW_TAG_typedef /
    // DW_TAG_pointer_type etc. into a human-readable string.
    return "<unknown>";
}

bool DWARFSourceModel::loaded() const { return impl_->loaded; }

} // namespace ocldbg
