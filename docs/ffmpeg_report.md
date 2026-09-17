# Validating ocldbg against FFmpeg

`ocldbg` is exercised in its own test suite by kernels written for its own test
suite. This is a validation against an independent OpenCL consumer: the FFmpeg
binary the distribution ships, unmodified and not rebuilt.

It reports two things separately, because they answer different questions:
**correctness**, whether what the debugger says is true, and **efficiency**,
what it costs to ask.

## Why FFmpeg

Twenty of FFmpeg's video filters run OpenCL kernels. The kernels were written
with no knowledge of this debugger, the binary under test is the distribution's
own, and nothing was recompiled, instrumented or patched. That last point is
what makes the exercise worth anything: a kernel written to suit the debugger
proves nothing about kernels that were not.

## Setup

| Component | Version |
| --- | --- |
| FFmpeg | 6.1.1-3ubuntu5, distribution binary at `/usr/bin/ffmpeg` |
| Oclgrind | built from source against LLVM 22 |
| pocl | 7.1 |
| ocldbg | Oclgrind backend and pocl CPU backend |
| Kernel under test | `libavfilter/opencl/avgblur.cl`, FFmpeg n6.1.1 |

`avgblur_opencl` compiles one program holding two kernels, `avgblur_horiz` and
`avgblur_vert`, run as separate passes. FFmpeg does not build its kernels with
`-g`, so the pocl backend needs `POCL_EXTRA_BUILD_FLAGS="-g -cl-opt-disable"`
for the kernel to carry debug info at all.

### Oclgrind really is the device

Without Oclgrind preloaded the same command finds no OpenCL, so none of the runs
below can be falling through to another implementation:

```
$ ffmpeg -init_hw_device opencl=ocl ...
Device creation failed: -19.
Failed to set value 'opencl=ocl' for option 'init_hw_device': No such device
```

## Scope

FFmpeg is about a million lines of C and `ocldbg` has nothing to say about
almost all of it. Its subject is FFmpeg's OpenCL surface: 13 kernel sources
holding **41 kernels**. Every number below is against that denominator, not
against FFmpeg as a whole.

## Coverage

`tools/ffmpeg_validation/sweep.py` drives each kernel: break on its output
write, ask for the locals the source declares, record whether execution stopped
and how many resolved.

| kernel source | kernels | breakpoint reached | locals resolved |
| --- | --- | --- | --- |
| `avgblur.cl` | 2 | 2 | 8/8 |
| `colorkey.cl` | 2 | 2 | 6/6 |
| `convolution.cl` | 4 | 4 | 16/16 |
| `deshake.cl` | 8 | 0 | 0/0 |
| `neighbor.cl` | 2 | 2 | 8/8 |
| `nlmeans.cl` | 4 | 2 | 6/6 |
| `overlay.cl` | 3 | 1 | 4/4 |
| `pad.cl` | 1 | 1 | 4/4 |
| `remap.cl` | 2 | 0 | 0/0 |
| `tonemap.cl` | 1 | 0 | 0/0 |
| `transpose.cl` | 1 | 1 | 4/4 |
| `unsharp.cl` | 2 | 1 | 4/4 |
| `xfade.cl` | 9 | 4 | 15/15 |
| **total** | **41** | **20** | **75/75** |

**20 of 41 kernels, 49%.** This is reach, not correctness, and the other 21 were
not shown to fail. Eleven have no filter chain in the harness at all: `deshake`
needs real motion, `remap` three inputs, `tonemap` HDR input. Ten are
alternatives a different filter option would select. Counting only kernels the
harness can drive, it is 20 of 30, **67%**.

**75 of 75 locals resolved, 100%**, at those twenty stops. This says a value was
produced, not that it was right, which is what the next section is for.

## Correctness

### Consistency is not proof

The obvious way to present a debugger is a table of its output that hangs
together: the loop counter climbs the way the source says it should, one
variable agrees with another. That is worth nothing to a reader who already
doubts the tool. A debugger reading the wrong work-item, or stopping one
iteration late, produces a table just as tidy.

Correctness needs an oracle that never consults the debugger. Two are used
below.

### Oracle 1 — the frame FFmpeg writes

`avgblur_vert` is the pass that writes the result:

```c
write_imagef(dst, loc, acc / count);
```

If the `acc` and `count` the debugger reports are the kernel's real values, the
output byte at that pixel must be `acc.x / count` scaled to 8 bits. FFmpeg was
run again with no debugger, writing raw `yuv420p` to a file.

| work-item | `acc.x` | `count` | predicted byte | FFmpeg's output | |
| --- | --- | --- | --- | --- | --- |
| (0,0) | 0.250980 | 4 | 16 | 16 | match |
| (1,0) | 0.454902 | 4 | 29 | 29 | match |
| (2,0) | 0.596078 | 4 | 38 | 38 | match |
| (3,0) | 0.690196 | 4 | 44 | 44 | match |
| (4,0) | 0.882353 | 4 | 56 | 56 | match |
| (5,0) | 1.156860 | 4 | 74 | 74 | match |

Six values read out of a running kernel, each predicting a byte produced by a
path with no debugger in it. The six predictions are distinct, so this also
pins the work-item identities: had the debugger mislabelled work-item 5 as 3, it
would have read work-item 3's `acc` and been compared against byte 5, and 44
against 74 is a mismatch.

#### The check is not vacuous

It failed first. The oracle was originally built on the horizontal pass, on the
assumption that `sizeY=0` left the vertical pass an identity. Four pixels
matched and two did not. The cause was the oracle, not the debugger:

```c
/* vf_avgblur_opencl.c */
if (s->radiusV <= 0) {
    s->radiusV = s->radiusH;
}
```

`sizeY=0` blurs vertically with the horizontal radius, so the final output is
not the horizontal pass's result; the two mismatches were pixels whose vertical
neighbourhood was not uniform and the four matches were luck. Reading the
output-writing kernel instead makes all six agree. A check that cannot fail
proves nothing, and this one demonstrably can.

### Oracle 2 — two backends, one kernel

The output oracle covers one backend. The second check reads the same kernel
with two mechanisms sharing nothing but the `OCLWorkItem` type:

- **pocl backend** — stops a host thread with ptrace, decodes DWARF against
  machine registers.
- **Oclgrind backend** — halts an LLVM IR interpreter from a plugin inside the
  host process.

`tools/ffmpeg_validation/cross_backend.py` runs a case on both and pairs stops
**by order, not by work-item label**, because whether the label is right is one
of the things under test.

Across six cases, on the code as it stood before the fixes below:

| | result |
| --- | --- |
| comparable readings | 9 |
| agreed | **9** |
| differed | 0 |
| readable on one backend only | 21 |
| stops paired | 12 |
| same work-item id | 4 |

Two figures pointing opposite ways, which is sharper than either alone: the
backends agreed on **every** value they could both read, and disagreed about
which work-item those values belonged to two times in three. Values right,
identity wrong -- and the identity half is what the fixes below address.

A first version of this comparison reported a spurious disagreement because it
paired stops by work-item label, which is precisely the thing that was broken.
Pairing by order is what made the split visible.

Re-running it against the fixed code is how the third fix was confirmed from
the outside: `avgblur_vert`, which the pocl backend had never once stopped in,
pairs three stops against Oclgrind's three.

| case | before | after |
| --- | --- | --- |
| `avgblur_horiz` | 3 / 3 | 3 / 3 |
| `avgblur_vert` | 3 / **0** | 3 / **3** |
| `sobel_global` | 3 / 3 | 3 / 3 |

The harness needed a change of its own to read those runs. Its parser matched
`WI(` followed by three numbers, so once a backend began reporting `WI(?)` for
a work-item it could not identify -- the first fix -- every stop became
invisible to it and the comparison silently found nothing. A measuring
instrument that assumes the old answer is worth checking after changing what is
measured.

### What the differential found

It was not a formality. It turned up three defects in the pocl backend, none of
them visible to the debugger's own suite, because that suite's kernels are
written with the names the backend looks for.

1. **An unidentified work-item was reported as the origin.** `read_local_id`
   ended by setting `{0,0,0}` and returning success, so a frame that identified
   nothing produced `WI(0,0,0)` as confidently as one that did. Three
   consecutive stops holding three different pixels all claimed work-item zero.
   *Fixed:* the work-item now reads `WI(?)`, and the variables are still shown,
   because they are correct and are what the stop was for.

2. **The y and z axes were fabricated.** Every source the backend has for a
   local id names the x axis alone, yet both completed the coordinate with
   `y = 0, z = 0`, so a work-group deeper than one element reported one
   work-item for a whole column. A kernel's own `gy` and `gz` cannot fill the
   gap: LLVM gives both the same location, `DW_OP_breg2 RCX+0,
   DW_OP_stack_value`, with no range saying where it holds, so reading them
   returns whatever that register contains -- 50, for a kernel whose y never
   exceeds 7. *Fixed:* the work-item is named only when the work-group is one
   element wide in y and z.

   That 50 is worth separating out. It is not a fault in the expression
   evaluator, which handles `DW_OP_stack_value` correctly and reports what the
   debug info claims. The debug info is what is wrong, and nothing in the
   debugger can repair it -- which is why the fix is to stop answering rather
   than to read harder.

3. **Only the first kernel of a multi-kernel program was reachable.** pocl
   compiles each kernel into its own module and the DWARF model returned early
   once any one was loaded, so a breakpoint on a line belonging to the second
   kernel was looked up in the first kernel's debug info and silently never set.
   A breakpoint in `avgblur_horiz` worked and the same request for
   `avgblur_vert` did nothing, which reads as the kernel never running.
   *Fixed:* each kernel module is asked in turn until one knows the line.

A fourth asymmetry is not a defect. FFmpeg picks `unsharp_local` or
`unsharp_global` according to how much local memory the device reports, so the
two backends genuinely run different kernels there. A differential surfaces that
identically to a bug, and only reading the filter's source separates them --
worth remembering before treating any single disagreement as a defect.

### Limits of the evidence

- Six work-items of the output oracle, all with `y = 0`, so the y axis was never
  varied there.
- One filter of the twenty for the oracle; six cases for the differential.
- Stepping, work-item selection and memory reads have no correctness evidence of
  any kind.
- "Correct at these points" is the claim. "The debugger is correct" is not.

## Efficiency

One 64x64 frame through `avgblur_opencl`, wall clock:

| configuration | time | |
| --- | --- | --- |
| pocl, cold kernel cache | 2.08 s | dominated by JIT compilation |
| pocl, warm kernel cache | 0.50 s | the honest baseline |
| Oclgrind | 0.14 s | interprets, nothing to compile |
| **ocldbg + Oclgrind**, 3 stops | **0.26 s** | 1.9x over its own baseline |
| **ocldbg + pocl**, 3 stops | **36.12 s** | 72x over its own baseline |

Two results here are worth stating plainly.

**Oclgrind is faster than pocl for a single frame.** The expectation is that an
interpreter must be slower than a JIT, and for sustained work it is; for one
frame pocl spends its time compiling, and Oclgrind has nothing to compile. Any
worry that a simulator is too slow to debug real video is misplaced at the frame
counts a debugging session uses.

**The debugger's cost differs by two orders of magnitude between backends, and
the reason is architectural.** The Oclgrind backend halts the interpreter from
inside the host process and only stops where a breakpoint actually is. The pocl
backend stops the process at every work-group dispatch to record it. FFmpeg's
NDRange here is 64x64 with a local size of (1,1,1), which is 4096 work-groups
and so 4096 ptrace stops, about 8.7 ms each. The cost scales with the number of
work-groups, not with the number of breakpoint hits, and OpenCL work dispatched
one work-item per group is the worst case for it.

Oclgrind's own cost scales with the pixels it interprets, undramatically:

| frame | one frame, no debugger |
| --- | --- |
| 32x32 | 0.15 s |
| 128x128 | 0.25 s |
| 256x256 | 0.52 s |
| 480x480 | 1.41 s |

### Choosing a backend

For a real-world application the two are not interchangeable. Oclgrind costs
little, knows each work-item's identity exactly because the interpreter tracks
it, and needs no `-g` from the application. The pocl backend runs the kernel as
real machine code, which is what you want when the question is about codegen,
but it needs the application to build with debug info, its per-dispatch cost is
severe on fine-grained NDRanges, and its work-item identification depends on
debug info that is not always there.

## Reproducing

```bash
# Coverage across every FFmpeg OpenCL kernel
python3 tools/ffmpeg_validation/sweep.py

# The same kernel read by both backends
python3 tools/ffmpeg_validation/cross_backend.py tools/ffmpeg_validation/cases.json
```

Both drive `/usr/bin/ffmpeg` and write their raw results as JSON beside the
logs. Run them on an otherwise idle machine: the pocl side is slow enough that
competing load changes how many stops complete within the timeout.

## What this does not show

- **Breakpoints and variable inspection only.** The Oclgrind command line is
  batch: `--break-at`, `--break-for`, `--print`. `step_over` and `step_in` exist
  on the backend and in the plugin but have no command-line surface, so no
  stepping was demonstrated on it.
- **Breakpoints are addressed by line, not by file**, an Oclgrind program being
  built from a single source string. That is what lets one line select between
  the two kernels of `avgblur`, and it would not be enough for a filter
  assembled from several sources.
- **No work-item selection was exercised.** The stops arrive in the
  interpreter's own order rather than being chosen.
- **Variables are not yet available over DAP on the Oclgrind backend.**
  Resolving them needs the DWARF a kernel module carries, and Oclgrind
  interprets IR with no such module on disk.
- **The `ocl` command family is CPU-backend only.** `ocl break`, `ocl wi` and
  `ocl p` reach `DebuggerContext`'s LLDB internals and cannot address an
  Oclgrind session at all.
