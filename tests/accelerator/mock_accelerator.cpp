// The mock plugin scopes one of its breakpoints to a module named a.out.
// RUN: mkdir -p %t.d
// RUN: %clangxx -g %s -o %t.d/a.out
// RUN: env LLDB_DEBUGSERVER_PATH=%mock_accelerator_server %ocldbg --dry-run --backend accelerator --print r0 --print pc --print not_a_register %t.d/a.out | %FileCheck %s
// REQUIRES: mock-accelerator

// The mock accelerator plugin in lldb-server sets its breakpoints on these
// functions and, at the connect hook, asks LLDB to create a second target for
// the accelerator. The mock has one thread whose register N reads 0x1000 + N.
extern "C" {
void mock_gpu_accelerator_initialize() {}
void mock_gpu_accelerator_connect() {}
int mock_gpu_accelerator_compute(int x) { return x * 2; }
int mock_gpu_accelerator_finish() { return 0; }
}

int main() {
    mock_gpu_accelerator_initialize();
    mock_gpu_accelerator_connect();
    int result = mock_gpu_accelerator_compute(21);
    mock_gpu_accelerator_finish();
    return result == 42 ? 0 : 1;
}

// LLDB prints its own stop messages too, so the lines below are not adjacent.
// CHECK: [ocldbg] Launching {{.*}}mock_accelerator.cpp.tmp.d/a.out under LLDB with an accelerator plugin
// CHECK: [ocldbg] Accelerator plugin stopped the host in mock_gpu_accelerator_initialize
// CHECK: [ocldbg] Accelerator plugin stopped the host in mock_gpu_accelerator_connect
// CHECK: [ocldbg] Accelerator target {{.*}} connected with 1 thread(s)
// CHECK: [ocldbg]   WI(0,0,0) grp(0,0,0)
// CHECK: [ocldbg]     r0 = 0x0000000000001000
// CHECK: [ocldbg]     pc = 0x0000000000001004
// CHECK: [ocldbg]     not_a_register = <unavailable>
// CHECK: [ocldbg] Accelerator plugin stopped the host in mock_gpu_accelerator_compute
// CHECK: [ocldbg] Accelerator plugin stopped the host in mock_gpu_accelerator_finish
// CHECK: [ocldbg] Host exited with status 0
// CHECK: [ocldbg] Accelerator session completed with 0 stop(s).
