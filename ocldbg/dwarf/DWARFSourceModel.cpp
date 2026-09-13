#include "DWARFSourceModel.h"

#include <algorithm>
#include <functional>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/DebugInfo/DIContext.h>
#include <llvm/DebugInfo/DWARF/DWARFAddressRange.h>
#include <llvm/DebugInfo/DWARF/DWARFCompileUnit.h>
#include <llvm/DebugInfo/DWARF/DWARFContext.h>
#include <llvm/DebugInfo/DWARF/DWARFDebugLine.h>
#include <llvm/DebugInfo/DWARF/DWARFDie.h>
#include <llvm/DebugInfo/DWARF/DWARFFormValue.h>
#include <llvm/DebugInfo/DWARF/DWARFLocationExpression.h>
#include <llvm/Object/Binary.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/Error.h>

using namespace llvm;

namespace ocldbg {

struct DWARFSourceModel::Impl {
    bool loaded = false;
    object::OwningBinary<object::ObjectFile> owning_binary;
    object::ObjectFile *object_file = nullptr;
    std::unique_ptr<DWARFContext> dwarf;
};

DWARFSourceModel::DWARFSourceModel() : impl_(std::make_unique<Impl>()) {}
DWARFSourceModel::~DWARFSourceModel() = default;

bool DWARFSourceModel::load(const std::string &binary_path) {
    auto expected_obj = object::ObjectFile::createObjectFile(binary_path);
    if (!expected_obj) {
        consumeError(expected_obj.takeError());
        return false;
    }
    impl_->owning_binary = std::move(*expected_obj);
    impl_->object_file = impl_->owning_binary.getBinary();
    if (!impl_->object_file) {
        return false;
    }
    impl_->dwarf = DWARFContext::create(*impl_->object_file);
    if (!impl_->dwarf) {
        return false;
    }
    impl_->loaded = true;
    return true;
}

SourceLocation DWARFSourceModel::pc_to_source(HostAddress pc) const {
    if (!impl_->loaded || !impl_->dwarf) {
        return {};
    }
    object::SectionedAddress saddr{pc, object::SectionedAddress::UndefSection};
    DILineInfoSpecifier spec;
    spec.FLIKind = DILineInfoSpecifier::FileLineInfoKind::AbsoluteFilePath;
    if (auto line_info = impl_->dwarf->getLineInfoForAddress(saddr, spec)) {
        if (line_info->Line != 0) {
            return SourceLocation{line_info->FileName, line_info->Line};
        }
    }
    return {};
}

std::vector<HostAddress> DWARFSourceModel::source_to_pcs(const SourceLocation &loc) const {
    if (!impl_->loaded || !impl_->dwarf) {
        return {};
    }
    std::vector<HostAddress> pcs;
    for (const auto &cu : impl_->dwarf->compile_units()) {
        const DWARFDebugLine::LineTable *lt = impl_->dwarf->getLineTableForUnit(cu.get());
        if (!lt) {
            continue;
        }
        for (const auto &row : lt->Rows) {
            if (row.Line != loc.line) {
                continue;
            }
            std::string file_path;
            if (lt->getFileNameByIndex(row.File, cu->getCompilationDir(),
                                       DILineInfoSpecifier::FileLineInfoKind::AbsoluteFilePath,
                                       file_path)) {
                if (file_path == loc.file || (loc.file.size() <= file_path.size() &&
                                              file_path.compare(file_path.size() - loc.file.size(),
                                                                loc.file.size(), loc.file) == 0)) {
                    pcs.push_back(row.Address.Address);
                }
            }
        }
    }
    std::sort(pcs.begin(), pcs.end());
    pcs.erase(std::unique(pcs.begin(), pcs.end()), pcs.end());
    return pcs;
}

std::string DWARFSourceModel::type_name(uint64_t dwarf_type_offset) const {
    if (!impl_->loaded || !impl_->dwarf) {
        return "<unknown>";
    }
    DWARFDie type_die = impl_->dwarf->getDIEForOffset(dwarf_type_offset);
    if (!type_die.isValid()) {
        return "<unknown>";
    }

    switch (type_die.getTag()) {
    case dwarf::DW_TAG_base_type:
    case dwarf::DW_TAG_typedef:
    case dwarf::DW_TAG_structure_type:
    case dwarf::DW_TAG_union_type:
    case dwarf::DW_TAG_class_type: {
        const char *name = type_die.getName(DINameKind::ShortName);
        return name ? name : "<unknown>";
    }
    case dwarf::DW_TAG_pointer_type: {
        DWARFDie pointee = type_die.getAttributeValueAsReferencedDie(dwarf::DW_AT_type);
        if (pointee.isValid()) {
            return type_name(pointee.getOffset()) + " *";
        }
        return "void *";
    }
    case dwarf::DW_TAG_const_type: {
        DWARFDie base = type_die.getAttributeValueAsReferencedDie(dwarf::DW_AT_type);
        if (base.isValid()) {
            return "const " + type_name(base.getOffset());
        }
        return "const";
    }
    case dwarf::DW_TAG_array_type: {
        DWARFDie elem = type_die.getAttributeValueAsReferencedDie(dwarf::DW_AT_type);
        if (elem.isValid()) {
            return type_name(elem.getOffset()) + "[]";
        }
        return "[]";
    }
    default: {
        const char *name = type_die.getName(DINameKind::ShortName);
        return name ? name : "<unknown>";
    }
    }
}

std::vector<VarInfo> DWARFSourceModel::variables_in_scope(HostAddress pc) const {
    if (!impl_->loaded || !impl_->dwarf) {
        return {};
    }
    std::vector<VarInfo> vars;

    std::function<void(DWARFDie)> collect_die;
    collect_die = [&](DWARFDie die) {
        if (!die.isValid()) {
            return;
        }
        for (DWARFDie child : die.children()) {
            dwarf::Tag tag = child.getTag();
            if (tag == dwarf::DW_TAG_variable || tag == dwarf::DW_TAG_formal_parameter) {
                VarInfo v;
                const char *name = child.getName(DINameKind::ShortName);
                if (name) {
                    v.name = name;
                }
                v.location_pc = pc;

                DWARFDie type_die = child.getAttributeValueAsReferencedDie(dwarf::DW_AT_type);
                if (!type_die.isValid()) {
                    DWARFDie origin =
                        child.getAttributeValueAsReferencedDie(dwarf::DW_AT_abstract_origin);
                    if (origin.isValid()) {
                        type_die = origin.getAttributeValueAsReferencedDie(dwarf::DW_AT_type);
                    }
                }
                if (type_die.isValid()) {
                    v.type_name = type_name(type_die.getOffset());
                }

                if (v.type_name.ends_with("*")) {
                    v.address_space = "__global";
                }

                auto locs = child.getLocations(dwarf::DW_AT_location);
                if (locs) {
                    for (const auto &loc_expr : *locs) {
                        if (!loc_expr.Range ||
                            (pc >= loc_expr.Range->LowPC && pc < loc_expr.Range->HighPC)) {
                            v.dwarf_location_expr.assign(loc_expr.Expr.begin(),
                                                         loc_expr.Expr.end());
                            break;
                        }
                    }
                }
                if (v.dwarf_location_expr.empty()) {
                    auto loc_attr = child.find(dwarf::DW_AT_location);
                    if (!loc_attr) {
                        DWARFDie origin =
                            child.getAttributeValueAsReferencedDie(dwarf::DW_AT_abstract_origin);
                        if (origin.isValid()) {
                            loc_attr = origin.find(dwarf::DW_AT_location);
                        }
                    }
                    if (loc_attr) {
                        if (auto block = loc_attr->getAsBlock()) {
                            v.dwarf_location_expr.assign(block->begin(), block->end());
                        }
                    }
                }

                if (!v.name.empty()) {
                    bool exists = false;
                    for (const auto &existing : vars) {
                        if (existing.name == v.name) {
                            exists = true;
                            break;
                        }
                    }
                    if (!exists) {
                        vars.push_back(std::move(v));
                    }
                }
            } else if (tag == dwarf::DW_TAG_inlined_subroutine) {
                collect_die(child);
            } else if (tag == dwarf::DW_TAG_lexical_block) {
                if (pc == 0 || child.addressRangeContainsAddress(pc)) {
                    collect_die(child);
                }
            }
        }
    };

    for (const auto &unit : impl_->dwarf->compile_units()) {
        DWARFDie subprog = unit->getSubroutineForAddress(pc);
        if (subprog.isValid()) {
            collect_die(subprog);
        }
        if (vars.empty()) {
            for (DWARFDie child : unit->getUnitDIE().children()) {
                if (child.getTag() == dwarf::DW_TAG_subprogram) {
                    collect_die(child);
                }
            }
        }
    }

    return vars;
}

bool DWARFSourceModel::loaded() const {
    return impl_->loaded;
}

} // namespace ocldbg
