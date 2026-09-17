#!/usr/bin/env python3
"""Differential test: one kernel, two unrelated debugging mechanisms.

The pocl backend stops a host thread with ptrace and decodes DWARF against
machine registers. The Oclgrind backend halts an LLVM IR interpreter from a
plugin inside the host process. They share the OCLWorkItem abstraction and
nothing else, so agreement is evidence neither is inventing values.

Stops are paired by order, not by work-item label, because whether the label is
right is itself one of the things under test.
"""
import json, math, os, pathlib, re, subprocess, sys

SP = pathlib.Path(__file__).parent
OCLDBG = "/home/deval/segfault26/build/ocldbg"
FFMPEG = "/usr/bin/ffmpeg"
HOME = os.path.expanduser("~")

def run(backend, case):
    env = dict(os.environ)
    env["PATH"] = "/usr/lib/llvm-22/bin:" + env.get("PATH", "")
    cmd = [OCLDBG]
    device = "opencl=ocl"
    if backend == "oclgrind":
        cmd += ["--backend", "oclgrind"]
    else:
        # FFmpeg does not build its kernels with -g, so pocl must be told to.
        env.update(OCL_ICD_VENDORS=f"{HOME}/conda_root/envs/pocl-env/etc/OpenCL/vendors",
                   LD_LIBRARY_PATH=f"{HOME}/conda_root/envs/pocl-env/lib",
                   LLDB_DEBUGSERVER_PATH="/usr/lib/llvm-22/bin/lldb-server",
                   POCL_CPU_NUM_WORKERS="1",
                   POCL_EXTRA_BUILD_FLAGS="-g -cl-opt-disable")
        device = "opencl=ocl:0.0"
    cmd += ["--dry-run", "--break-at", str(case["line"]), "--break-for", str(case["hits"])]
    for p in case["prints"]:
        cmd += ["--print", p]
    vf = (f"format=yuv420p,hwupload,{case['chain']},hwdownload,format=yuv420p")
    cmd += [FFMPEG, "-hide_banner", "-loglevel", "error", "-init_hw_device", device,
            "-f", "lavfi", "-i", f"testsrc=size={case['size']}:rate=1:duration=1",
            "-filter_hw_device", "ocl", "-vf", vf, "-frames:v", "1", "-f", "null", "-"]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=900, env=env)
    return p.stdout + p.stderr

def stops(text):
    out, cur = [], None
    for ln in text.splitlines():
        # WI(?) is a legitimate report: the backend could not identify the
        # work-item and says so rather than naming one.
        m = re.search(r"Breakpoint hit at line \d+ for (WI\([^)]*\))", ln)
        if m:
            cur = [m.group(1), {}]; out.append(cur); continue
        m = re.match(r"\s+(\w+)(?: \([^)]*\))? = (.+)$", ln)
        if m and cur:
            cur[1][m.group(1)] = m.group(2).strip()
    return out

def nums_of(v):
    """The numbers in a printed value, or None when there is no value."""
    if v is None or "unavailable" in v:
        return None
    found = re.findall(r"-?\d+\.?\d*(?:e[-+]?\d+)?", v)
    return [float(x) for x in found] if found else v.strip()

def same(a, b):
    """The backends print floats to different widths, so 1.02745 and 1.027451 are
    one value shown two ways. Rounding to a fixed place puts a pair like that on
    either side of a boundary and calls it a disagreement, so compare with a
    relative tolerance instead."""
    if a is None or b is None:
        return None
    if isinstance(a, str) or isinstance(b, str):
        return a == b
    if len(a) != len(b):
        return False
    return all(math.isclose(x, y, rel_tol=1e-5, abs_tol=1e-9) for x, y in zip(a, b))

def main():
    cases = json.loads(pathlib.Path(sys.argv[1]).read_text())
    agree = differ = one_only = lbl_ok = lbl_bad = 0
    rows = []
    for c in cases:
        a, b = stops(run("oclgrind", c)), stops(run("cpu", c))
        print(f"{c['name']}: oclgrind {len(a)} stops, pocl {len(b)} stops", flush=True)
        for i in range(min(len(a), len(b))):
            (la, va), (lb, vb) = a[i], b[i]
            lbl_ok, lbl_bad = (lbl_ok+1, lbl_bad) if la == lb else (lbl_ok, lbl_bad+1)
            for var in c["prints"]:
                x, y = va.get(var), vb.get(var)
                verdict_same = same(nums_of(x), nums_of(y))
                if verdict_same is None:
                    one_only += 1; verdict = "one backend only"
                elif verdict_same:
                    agree += 1; verdict = "agree"
                else:
                    differ += 1; verdict = "DIFFER"
                rows.append(dict(kernel=c["name"], stop=i, var=var, oclgrind=x, pocl=y,
                                 verdict=verdict, label_oclgrind=la, label_pocl=lb))
                print(f"   stop{i} {var:<7} oclgrind={str(x):<26} pocl={str(y):<26} {verdict}",
                      flush=True)
    (SP/"xback_results.json").write_text(json.dumps(rows, indent=1))
    tot = agree + differ
    print(f"\nVALUES  comparable {tot}   agree {agree}   differ {differ}   "
          f"readable on one backend only {one_only}")
    if tot: print(f"        value agreement {100.0*agree/tot:.1f}%")
    print(f"LABELS  stops paired {lbl_ok+lbl_bad}   same work-item id {lbl_ok}   different {lbl_bad}")
    if lbl_ok+lbl_bad: print(f"        label agreement {100.0*lbl_ok/(lbl_ok+lbl_bad):.1f}%")

main()
