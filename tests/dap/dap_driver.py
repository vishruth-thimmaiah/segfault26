import sys
import subprocess
import os
import json

def send_msg(p, obj):
    data = json.dumps(obj).encode("utf-8")
    header = f"Content-Length: {len(data)}\r\n\r\n".encode("utf-8")
    p.stdin.write(header + data)
    p.stdin.flush()

def read_msg(p):
    line = p.stdout.readline().decode("utf-8")
    while line in ["\r\n", "\n"]:
        line = p.stdout.readline().decode("utf-8")
    if not line:
        return None
    if not line.startswith("Content-Length:"):
        return None
    length = int(line.split(":")[1].strip())
    blank = p.stdout.readline().decode("utf-8")
    raw = p.stdout.read(length)
    return json.loads(raw.decode("utf-8"))

def expect_msg(p):
    msg = read_msg(p)
    if msg is None:
        sys.exit(1)
    return msg

def main():
    if len(sys.argv) < 5:
        print("Usage: dap_driver.py <ocldbg> <test_host_runner> <kernel.cl> <kernel_name>")
        sys.exit(1)

    ocldbg_bin = sys.argv[1]
    host_runner = sys.argv[2]
    kernel_file = sys.argv[3]
    kernel_name = sys.argv[4]

    env = dict(os.environ)
    p = subprocess.Popen([ocldbg_bin, "--dap"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=env)

    # 1. Initialize
    send_msg(p, {"seq": 1, "type": "request", "command": "initialize", "arguments": {"adapterID": "ocldbg"}})
    r_init = read_msg(p)
    e_init = read_msg(p)
    if r_init and r_init.get("success"):
        print("[dap] initialized")

    # 2. Launch
    send_msg(p, {"seq": 2, "type": "request", "command": "launch", "arguments": {"program": host_runner, "args": [kernel_file, kernel_name] + sys.argv[5:]}})
    r_launch = read_msg(p)
    if r_launch and r_launch.get("success"):
        print("[dap] launched")

    # 3. SetBreakpoints
    bp_line = 13
    send_msg(p, {"seq": 3, "type": "request", "command": "setBreakpoints", "arguments": {"source": {"path": kernel_file}, "breakpoints": [{"line": bp_line}]}})
    r_bp = read_msg(p)
    if r_bp and r_bp.get("success"):
        bps = r_bp.get("body", {}).get("breakpoints", [])
        if bps and bps[0].get("verified"):
            print(f"[dap] breakpoints set: line {bps[0].get('line')}")

    # 4. ConfigurationDone
    send_msg(p, {"seq": 4, "type": "request", "command": "configurationDone", "arguments": {}})
    read_msg(p)

    # Wait for stopped event
    e_stop1 = read_msg(p)
    if e_stop1 and e_stop1.get("event") == "stopped":
        print("[dap] stopped at breakpoint")

    # 5. Threads
    send_msg(p, {"seq": 5, "type": "request", "command": "threads"})
    r_threads = read_msg(p)
    threads = r_threads.get("body", {}).get("threads", [])
    if threads:
        print(f"[dap] threads: {threads[0].get('name')}")

    # 6. StackTrace
    send_msg(p, {"seq": 6, "type": "request", "command": "stackTrace", "arguments": {"threadId": 1}})
    r_st = read_msg(p)
    frames = r_st.get("body", {}).get("stackFrames", [])
    if frames:
        print(f"[dap] stackTrace: line {frames[0].get('line')}")

    # 7. Scopes
    send_msg(p, {"seq": 7, "type": "request", "command": "scopes", "arguments": {"frameId": 1}})
    r_scopes = read_msg(p)
    scopes = r_scopes.get("body", {}).get("scopes", [])
    if scopes:
        print(f"[dap] scopes: {scopes[0].get('name')}")

    # 8. Variables
    send_msg(p, {"seq": 8, "type": "request", "command": "variables", "arguments": {"variablesReference": 1000}})
    r_vars = read_msg(p)
    vars_list = r_vars.get("body", {}).get("variables", [])
    vars_dict = {v.get("name"): v.get("value") for v in vars_list}
    if "acc" in vars_dict:
        print(f"[dap] var: acc = {vars_dict['acc']}")
    if "n" in vars_dict:
        print(f"[dap] var: n = {vars_dict['n']}")

    # 9. SelectWorkItem
    send_msg(p, {"seq": 9, "type": "request", "command": "ocldbg/selectWorkItem", "arguments": {"gx": 0, "gy": 0, "gz": 0}})
    r_sel = read_msg(p)
    if r_sel and r_sel.get("body", {}).get("selected"):
        print(f"[dap] selectWorkItem: {r_sel.get('body', {}).get('workItem')}")

    # 10. Continue
    send_msg(p, {"seq": 10, "type": "request", "command": "continue", "arguments": {"threadId": 1}})
    read_msg(p)
    print("[dap] continue")

    # Wait for next stop
    e_stop2 = read_msg(p)
    if e_stop2 and e_stop2.get("event") == "stopped":
        print("[dap] stopped at breakpoint")

    # Variables after continue
    send_msg(p, {"seq": 11, "type": "request", "command": "variables", "arguments": {"variablesReference": 1000}})
    r_vars2 = read_msg(p)
    vars_dict2 = {v.get("name"): v.get("value") for v in r_vars2.get("body", {}).get("variables", [])}
    if "acc" in vars_dict2:
        print(f"[dap] var: acc = {vars_dict2['acc']}")

    # 11. Disconnect
    send_msg(p, {"seq": 12, "type": "request", "command": "disconnect", "arguments": {}})
    read_msg(p)
    p.wait()
    print("[dap] disconnected")

if __name__ == "__main__":
    main()
