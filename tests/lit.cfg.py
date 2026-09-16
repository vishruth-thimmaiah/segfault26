import os
import platform
import shutil
import lit.formats

config.name = "segfault26"
config.test_format = lit.formats.ShTest(True)
config.suffixes = [".cl", ".ll", ".test", ".cpp"]

# Per-test timeout, to catch a hang rather than to budget the work. It covers
# every RUN line in a test: workgroup_bounds.cl drives six debuggee launches and
# takes about 20s here with a cold pocl cache, so the limit leaves room for a
# slower runner. Needs the psutil module, without which lit refuses to start.
lit_config.maxIndividualTestTime = 120

config.test_source_root = os.path.dirname(__file__)
config.test_exec_root = getattr(
    config, "segfault26_binary_root", os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "build"))
)

# Directories that do not contain lit tests. amd_backend/ holds manual
# verification kernels for `ocldbg --backend amd` (needs real AMD GPU +
# ROCm/amd-dbgapi, which CI doesn't have); no RUN: lines by design.
config.excludes = ["kernels", "integration", "amd_backend"]

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
clangxx = shutil.which("clang++") or "clang++"

# The Oclgrind backend needs the plugin that gets built only when Oclgrind
# was found; tests that drive it declare REQUIRES: oclgrind.
oclgrind_plugin = getattr(config, "oclgrind_plugin", "")
if oclgrind_plugin and os.path.exists(oclgrind_plugin):
    config.available_features.add("oclgrind")

# Lets a test say which host architecture it needs.
config.available_features.add(platform.machine())

config.substitutions.append(("%test_host_runner", host_runner))
config.substitutions.append(("%ocldbg", ocldbg))
config.substitutions.append(("%FileCheck", filecheck))
config.substitutions.append(("%llvm-dis", llvm_dis))
config.substitutions.append(("%clangxx", clangxx))

for var in [
    "PATH",
    "LD_LIBRARY_PATH",
    "OCL_ICD_VENDORS",
    "POCL_CACHE_DIR",
    "LLDB_DEBUGSERVER_PATH",
]:
    if var in os.environ:
        config.environment[var] = os.environ[var]
