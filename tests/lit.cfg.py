import os
import shutil
import lit.formats

config.name = "segfault26"
config.test_format = lit.formats.ShTest(True)
config.suffixes = [".cl", ".ll", ".test"]

config.test_source_root = os.path.dirname(__file__)
config.test_exec_root = getattr(
    config, "segfault26_binary_root", os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "build"))
)

# Directories that do not contain lit tests
config.excludes = ["kernels", "integration"]

# Resolve test_host_runner
host_runner = getattr(config, "test_host_runner", None)
if not host_runner or not os.path.exists(host_runner):
    candidate = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "build", "test_host_runner"))
    if os.path.exists(candidate):
        host_runner = candidate
    else:
        host_runner = "test_host_runner"

ocldbg = getattr(config, "ocldbg_bin", None)
if not ocldbg or not os.path.exists(ocldbg):
    candidate = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "build", "ocldbg"))
    if os.path.exists(candidate):
        ocldbg = candidate
    else:
        ocldbg = "ocldbg"

filecheck = getattr(config, "filecheck_bin", None)
if not filecheck or not os.path.exists(filecheck):
    filecheck = shutil.which("FileCheck") or "FileCheck"

llvm_dis = shutil.which("llvm-dis") or "llvm-dis"

config.substitutions.append(("%test_host_runner", host_runner))
config.substitutions.append(("%ocldbg", ocldbg))
config.substitutions.append(("%FileCheck", filecheck))
config.substitutions.append(("%llvm-dis", llvm_dis))

for var in [
    "PATH",
    "LD_LIBRARY_PATH",
    "OCL_ICD_VENDORS",
    "POCL_CACHE_DIR",
    "LLDB_DEBUGSERVER_PATH",
]:
    if var in os.environ:
        config.environment[var] = os.environ[var]
