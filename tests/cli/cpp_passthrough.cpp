// RUN: %clangxx -g -O0 %s -o %t.exe
// RUN: %ocldbg -b -o "b compute" -o "r" -o "p a" -o "p b" -o "p a + b" -o "c" %t.exe | %FileCheck %s

/**
  * cpp_passthrough.cpp — Verifies that ocldbg passes standard C++ debugging commands
  * directly to the embedded LLDB interpreter when operating on C++ binaries.
  */

#include <iostream>

int compute(int a, int b) {
    int sum = a + b;
    return sum;
}

int main() {
    int res = compute(10, 32);
    std::cout << "Result: " << res << std::endl;
    return 0;
}

// CHECK: Current executable set to '{{.*}}cpp_passthrough.cpp.tmp.exe' ({{x86_64|aarch64}}).
// CHECK: (ocldbg) b compute
// CHECK: Breakpoint 1: where = {{.*}}compute(int, int)
// CHECK: (ocldbg) r
// CHECK: Process {{[0-9]+}} stopped
// CHECK: * thread #1, name = '{{.*}}cpp_passthrough{{.*}}', stop reason = breakpoint 1.1
// CHECK: frame #0: {{.*}}compute(a=10, b=32) at cpp_passthrough.cpp:12
// CHECK: (ocldbg) p a
// CHECK: (int) 10
// CHECK: (ocldbg) p b
// CHECK: (int) 32
// CHECK: (ocldbg) p a + b
// CHECK: (int) 42
// CHECK: (ocldbg) c
// CHECK: Process {{[0-9]+}} resuming
// CHECK: Process {{[0-9]+}} exited with status = 0
