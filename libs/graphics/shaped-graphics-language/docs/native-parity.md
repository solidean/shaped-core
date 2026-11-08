# Native parity

SGL's own SPIR-V and DXIL against the same SGL's text through DXC, on one device: what the native route is held to, and what is left.
Back to [architecture](architecture.md#the-native-layer).

The bar is runtime within ±3% of DXC's on every shader both routes build, or each exception named with its cause.
Size is not the bar, since a driver compiles a module again, but a size gap nobody can explain is a lead, and each one here is explained.

## How to measure

**Runtime** is two benchmarks, each pairing one SGL source on its two routes, alternating dispatch by dispatch or frame by frame:

```bash
uv run dev.py benchmark "SGL's own binary against its text through DXC" --timeout 170
```

* `sr - ...` dispatches à-trous, SVGF's filter and the network's convolution, from [sgl-against-hlsl-benchmark.cc](../../shaped-rendering/tests/bench/sgl-against-hlsl-benchmark.cc).
* `sv - the tracers ...` runs the megakernel and the wavefront on the cornell box, from [native-route-benchmark.cc](../../shaped-viewer/tests/native-route-benchmark.cc).
  It gives every wavefront kernel its own ratio, and times a frame only once the compiles a frame starts have settled.
  A material's shade kernel is still compiling for the first frames, and shades with the stand-in until it lands.

Both open no window.
Three effects of the B580 decide how to read them:

* A newly loaded binary runs in a driver's cold tier, up to 16% slow, for its first dispatches; both benchmarks run past it before timing.
* A process lands in one of two speed modes about 1.5 times apart, which moves both routes alike but can move a ratio by 10% in one process.
  So a ratio is read across at least three processes, and one that only one process shows is that process's mode.
* Outside a slow-mode process, sr's ratios repeat within 0.1% from process to process, and a whole frame's within about 3%.
* A wavefront kernel's own ratio is where its timestamps land among stages the GPU overlaps, not what the kernel costs.
  Between two sets of three processes, vulkan's finalize read 1.15 and then 3.17, and its connect 1.16 and then 0.36, while the frame's total moved by 3%.
  So the bar is read on a frame and on a single dispatch, and a kernel's ratio is a pointer for where to look.

**Size** is [tools/native-parity.py](../tools/native-parity.py), over every entry point of the real shaders:

```bash
uv run dev.py build -t sgl
uv run libs/graphics/shaped-graphics-language/tools/native-parity.py table
uv run libs/graphics/shaped-graphics-language/tools/native-parity.py diff <file.sgl> <entry> <cs|vs|ps|lib>
```

`table` is the table below, and `diff` one entry's opcode histograms side by side, with both binaries and listings kept.
`check` counts SGL's own operations alone, in about a second, against [native-size-baseline.json](../tools/native-size-baseline.json).
An entry that grows more than 2% fails `dev.py check`'s `sgl-native-size` gate, and `update` commits a new baseline where the growth is meant or an entry shrank.
DXC gets slib's own arguments, so its side is what a context of the text route builds.
An operation is one instruction inside the function, so declarations, names and decorations do not count.

## Runtime

Native over DXC, GPU time on the Arc B580: the median of three processes, in each of two sets taken apart.

| what | dx12 before | dx12 after | vulkan before | vulkan after |
|---|---:|---:|---:|---:|
| à-trous, one dispatch | 0.985 | 0.985, 0.984 | 0.999 | 1.000, 0.999 |
| SVGF filter, one dispatch | 0.989 | 0.989, 0.988 | 1.003 | 1.001, 1.001 |
| convolution, one dispatch | 1.006 | 1.006, 1.006 | 0.936 | 0.936, 0.935 |
| megakernel, one frame | 0.99 | 0.93, 0.93 | 0.84 | 0.86, 0.88 |
| wavefront, one frame | 1.08 | **1.05, 1.08** | 0.79 | 0.82, 0.79 |

Of the wavefront's kernels on dx12, only shade read high in both sets, 1.20 and 1.19, and connect went from 1.26 before to 0.96 and 1.02.

The same benchmarks on the Radeon iGPU of a Ryzen 9 9950X3D2 (RDNA 2, driver 32.0.21042.62), `SC_GPU=amd`, the median of three processes:

| what | dx12 | vulkan |
|---|---:|---:|
| à-trous, one dispatch | 0.997 | 1.000 |
| SVGF filter, one dispatch | 1.011 | 1.001 |
| convolution, one dispatch | **1.032** | 0.984 |
| megakernel, one frame | 0.842 | 1.029 |
| wavefront, one frame | 1.019 | 0.890 |

Every ratio repeated to the third decimal in all three processes, so the AMD GPU shows none of the B580's speed modes.
The convolution on dx12 is just past the bar, the one exception there, and the megakernel on vulkan just inside it.
Neither has been diagnosed; the convolution's opposite sign on vulkan says the gap is the DXIL rather than the kernel.

**Both benchmarks need the text route asked for by name.**
Once the native route became each context's default, a context left at its defaults ran SGL's binary on both sides, and every ratio read 1.000.
The AMD numbers above are from after that was fixed; the B580's are from before the default changed.

"Before" is main at 35d0f1dba on dx12; on vulkan it already has the first fix below, which was in before the first vulkan run.
Native's vulkan frame is a fifth faster than DXC's, and its extend, segment and sort read under half of DXC's in both sets.
A difference that large is a question about what the two modules trace rather than a win, and it is open.
The bold ratio is what remains outside ±3%, under [what remains](#what-remains).

## Size

Native over DXC in operations, for the entries whose gap was large or remains; `table` prints all 106.

| entry | target | before | after |
|---|---|---:|---:|
| à-trous `main_cs` | spirv | 1.32 | 1.02 |
| viewer `path_trace` | spirv | 63,248 bytes against 42,744 | 51,628 bytes, 1.21 |
| viewer `path_trace` | dxil | 1.25 | 0.98 |
| viewer `path_miss` | dxil | 1.99 | 1.04 |
| wavefront `extend` | dxil | 1.21 | 1.04 |
| wavefront `segment` | dxil | 1.42 | 0.99 |
| wavefront `connect` | dxil | 1.22 | 1.05 |
| wavefront `connect` | spirv | 1.14 | 1.14 |
| a material's shade kernel | dxil | 1.96 | 1.21 |
| a material's shade kernel | spirv | 1.88 | 1.39 |
| raster `environment_ps` | dxil | 1.96 | 1.02 |
| raster `cull_second` | dxil | 1.19 | 1.16 |
| `bc7_cs` | dxil | 1.49 | 1.27 |
| bloom `first_cs` | dxil | 1.21 | 1.03 |
| `bc3_cs` | spirv | 1.30 | 1.27 |

The shade kernel is generated per material at run time, so `table` does not hold it; the row is the cornell box's `pbr`.
Every other entry stands within about 10% of DXC's operations, and most below it.

DXIL's bytes run 5% to 20% over DXC's even where the operations match, imgui's being 30 against 30.
That is the container rather than the code: DXC's side is stripped of reflection and debug parts here, and the gap does not grow with the code.

## What each gap was

* **SPIR-V ran no pass.**
  DXIL's route ran five after `construct_ssa` and SPIR-V's none, so nothing folded, shared or swept on SPIR-V.
  `simplify` gained a structured mode, and SPIR-V runs it with `share_common_values`, `remove_unused` and `chain_sums`.
* **SSA promotes a vector lane by lane.**
  So each merge rebuilt the vector with a construct and took it apart with extracts, which was most of à-trous' 107 extra operations.
  `regroup_lanes` makes one phi of the vector where a way in hands one over, and `simplify` folds a construct of one vector's lanes back to it.
* **A raw buffer's address was a word index multiplied back.**
  A `bytes` load reached the DXIL writer as `offset >> 2`, which it multiplied by 4 again per lane.
  It now takes the offset as the byte address HLSL's `Load` takes, which EVAL-97 allows, and gathers constant additions into one base.
  `merge_buffer_loads` then makes one load of up to four lanes of neighbouring loads, as DXC does: connect went from 1.25 to 0.96.
* **Constants never folded through a vector or a builtin.**
  A material leaves most of openpbr's lobes at constants, and `simplify` folded scalar arithmetic only, so they all ran.
  It now folds known float lanes, and builtins at `.fast` through `soft_math`, as the interpreter does; and at `.fast`, `x * 0`, `x + 0` and their kin.
* **A known branch stayed on SPIR-V.**
  The structured mode folds a selection's known condition to a branch, and keeps the other side as a label and `OpUnreachable` for any merge naming it.
* **A local array a loop indexes stayed in memory.**
  The viewer's environment loops now say `@unroll`, which took path_miss's DXIL from 1209 operations to 412, DXC's being 397.
  The compiler unrolling such loops itself was tried, and grew every entry it touched: DXC keeps these loops.
  [perf-sgl-native-constant-loops-not-unrolled](issues/.closed/2026/11/perf-sgl-native-constant-loops-not-unrolled.md) holds the measurements.
* **A table of constants was memory each invocation filled.**
  A `let` of constants an index reaches at run time stayed a local, which DXIL fills with a store per element at every use.
  bc7 inlines dozens of them, its weight and partition tables: 1171 stores against DXC's 460.
  `state_constant_tables` makes each one a table: an internal constant on DXIL and a `Private` variable on SPIR-V, one per set of leaves.
  bc7's DXIL went from 1.49 to 1.27 and bloom's from 1.21 to 1.03, and nothing grew.

## What remains

* **dx12's wavefront frame, 1.05 to 1.08.**
  Its shade kernel is the one that reads high in every set, 1.17 to 1.20, and its DXIL is 1.21 times DXC's in operations.
  That DXIL still holds about 420 more `fmul`, 126 more `fdiv` and 86 more `dot3` than DXC's.
  The shape says values DXC shares across branches or rewrites, such as `x / c` as `x * (1 / c)`, which native computes again.
  Matching the two listings function by function is the next step; nothing measured so far is a cause.
* **SPIR-V turns no if-else into a select.**
  DXC's SPIR-V goes through spirv-opt, which does, and `flatten_branches` is DXIL's alone: connect's SPIR-V holds 171 phis and no `OpSelect` against DXC's 78 and 14.
  vulkan's frame is faster than DXC's regardless, so this is a size gap, and a lead should a vulkan kernel come in slow.
* **bc7's DXIL, 1.27 in size.**
  Its stores are DXC's now; what is left is 195 more `fdiv`, 234 more `select` and 242 more `load` than DXC's.
  The `fdiv`s are the wavefront's shape again, a division by a constant DXC writes as a multiplication.
  [perf-sgl-dxil-bc7-arithmetic-gap](issues/perf-sgl-dxil-bc7-arithmetic-gap.md) has the rest; it is not timed here.
