#include "OCLAddressSpaces.h"

#include <cstdint>
#include <filesystem>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <memory>

namespace ocldbg {

namespace {

/// SPIR address space numbering, as used by !kernel_arg_addr_space.
std::string address_space_name(uint64_t spir_address_space) {
    switch (spir_address_space) {
    case 0:
        return "__private";
    case 1:
        return "__global";
    case 2:
        return "__constant";
    case 3:
        return "__local";
    case 4:
        return "__generic";
    default:
        return {};
    }
}

/// pocl caches the whole program's bitcode three levels above the per-kernel
/// object it builds: <program>/<kernel>/<variant>/<kernel>.so.
std::filesystem::path program_bitcode_for(const std::filesystem::path &kernel_module) {
    return kernel_module.parent_path().parent_path().parent_path() / "program.bc";
}

std::string operand_string(const llvm::MDNode *node, unsigned index) {
    if (node == nullptr || index >= node->getNumOperands()) {
        return {};
    }
    const auto *str = llvm::dyn_cast_or_null<llvm::MDString>(node->getOperand(index));
    return str != nullptr ? str->getString().str() : std::string{};
}

/// Returns the address space operand, or a negative value when absent.
int64_t operand_int(const llvm::MDNode *node, unsigned index) {
    if (node == nullptr || index >= node->getNumOperands()) {
        return -1;
    }
    const auto *value = llvm::dyn_cast_or_null<llvm::ConstantAsMetadata>(node->getOperand(index));
    if (value == nullptr) {
        return -1;
    }
    const auto *constant = llvm::dyn_cast<llvm::ConstantInt>(value->getValue());
    return constant != nullptr ? constant->getSExtValue() : -1;
}

} // namespace

bool OCLAddressSpaces::load(const std::string &kernel_module_path) {
    arg_spaces_.clear();
    loaded_ = false;

    std::filesystem::path module_path(kernel_module_path);
    std::filesystem::path bitcode = program_bitcode_for(module_path);
    std::error_code ec;
    if (!std::filesystem::exists(bitcode, ec)) {
        return false;
    }

    llvm::LLVMContext context;
    llvm::SMDiagnostic diag;
    std::unique_ptr<llvm::Module> program = llvm::parseIRFile(bitcode.string(), diag, context);
    if (!program) {
        return false;
    }

    std::string kernel_name = module_path.stem().string();
    const llvm::Function *kernel = program->getFunction(kernel_name);
    if (kernel == nullptr) {
        return false;
    }
    const llvm::MDNode *spaces = kernel->getMetadata("kernel_arg_addr_space");
    const llvm::MDNode *names = kernel->getMetadata("kernel_arg_name");
    if (spaces == nullptr || names == nullptr) {
        return false;
    }

    for (unsigned i = 0; i < spaces->getNumOperands(); ++i) {
        std::string name = operand_string(names, i);
        int64_t space = operand_int(spaces, i);
        if (name.empty() || space < 0) {
            continue;
        }
        std::string space_name = address_space_name(static_cast<uint64_t>(space));
        if (!space_name.empty()) {
            arg_spaces_[name] = space_name;
        }
    }

    loaded_ = true;
    return true;
}

std::string OCLAddressSpaces::lookup(const std::string &variable,
                                     const std::string &type_name) const {
    if (!loaded_ || !type_name.ends_with("*")) {
        return {};
    }
    auto arg = arg_spaces_.find(variable);
    return arg != arg_spaces_.end() ? arg->second : std::string{};
}

} // namespace ocldbg
