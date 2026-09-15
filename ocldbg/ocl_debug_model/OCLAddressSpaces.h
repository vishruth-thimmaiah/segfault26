#pragma once
#include <map>
#include <string>

namespace ocldbg {

/// OpenCL address spaces of the variables in a compiled kernel.
///
/// The debug info cannot answer this. pocl lowers the kernel to the host
/// target before emitting the object, which flattens every pointer to address
/// space 0 and leaves no DW_AT_address_class behind, so `__global float *` and
/// `__local float *` share one DWARF type. What does survive is the
/// !kernel_arg_addr_space metadata LLVM attaches to every spir_kernel, so the
/// address spaces are read from the program bitcode pocl caches beside the
/// kernel module.
class OCLAddressSpaces {
public:
    /// Parse the program bitcode cached alongside @p kernel_module_path, whose
    /// file name also names the kernel. Returns false when there is none, which
    /// is the normal outcome for a module that pocl did not build.
    bool load(const std::string &kernel_module_path);

    [[nodiscard]] bool loaded() const { return loaded_; }

    /// Address space of the pointer @p variable: "__global", "__local",
    /// "__constant" or "__private".
    ///
    /// Empty for a non-pointer, which carries no address space qualifier, and
    /// for a pointer the metadata does not cover — a pointer declared in the
    /// kernel body, whose address space pocl has already flattened away.
    [[nodiscard]] std::string lookup(const std::string &variable,
                                     const std::string &type_name) const;

private:
    std::map<std::string, std::string> arg_spaces_;
    bool loaded_ = false;
};

} // namespace ocldbg
