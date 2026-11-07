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
* Outside a slow-mode process, sr's ratios repeat within 0.1% from process to process, and the wavefront's per-kernel ratios within about 3%.
  So a difference under those is noise.

**Size** is [tools/native-parity.py](../tools/native-parity.py), over every entry point of the real shaders:

```bash
uv run dev.py build -t sgl
uv run libs/graphics/shaped-graphics-language/tools/native-parity.py table
uv run libs/graphics/shaped-graphics-language/tools/native-parity.py diff <file.sgl> <entry> <cs|vs|ps|lib>
```

`table` is the table below, and `diff` one entry's opcode histograms side by side, with both binaries and listings kept.
DXC gets slib's own arguments, so its side is what a context of the text route builds.
An operation is one instruction inside the function, so declarations, names and decorations do not count.

## Runtime

Native over DXC, GPU time on the Arc B580, the median of three processes; best and worst where they differ by more than noise.

| kernel | dx12 before | dx12 after | vulkan before | vulkan after |
|---|---:|---:|---:|---:|
| à-trous | 0.985 | 0.985 | 0.999 | 1.000 |
| SVGF filter | 0.989 | 0.989 | 1.003 | 1.001 |
| convolution | 1.006 | 1.006 | 0.936 | 0.936 |
| megakernel | 0.99 | 0.93 | 0.84 | 0.86 |
| wavefront, whole frame | 1.08 | 1.05 | 0.79 | 0.82 |
| wavefront begin | 1.01 | 1.01 | 1.00 | 1.00 |
| wavefront extend | 1.02 | 1.00 | 0.46 | 0.50 |
| wavefront segment | 1.01 | 0.99 | 0.52 | 0.55 |
| wavefront sort | 1.00 | 0.99 | 0.56 | 0.55 |
| wavefront shade | 1.18 | **1.20** | 1.06 | **1.09** |
| wavefront connect | 1.26 | 0.96 | 1.13 | **1.16** |
| wavefront finalize | 1.05 | **1.05** | 1.10 | **1.15** |
| wavefront arguments | 1.23 | **1.09** | 0.94 | 1.01 |
| wavefront resolve | 0.98 | 1.00 | 0.95 | 1.00 |

"Before" is main at 35d0f1dba on dx12; on vulkan it already has the first fix below, which was in before the first vulkan run.
Native runs vulkan's extend, segment and sort in half the time both DXC routes take, and that stood before this work.
A difference that large is a question about what the two modules trace rather than a win, and it is open.
The bold ratios are what remains outside ±3%, under [what remains](#what-remains).

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
| `bc7_cs` | dxil | 1.49 | 1.49 |
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
  DXC unrolls a short loop of constant bounds and promotes the array; native unrolls only what `@unroll` asks for.
  The viewer's environment loops now say `@unroll`, which took path_miss's DXIL from 1209 operations to 412, DXC's being 397.
  The compiler choosing that itself is [perf-sgl-native-constant-loops-not-unrolled](issues/perf-sgl-native-constant-loops-not-unrolled.md).

## What remains

* **The shade kernel, 1.20 on dx12 and 1.09 on vulkan.**
  Its DXIL still holds about 420 more `fmul`, 126 more `fdiv` and 86 more `dot3` than DXC's.
  The shape says values DXC shares across branches or rewrites, such as `x / c` as `x * (1 / c)`, which native computes again.
  Matching the two listings function by function is the next step; nothing measured so far is a cause.
* **connect on vulkan, 1.16.**
  Its SPIR-V holds 171 phis against DXC's 78, and no `OpSelect` against DXC's 14.
  DXC's SPIR-V goes through spirv-opt, which turns a small if-else into a select, and native SPIR-V never does: `flatten_branches` is DXIL's alone.
  On dx12, where both routes get selects, connect is 0.96, so a structured if-conversion is the lead.
* **finalize on vulkan, 1.15.**
  Its SPIR-V is at parity, 327 operations against 314, so nothing the counts show is the cause, and it is open.
* **finalize and arguments on dx12, 1.05 and 1.09.**
  These are a few short dispatches per frame, under 4 ms of 24 frames, so a fixed cost per dispatch shows as a ratio; their code is at parity or below.
* **bc7's DXIL, 1.49 in size.**
  This is the unrolling above, in sr's own compression shader: 67 allocas against DXC's 27, and 1156 stores against 460.
  It is not timed here.
