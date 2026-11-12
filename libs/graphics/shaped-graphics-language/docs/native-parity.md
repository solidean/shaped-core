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
* `sr - ..., the kernels with no HLSL original` dispatches the BC encoders, bloom's four passes, the box filter's 2D level, the exposure meter and SVGF's temporal and variance passes,
  from [sgl-routes-benchmark.cc](../../shaped-rendering/tests/bench/sgl-routes-benchmark.cc).
* `sv - the tracers ...` runs the megakernel and the wavefront on the cornell box, from [native-route-benchmark.cc](../../shaped-viewer/tests/native-route-benchmark.cc).
  It gives every wavefront kernel its own ratio, and times a frame only once the compiles a frame starts have settled.
  A material's shade kernel is still compiling for the first frames, and shades with the stand-in until it lands.

Both open no window.
Three effects of the B580 decide how to read them:

* A newly loaded binary runs in a driver's cold tier, up to 16% slow, for its first dispatches; both benchmarks run past it before timing.
  The first process after the compiler changed can read far worse than that, past the warm-up, with the driver's caches cold.
  On 2026-11-08 one such process read the BC encoders at 1.5 to 3.1 on the B580's dx12, and the two after it at 0.67 to 0.88; a ratio a change moves that far is read again.
* A process lands in one of two speed modes about 1.5 times apart, which moves both routes alike but can move a ratio by 10% in one process.
  So a ratio is read across at least three processes, and one that only one process shows is that process's mode.
* Outside a slow-mode process, sr's ratios repeat within 0.1% from process to process, and a whole frame's within about 3%.
* A wavefront kernel's own ratio is where its timestamps land among stages the GPU overlaps, not what the kernel costs.
  Between two sets of three processes, vulkan's finalize read 1.15 and then 3.17, and its connect 1.16 and then 0.36, while the frame's total moved by 3%.
  So the bar is read on a frame and on a single dispatch, and a kernel's ratio is a pointer for where to look.

**The driver's own figures** are what the GPU runs rather than what it was handed, and the better measure of size on vulkan.
AMD reports registers (`numUsedVgprs`, `numUsedSgprs`) and Intel machine instructions and blocks; dx12 reports none.

```bash
uv run dev.py build -t pipeline-stats
uv run libs/graphics/shaped-graphics-language/tools/native-parity.py driver [filter]   # SC_GPU=amd for the Radeon
```

`driver` builds every compute entry point's pipeline on both SPIR-V routes, on a context that captures the figures, and prints the ones that say what code costs.
The vulkan benchmark of `sr` logs the same for its three kernels.
On 2026-11-08, after the SPIR-V passes below, the entries where the two routes part:

| kernel | B580 instructions | Radeon vector registers |
|---|---:|---:|
| wavefront `begin` | 593, 749 | 61, 71 |
| wavefront `extend` | 3555, 3827 | **112, 96** |
| wavefront `connect` | 2330, 2497 | **109, 96** |
| wavefront `finalize` | 481, 455 | 61, 64 |
| wavefront `sort` | 65, 52 | the same |
| wavefront `args_shade` | 379, 302 | the same |
| the shade kernel | 8602, 9086 | **191, 168** |
| `bc7_cs` | 8253, 9571 | the same |
| `bc1_cs` | 3095, 2951 | the same |
| SVGF's filter | 2010, 2005 | 34, 32 |

Native first, DXC second.
Most of the 41 compute entries compile to within 1% of DXC's instructions on the B580 and to the same registers on the Radeon, whatever their modules' sizes.
Each driver cleans up what a module leaves, which is why a module's operations overstate a gap.

**shan's compute cost** estimates what dx12 has no figures for: `uv run dev.py run shan -- diff` names it `compute_cost`.
It weighs each operation's lanes by its class's rate, as often as a wave runs it, and leaves memory out.
On 2026-11-08 it put the wavefront's `extend` and `connect` at 1.08 and 1.07 of DXC's on DXIL and 0.99 on SPIR-V, beside the dx12 frame's 1.03 to 1.06 and vulkan's faster one.
It also put bc1 at 1.33 and 1.39 on the two formats, which no benchmark times, and the convolution below DXC's on both.
bc1's was ten integer multiplies by 4 a loop, which DXC writes as shifts; `simplify` and the DXIL builder now do too, and shan counts one as a shift, as every driver runs it.
What is left of bc1's, 1.19 and 1.25, is the overhead of four loops DXC unrolls and native keeps, which the B580's driver unrolls as well.

Its rates are measured: `uv run dev.py run pipeline-stats -- throughput` times a kernel per operation and prints each against an fma.

| operation | B580 | Radeon |
|---|---:|---:|
| sqrt, exp2, a float division | 3.2 | 3.3 |
| sin | 7.3 | 4.7 |
| integer multiply | 7.5 | 4.4 |
| integer division | 18 | 23 |
| conversion | 0.85 | 1.6 |
| an add after a multiply | 0 | 0 |

Against the benchmarks it orders the convolution's vulkan gap right, 0.90 for the B580's 0.935, and puts the other kernels within a few percent of 1.0, where they measure.
It does not see the Radeon's dx12 convolution at 1.03, which is not arithmetic.
A kernel that waits on memory is nearer 1.0 than the model says, since the model leaves memory out.
The bold rows are where native costs more: a wave holding more vector registers leaves room for fewer waves beside it.

`pipeline-stats compute ... --isa <dir>` writes the driver's own forms of each route's code there, which on the Radeon is the ISA.
Its listing of `extend` has the same memory instructions on both routes, near enough the same instructions, and the same traversal loop.
So the 16 registers are where values live rather than what runs.
Which region of the 2500 instructions holds them takes a liveness count over the ISA, which nothing does yet; the Radeon's vulkan frame is 0.89 of DXC's whatever it is.

**shan's live words are no register count.**
`uv run dev.py run shan -- diff` on the binaries `diff` leaves splits them into vector words, values that differ across a wave, and scalar words, values it shares.
Against AMD's vector registers on 2026-11-08, native and DXC's SPIR-V:

| kernel | vector words | `numUsedVgprs` |
|---|---:|---:|
| à-trous | 31, 36 | 30, 30 |
| SVGF's filter | 28, 30 | 34, 32 |
| convolution | 79, 82 | 38, 38 |

The driver schedules before it allocates, so live words in the module's own order say where two modules part, not what either will occupy.
The convolution's unrolled loads are the clearest case: half the words, on both routes alike.

**Size** is [tools/native-parity.py](../tools/native-parity.py), over every entry point of the real shaders:

```bash
uv run dev.py build -t sgl
uv run libs/graphics/shaped-graphics-language/tools/native-parity.py table
uv run libs/graphics/shaped-graphics-language/tools/native-parity.py diff <file.sgl> <entry> <cs|vs|ps|lib>
```

`table` is the table below, and `diff` one entry's opcode histograms side by side, with both binaries and listings kept.
`lines` is one entry's DXIL arithmetic per `.sgl` file and line on both routes, which says where a gap stands.
`check` counts SGL's own operations alone, in about a second, against [native-size-baseline.json](../tools/native-size-baseline.json).
An entry that grows more than 2% fails `dev.py check`'s `sgl-native-size` gate, and `update` commits a new baseline where the growth is meant or an entry shrank.
The gate holds the `pbr` material's shade kernel too, from a copy the program would generate: [pbr_shade_kernel.sgl](../../shaped-viewer/tests/data/pbr_shade_kernel.sgl).
A viewer test fails once the generator writes something else, and `SC_UPDATE_NATIVE_SAMPLES=1` on that test writes the copy anew.
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
After the constant phis below, on 2026-11-08, the dx12 frame read 1.032, 1.034 and 1.037, and vulkan's 0.80, 0.80 and 0.80.
Once SPIR-V's branches became selects, vulkan's megakernel read 0.79 and its wavefront 0.76 in one process, and sr's dispatches what they read before.

The same benchmarks on the Radeon iGPU of a Ryzen 9 9950X3D2 (RDNA 2, driver 32.0.21042.62), `SC_GPU=amd`, the median of three processes:

| what | dx12 | vulkan |
|---|---:|---:|
| à-trous, one dispatch | 0.997 | 1.000 |
| SVGF filter, one dispatch | 1.011 | 1.001 |
| convolution, one dispatch | **1.032** | 0.984 |
| megakernel, one frame | 0.842 | 1.029 |
| wavefront, one frame | 1.019 | 0.890 |

Every ratio repeated to the third decimal in all three processes, so the AMD GPU shows none of the B580's speed modes.

**The BC encoders** joined the `sr` benchmark on 2026-11-08, text against native only, since they have no HLSL original.
Each encodes one 1024x1024 image; native over text, two clean processes on the B580 and one on the Radeon:

| encoder | B580 dx12 | B580 vulkan | Radeon dx12 | Radeon vulkan |
|---|---:|---:|---:|---:|
| bc1 | 0.90, 0.90 | 0.98, 0.99 | 0.98 | 1.00 |
| bc3 | 0.84, 0.84 | 0.93, 0.96 | 1.00 | 1.08 |
| bc4 | 0.80, 0.80 | 1.00, 1.02 | 1.01 | 1.00 |
| bc5 | 0.82, 0.82 | 1.00, 0.98 | 1.00 | 1.00 |
| bc7 | 0.61, 0.66 | 0.59, 0.60 | **1.05** | 1.01 |

Before that, bc1 read 1.63 on the B580's vulkan, from a cost no module count showed: the driver's scratch memory, 19 200 bytes against DXC's 8 192.
A local array indexed by a loop's counter lives in scratch on the B580, where the Radeon keeps it in registers on either route.
The texel loops of every encoder but bc7's now say `@unroll`, so each index is a constant and the arrays are registers: scratch went to 0 for bc1, bc4 and bc5.
bc1 went from 0.22 ms to 0.048 natively on vulkan, and bc4 and bc5 halved on dx12; the Radeon moved within a few percent.
bc7's stay loops: unrolled, its native was 22% faster on vulkan and 6% slower on dx12, while DXC's vulkan module took 586 kB of scratch and 5 times as long.
bc7's native is the faster route on the B580 anyway, from half of DXC's scratch, 27 520 bytes against 55 424.

**The rest of sr's kernels** joined them the same day, each at the size its routine runs it at for a 1024x1024 image.
Native over text, one process on the B580 and on the Radeon's dx12, two on the Radeon's vulkan:

| kernel | B580 dx12 | B580 vulkan | Radeon dx12 | Radeon vulkan |
|---|---:|---:|---:|---:|
| bloom first | 0.99 | 1.00 | 1.01 | 1.01, 1.01 |
| bloom down | 1.00 | 0.90 | 1.00 | 1.00, 1.00 |
| bloom up | 0.99 | 1.00 | 1.00 | 1.00, 1.00 |
| bloom finish | 1.00 | 1.00 | 1.00 | **1.05, 1.05** |
| mipmap 2d | 1.01 | 1.01 | 1.00 | 1.00, 1.00 |
| exposure meter | 1.03 | 1.02 | 1.00 | 1.00, 1.00 |
| SVGF temporal | 1.00 | 1.00 | 1.00 | 1.00, 0.99 |
| SVGF variance | 0.95 | 1.00 | 1.01 | 1.00, 1.00 |

bloom down is a 10 µs dispatch and the exposure meter one group of 256 threads, so their B580 ratios are the timestamps' resolution; each one's fastest dispatch is even.
The B580 compiles both routes of every kernel here to the same instruction count, but SVGF temporal's 173 against 175, and none uses scratch.
The convolution on dx12 is just past the bar, the one exception there, and the megakernel on vulkan just inside it.
Once SPIR-V's branches became selects and its vectors one phi a merge, vulkan's megakernel read 1.004 and its wavefront 0.890 in one process.
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
| à-trous `main_cs` | spirv | 1.32 | 0.93 |
| viewer `path_trace` | spirv | 63,248 bytes against 42,744 | 0.90 |
| viewer `path_trace` | dxil | 1.25 | 0.98 |
| viewer `path_miss` | dxil | 1.99 | 1.04 |
| wavefront `extend` | dxil | 1.21 | 1.04 |
| wavefront `segment` | dxil | 1.42 | 0.99 |
| wavefront `connect` | dxil | 1.22 | 1.05 |
| wavefront `connect` | spirv | 1.14 | 1.01 |
| a material's shade kernel | dxil | 1.96 | 1.03 |
| a material's shade kernel | spirv | 1.88 | 0.99 |
| raster `environment_ps` | dxil | 1.96 | 1.02 |
| raster `cull_second` | dxil | 1.19 | 1.16 |
| `bc7_cs` | dxil | 1.49 | 1.23 |
| bloom `first_cs` | dxil | 1.21 | 1.03 |
| `bc3_cs` | spirv | 1.30 | 1.07 |

The shade kernel is generated per material at run time; the row is the cornell box's `pbr`, whose committed copy `table` holds.
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
* **A phi of two equal constants stayed a phi.**
  openpbr's `bsdf_lobe_probs` returns its struct from two places, and for a material with no transmission both leave `transmission` at zero.
  One zero was a null constant and the other a folded `0.0`, so `simplify` saw two values, and every branch on `p.transmission` stayed.
  Counting operations per `.sgl` line on both sides showed it: DXC had none on the refraction lines, native 64 on one of them.
  `simplify` now takes constants of the same bits as one value, which took the shade kernel's DXIL from 1.21 to 1.08 of DXC's.
  `native-parity.py lines` is that count: it compiles DXC's side from HLSL stripped of its `#line`s with `-Zi`, since DXC keeps one file name for all of them, and maps each line back.
* **A branch already decided, and a vector compared with a constant, stayed.**
  With no coat, openpbr's `else if pick < p.fuzz + p.coat` is the condition its `if` already found false, which `simplify` now follows down the dominator tree.
  `any` and `all` of a comparison known in every lane fold, so `s.geometry_coat_normal != float3(0.0, 0.0, 1.0)` of the default normal goes.
  `simplify` also reads every scalar constant through one entry per type and bits, which a phi, a select and a condition compare.
  The shade kernel's DXIL went from 1.08 to 1.03 of DXC's.
* **A product of constants was a division per lane.**
  bc7 rescales a byte as `x * 255.0 / 255.0` and `x * top / 255.0`, which DXC's fast flags reassociate and native kept, 192 `fdiv`s by 255.
  At `.fast` the DXIL builder now folds `(v * c) / d` to `v * (c / d)` or to `v`, writes `v / d` as `v * (1 / d)`, and lets lanes share `top / 255`.
  bc7's DXIL went from 1.27 to 1.23, and holds no division by a constant, as DXC's holds none.
* **SPIR-V turned no if-else into a select.**
  `flatten_branches` was DXIL's alone, and DXC's SPIR-V goes through spirv-opt, which does: connect's SPIR-V held 171 phis and no `OpSelect` against DXC's 78 and 14.
  The pass gained a structured mode, which folds a selection whose arms head nothing and joins a block only into one no construct names.
  It keeps a phi of a pointer or a handle as a branch, since logical addressing selects neither.
  Every SPIR-V entry shrank: the path tracer from 1.21 to 1.06 of DXC's, the shade kernel from 1.29 to 1.11, and bc7 from 0.98 to 0.82.
* **A dead arm stayed on SPIR-V as stubs, and a scaled vector built its splat.**
  The structured `simplify` kept every block no walk reached as a label and `OpUnreachable`, 212 of them in the shade kernel; it now keeps only a merge or continue target a header that stays names.
  `v * s` was a splat and a vector `OpFMul` where DXC writes `OpVectorTimesScalar`, which the writer now does, leaving out a splat nothing else reads.
  The shade kernel's SPIR-V went from 1.11 to 1.03 of DXC's, and the path tracer's from 1.06 to 1.03.
* **A vector crossing nested merges stayed lanes at the inner one.**
  `regroup_lanes` found a group only from a `construct` reading phis, and an inner merge's lane phis are read by the outer merge's alone.
  So the shade kernel kept 602 scalar float phis against DXC's 154, beside DXC's 143 of `float3` and 51 of `float4`.
  A way whose lanes are another merge's phis now makes those a group first, so the vector is one phi at every merge it crosses.
  The shade kernel's SPIR-V went from 1.03 to 0.99 of DXC's, the path tracer's from 1.03 to 0.90, and bc7's from 0.80 to 0.70.
* **An unrolled loop tested a constant in every arm of every iteration.**
  bc4 read `bc_constants.snorm` and compared it with 0 in both arms of each texel's `channel_range` branch, 32 times, where DXC hoists the test out of the loop before unrolling it.
  No copy dominates another, so `share_common_values` could not merge them.
  `hoist_common_values`, which moved access chains alone, now also moves a value the whole dispatch shares into the common dominator of its copies, in rounds until nothing moves.
  bc4's DXIL went from 1.07 of DXC's operations to 1.03, and bc5's from 1.07 to 1.04.
* **A `not` of a known condition stayed a branch.**
  `simplify` folded every logical operation but `logical_not`, so an unrolled `continue` past a test of a constant kept its branch, which the DXIL writer then wrote as `br i1 true`.
  In bc3, whose color block never has a transparent texel, that left 48 such branches and a phi for every accumulator at each of their merges.
  `simplify` now folds a `not` of a constant, and of a `not`.
  bc3's DXIL went from 1.10 of DXC's operations to 1.00, and its SPIR-V from 1.02 to 0.91.
* **A comparison of a value that is one of a few constants stayed a comparison.**
  bc7's `fit_subset` takes `pmode` as `if opaque => 3 else 2` or `if mode == 1 => 1 else 2`, so its `pmode != 0` is always true, which DXC folds.
  Native kept `quantize`'s path without a p-bit and `quantize_pair`'s early return in every copy.
  `simplify` now folds a comparison every choice of whose operands answers alike, a choice being a constant, a select of two or a phi of them.
  bc7's DXIL went from 1.23 of DXC's operations to 1.09, and its SPIR-V from 0.70 to 0.61.
* **An array passed by value was copied, and every read of an element was a load.**
  Inlining leaves a function's array parameter as a local the caller's array is copied into, and bc7 copied its 64 floats three times.
  `share_common_values` shared no read of a local, since a local can be written.
  A read after which no store to its local can run is now final: a read it dominates of the same element is it, and a local whose one store copies another local from a final read is that other local.
  `remove_unused` drops a store into a local nothing reads, which is what the forwarded copy then is.
  bc7's DXIL went from 1.09 of DXC's operations to 0.99, three allocas fewer.
  Its time moved toward DXC's on every device: the Radeon's dx12 from 1.03 to 0.97, the B580's dx12 from 0.66 to 0.73, and the B580's vulkan from 0.57 to 0.56.

## What remains

* **dx12's wavefront frame, 1.03 to 1.04.**
  Its shade kernel's DXIL is 1.03 times DXC's in operations, from 1.21 before the constant phis, and its arithmetic 1.03.
  The last 1.08 to 1.03 left the frame where it was, 1.036 to 1.042, so what is left of the gap is not the count of operations.
  The largest lines left are `reorient_frame`'s, behind `leaned != f.n`, which DXC skips on the path that returned `n` itself.
  A kernel's own ratio does not measure it: with the SPIR-V unchanged, vulkan's shade read 1.01 in one set and 1.11 in another.
  `native-parity.py lines` counts both routes' arithmetic per `.sgl` line, which is how to find the next part of it.
  On 2026-11-08 the frame read 1.049 and 1.051, and among its kernels only shade stood apart, 1.19 and 1.20, with extend, segment and connect within 1%.
  After the forwarded copies and the folded comparisons below it read 1.15 and 1.16, and the frame 1.04 and 1.04.
  The Radeon's dx12 shade read 1.01 and vulkan's on the B580 1.0, so the gap is the B580's dx12 driver's alone, which reports no figures.
  Three causes were tried and ruled out, each a run against 1.19:
  - Handles: the shade kernel made a bindless texture's handle in each arm of an if-else over samplers, 54 where DXC makes 3.
    `hoist_common_values` now computes such a chain once in the arms' common dominator, which took the DXIL to 61 handles against DXC's 72; shade read 1.19 and 1.20.
  - `flatten_branches` off, for DXC's branches where native has 49 more selects: 1.20.
  - `merge_buffer_loads` off, for DXC's 112 single loads where native has 49 and 22 of four lanes: 1.33, so the merging helps.
  - Shader model 6.8, as DXC's module states, which also sets `ResMayNotAlias` for its UAVs: 1.19 and 1.16.
  The two modules' buffer accesses state the same alignment, their handles stand in the same blocks, and their loops nest alike.
  Native's arithmetic is 1.03 of DXC's, spread over many lines with none above 24 operations, so what is left needs the driver's own view of the code.
* **AMD's dx12 convolution, 1.03.**
  Its hot loop is the same shape on both routes, block for block: the same conditional loads, the same phis.
  DXC's extra live words are the row prologue, which it writes with branches where native selects, and which runs three times a thread.
  The DXIL is not where it shows, as far as four measurements on 2026-11-08 can say, each against a run of 1.032.
  - The hot loop holds DXC's 96 `fmul` and 96 `fadd`, its 12 loads, two fewer instructions, and a chain of the same depth, 13.
  - Issuing its loads first in the block, where native had them beside their uses and DXC early, read 1.032 again.
  - Marking every integer `add`, `sub` and `mul` `nsw`, as DXC does, read 1.027 and 1.031.
  - Shader model 6.8 rather than 6.6, which also sets `ResMayNotAlias` as DXC's module does, read 1.029 and 1.030.
  - shan counts DXC's DXIL at 124 vector words live against native's 82, so the slower module is not the one that holds more.
  What AMD's compiler makes of each module is the next place to look: its register count and its ISA.
* **The Radeon's vulkan bloom finish, 1.05: a wave size the driver picks.**
  AMD's compiler makes DXC's module a wave64 program and native's a wave32 one; its ISA's header says which, and the instructions are the same on both.
  It is the one kernel of the 15 surveyed on 2026-11-08 whose two routes the driver gives different wave sizes; the others agree, 10 at wave64 and 4 at wave32.
  The trigger is in the module's shape rather than its preamble, which is the same on both routes but for ids.
  - Stamping DXC's generator number into native's module left it wave32.
  - DXC wraps the body in a `switch` whose merge holds the one `Return`, for the bounds check's early return, where native returns from the branch.
    Written with no early return, both routes compile to wave32; written without `scattered`'s branches, or without `scattered`, so do both.
  - bloom first and up have the same early return, and their two routes agree, so the `switch` is not the whole rule.
  SPIR-V has no way to ask for a wave size, so the module cannot fix it; a pipeline can, through `VK_EXT_subgroup_size_control`, which sg does not expose.
* **The Radeon's vulkan bc3, 1.07 and 1.08: the driver copies each texel past its sRGB conversion.**
  Both routes are wave64, native at 126 vector registers to DXC's 128, and their ISA is the same 4500 instructions but for 34 `v_mov` and 14 `s_branch`.
  Those sit in `stored`'s `if bc_constants.srgb != 0`, once a texel of the unrolled read.
  DXC's arm converts a texel in the registers it was loaded into, so the other arm is empty and the merge a fallthrough.
  Native's arm writes two of the three channels to fresh registers, so the other arm copies the texel there and the converting one jumps over it.
  Both modules merge the same `float4` phi, of the converted texel and the loaded one, so the choice is the driver's register coalescing.
  Three differences between the two modules were tried on 2026-11-08 and changed nothing:
  - the phi's operands written in the other order;
  - the empty arm laid out first, as DXC's is, by swapping the source's arms;
  - `flatten_branches` off, which keeps the conversion's branches as DXC does: 4 jumps rather than 14, but 121 copies rather than 53.
* **The raster cull's `cull_second` DXIL, 1.16 in operations.**
  It computes an item's world box twice, in `in_view` and again for `occluded`, where DXC computes it once.
  The first is in the arm of `if item.has_box == 0u => return true` that falls through, and the second under `visible and item.has_box != 0u`.
  No copy dominates the other; DXC's jump threading sends the arm that returned straight past the second test, after which the first does.
