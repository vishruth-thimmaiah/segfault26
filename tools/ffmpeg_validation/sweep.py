#!/usr/bin/env python3
"""Sweep ocldbg's Oclgrind backend across every FFmpeg OpenCL kernel we can reach.

For each kernel: break on its output-write line, ask for the locals the source
declares, and record whether execution stopped there and how many of those
locals resolved to a value.
"""
import json, pathlib, re, subprocess, sys, os

SP = pathlib.Path(__file__).parent
CL = SP / "ffcl"
OCLDBG = "/home/deval/segfault26/build/ocldbg"
FFMPEG = "/usr/bin/ffmpeg"
SIZE = os.environ.get("SWEEP_SIZE", "16x16")

# Each entry: the .cl file FFmpeg compiles, and the filter chain that runs it.
CHAIN = {
 "avgblur.cl":     ("avgblur_opencl=sizeX=3:sizeY=3", "yuv420p", 1),
 "colorkey.cl":    ("colorkey_opencl=0x223344:0.3:0.1", "rgba", 1),
 "convolution.cl": ("sobel_opencl", "yuv420p", 1),
 "neighbor.cl":    ("dilation_opencl", "yuv420p", 1),
 "nlmeans.cl":     ("nlmeans_opencl=s=1.0:p=3:r=5", "yuv420p", 1),
 "pad.cl":         ("pad_opencl=width=24:height=24", "yuv420p", 1),
 "transpose.cl":   ("transpose_opencl", "yuv420p", 1),
 "unsharp.cl":     ("unsharp_opencl", "yuv420p", 1),
 "overlay.cl":     ("overlay_opencl", "yuv420p", 2),
 "xfade.cl":       ("xfade_opencl=transition=fade:duration=1:offset=0", "yuv420p", 2),
}
# convolution.cl holds four kernels; each has its own filter.
KERNEL_CHAIN = {
 "convolution_global": "convolution_opencl=0m='0 0 0 0 1 0 0 0 0'",
 "sobel_global": "sobel_opencl", "prewitt_global": "prewitt_opencl",
 "roberts_global": "roberts_opencl", "erosion_global": "erosion_opencl",
 "dilation_global": "dilation_opencl",
 # Several kernels are alternatives chosen by a filter option, so each needs
 # the option that selects it rather than the file's default chain.
 "colorkey": "colorkey_opencl=0x223344:0.3:0.0",
 "wipeleft":  "xfade_opencl=transition=wipeleft:duration=1:offset=0",
 "wiperight": "xfade_opencl=transition=wiperight:duration=1:offset=0",
 "wipeup":    "xfade_opencl=transition=wipeup:duration=1:offset=0",
 "wipedown":  "xfade_opencl=transition=wipedown:duration=1:offset=0",
 "slideleft": "xfade_opencl=transition=slideleft:duration=1:offset=0",
 "slideright":"xfade_opencl=transition=slideright:duration=1:offset=0",
 "slideup":   "xfade_opencl=transition=slideup:duration=1:offset=0",
 "slidedown": "xfade_opencl=transition=slidedown:duration=1:offset=0",
}
# A few kernels need a different pixel format to be selected at all.
KERNEL_PIXFMT = {
 "overlay_internal_alpha": "rgba", "overlay_external_alpha": "rgba",
 "colorkey": "rgba",
}

def locals_of(path, start, end):
    """Local scalar/vector names the kernel declares, as printable candidates."""
    body = path.read_text().splitlines()[start:end]
    names = []
    for line in body:
        m = re.match(r'\s*(?:const\s+)?(?:int|float|uint|long)[248]?\s+(\w+)\s*[=;]', line)
        if m and m.group(1) not in names:
            names.append(m.group(1))
    return names[:4]

def build_cmd(chain, pixfmt, ninputs, brk, prints):
    vf = f"format={pixfmt},hwupload,{chain},hwdownload,format={pixfmt}"
    if ninputs == 2:
        vf = (f"[0:v]format={pixfmt},hwupload[a];[1:v]format={pixfmt},hwupload[b];"
              f"[a][b]{chain},hwdownload,format={pixfmt}")
    cmd = [OCLDBG, "--backend", "oclgrind", "--dry-run",
           "--break-at", str(brk), "--break-for", "1"]
    for p in prints:
        cmd += ["--print", p]
    cmd += [FFMPEG, "-hide_banner", "-loglevel", "error",
            "-init_hw_device", "opencl=ocl", "-f", "lavfi",
            "-i", f"testsrc=size={SIZE}:rate=1:duration=1"]
    if ninputs == 2:
        cmd += ["-f", "lavfi", "-i", f"testsrc2=size={SIZE}:rate=1:duration=1"]
    cmd += ["-filter_hw_device", "ocl",
            "-filter_complex" if ninputs == 2 else "-vf", vf,
            "-frames:v", "1", "-f", "null", "-"]
    return cmd

def main():
    cat = json.loads((CL / "kernels.json").read_text())
    results = []
    for fname, kernels in cat.items():
        for k in kernels:
            name, brk = k["kernel"], k["write_line"]
            row = {"file": fname, "kernel": name, "line": brk,
                   "status": "no oracle point", "vars_asked": 0, "vars_read": 0}
            if brk is None:
                # No image write to anchor on; a breakpoint still works anywhere,
                # so fall back to a statement in the middle of the kernel body.
                brk = k["start"] + max(1, (k["end"] - k["start"]) // 2)
                row["line"] = brk
                row["status"] = "no image write; broke mid-body"
            if fname not in CHAIN:
                row["status"] = "no filter chain"; results.append(row); continue
            chain, pixfmt, nin = CHAIN[fname]
            chain = KERNEL_CHAIN.get(name, chain)
            pixfmt = KERNEL_PIXFMT.get(name, pixfmt)
            prints = locals_of(CL / fname, k["start"], k["end"])
            row["vars_asked"] = len(prints)
            try:
                p = subprocess.run(build_cmd(chain, pixfmt, nin, brk, prints),
                                   capture_output=True, text=True, timeout=300)
                out = p.stdout + p.stderr
            except subprocess.TimeoutExpired:
                row["status"] = "timeout"; results.append(row); continue
            if "Breakpoint hit at line" in out:
                read = len(re.findall(r"^\s+\w+ = (?!<unavailable>)", out, re.M))
                row["status"] = "stopped"; row["vars_read"] = read
            elif "completed with 0" in out or "Dry run completed" in out:
                row["status"] = "kernel not reached"
            else:
                row["status"] = "launch failed"
            results.append(row)
            print(f"  {fname:<16} {name:<22} line {str(brk):<5} {row['status']:<18} "
                  f"vars {row['vars_read']}/{row['vars_asked']}", flush=True)
    (SP / "sweep_results.json").write_text(json.dumps(results, indent=1))
    print(f"\nwrote {len(results)} rows")

main()
