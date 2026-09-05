"""
test_breakpoint.py — integration test scaffold for Milestone 2

Owner: Person F

Starts ocldbg in DAP mode, sets a breakpoint, verifies the debugger halts
at the correct work-item and reports the expected variable values.

Requires:
    - ocldbg binary in PATH or BUILD_DIR env var
    - pocl installed with POCL_EXTRA_BUILD_FLAGS support
    - A compiled host binary for reduction_bug.cl (see tests/host/)
"""

import json
import os
import subprocess
import struct
import unittest

OCLDBG = os.environ.get("OCLDBG_BIN", "ocldbg")
HOST_BIN = os.environ.get("TEST_HOST_BIN", "tests/host/reduction_host")


def send_dap(proc, msg: dict) -> None:
    """Send a DAP message to the ocldbg process."""
    body = json.dumps(msg).encode()
    header = f"Content-Length: {len(body)}\r\n\r\n".encode()
    proc.stdin.write(header + body)
    proc.stdin.flush()


def recv_dap(proc) -> dict:
    """Read one DAP message from the ocldbg process."""
    # TODO (Person F): read Content-Length header, then body
    raise NotImplementedError("recv_dap not yet implemented")


class TestBreakpoint(unittest.TestCase):

    def setUp(self):
        # TODO (Person F): launch ocldbg --backend cpu <host_binary>
        # and connect to its stdin/stdout DAP stream
        pass

    def tearDown(self):
        pass

    def test_breakpoint_halts_at_correct_wi(self):
        """
        Set a breakpoint at reduction_bug.cl line 19 (the inner loop body).
        Expect halt at work-item (3,0,0).
        Expect variable `acc` to be readable.
        """
        # TODO (Person F):
        #   1. Send DAP `initialize`
        #   2. Send DAP `launch` with host binary and kernel
        #   3. Send DAP `setBreakpoints` for reduction_bug.cl line 19
        #   4. Send DAP `configurationDone`
        #   5. Wait for `stopped` event
        #   6. Assert event body.threadId maps to WI(3,0,0)
        #   7. Send DAP `variables` request
        #   8. Assert `acc` is present and type is float
        self.skipTest("Not yet implemented")

    def test_step_advances_work_item(self):
        """
        After halting at a breakpoint, step_over() should advance to the
        next source line.
        """
        self.skipTest("Not yet implemented")


if __name__ == "__main__":
    unittest.main()
