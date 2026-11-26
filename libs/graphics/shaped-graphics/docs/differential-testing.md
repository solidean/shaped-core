# Differential testing

One shader, run every way this machine can run it, and every result held to every other.
The other sg tests check a final answer someone wrote down; this one needs no answer, only agreement.
That is what makes a corpus of shaders as they appear in the wild cheap to grow: a new file is a new test, with no expected values in it.

The code is [tests/differential/](../tests/differential/).

## What runs where

A **route** is one way a context takes SGL.
A dx12 context has two, `native` (SGL's own DXIL) and `dxc` (SGL's HLSL through DXC), and a vulkan one the same for SPIR-V.
webgpu (`wgsl`) and metal (`msl`) have one each.
The engine states the route in the compile's options, so a context's own preference does not matter.
A native compile that fell back to DXC, because SGL has no lowering for it yet, is logged at info and compared with nothing.

Four tests run the corpus:

- `sg differential - every corpus entry point computes alike on each route this context takes` is an invocable, so every driver runs it.
  The validation layers and the never-block mode see both routes there.
- `sg differential - every corpus entry point computes alike on every backend and route of this machine` creates one context per backend this binary has.
  It holds every (backend, route) pair to the first one that ran.
  A software adapter is left out: WARP is no device a shader ships to.

- `sg differential - every corpus entry point computes what its committed reference says` holds the same pairs to what another machine computed.
- `sg differential - every corpus entry point computes on the GPU what SGL's interpreter says` holds them to [the interpreter](#the-interpreter), with no device at all.

All four run in the default suite; each takes about a second.

## A corpus file and its setup

A corpus file is an SGL file in [tests/differential/corpus/](../tests/differential/corpus/), and every entry point of its setup's stage is run.
The CMake globs the directory, so adding a file is all it takes.
Its comment block names a setup, and may override the setup's tolerances:

```sgl
// differential: setup floats
// differential: routes relative 0.00001
// differential: across relative 0.0001
use diff_floats

@compute(64) fun shade(@thread_id id: int3){diff_floats.io}:
    ...
```

A **setup** is the C++ that fills the inputs, dispatches and reads back, written once and shared by every file naming it.
Its contract with a corpus file is an SGL module in [tests/differential/setups/](../tests/differential/setups/): the file `use`s it and lists its binding.
So a shader that binds the wrong thing does not compile, rather than failing on one backend at bind time.
The setup fills the module's generated binding type, so the two sides cannot drift apart.
A setup registers itself with `sg_diff::register_setup`, naming the stage it runs, its run, its default tolerances, and its interpreter half where it has one.
Its module is `diff_<name>` unless it names another, as `blended` and `samples` share `targets`' module and vertex stage.
One that needs a feature names it, as `scene` names `ray_query`, and a context without it skips the setup's files rather than failing them.
A corpus file's own `require` lines do the same for it alone, as `half_math.sgl` requires `shader_f16` on the `floats` setup.
Its run is handed the entry point compiled on the route, and builds the pipeline itself.
A stage the setup brings, as the raster setup brings its vertex stage, compiles on the same route through `sg_diff::compiled_on`.
Those stages live in [tests/differential/setups/shaders/](../tests/differential/setups/shaders/), a package of their own.

| setup | stage | input | output | routes | across | interpreted |
|---|---|---|---|---|---|---|
| `words` | compute | 256 32-bit words, the edges first, then hashed bits | four words a thread | exact | exact | yes |
| `floats` | compute | 256 ordinary floats, -64 to 64, no zero | four floats a thread | 2 ulps | 16 ulps | yes |
| `texels` | compute | a 16 by 16 rgba8 texture with its mip chain, a nearest and a trilinear sampler | four float4 a thread | exact | relative 0.005 | yes |
| `raster` | pixel | one triangle over a 16 by 16 rgba32_float target, interpolated `uv` and `weight`, the `floats` input and the `texels` texture | the target | 2 ulps | relative 0.005 | yes |
| `scene` | compute | a TLAS of a two-geometry terrain and four placed octahedra, one mirrored | two float4 a thread, `hits` and `ids` | exact | relative 0.0001 | yes |
| `targets` | pixel | two depth-tested triangles over a 16 by 16 set of rgba8 sRGB, rgba16_float, rgb10a2, rg11b10_float and r32_uint targets | each target, decoded to a float a channel; `word` as its integers | exact | relative 0.005 | not yet |
| `blended` | pixel | the same two triangles blended over a cleared rgba8 sRGB, two rgba16_float and an rgb10a2 target: alpha over, added, subtracted and min | each target, decoded to a float a channel | exact | relative 0.005 | not yet |
| `samples` | pixel | the same two triangles over a cleared 4-sample rgba16_float target | every sample of every pixel, read back by a compute stage on the same route | exact | relative 0.005 | not yet |
| `tallies` | compute | the `words` input, and 64 atomic totals starting at 0 or all ones | the totals, which no thread order may change | exact | exact | yes |
| `records` | compute | 256 records of mixed members, a nested struct and a fixed array among them, written through SGL's generated struct | the rearranged records, and each member as the GPU read it | exact | exact | not yet |

The `across` bounds of the two texture setups allow for a filter weight of 8 fractional bits, which some hardware has.
The `targets` bound allows a step of the coarsest format, since an implementation may round a float into one either way.
**A corpus value never lands on a tie the target leaves open**: a step no bound absorbs is a vendor's choice.
Alpha 0.5 into rgb10a2's two alpha bits was one, between Intel and NVIDIA, and so was a ray along a triangle edge no other triangle shares.
An output a setup marks `is_exact`, as `targets` marks its integer target, is compared byte for byte and committed as a hash, whatever its file's bound.
`records` also holds what the GPU read to what the host wrote, member by member, and logs the first member read from elsewhere as an error.
Agreement between configurations cannot see a layout every backend gets wrong alike; that check can.
On the Arc B580 sampling and derivatives are bit-exact across dx12 and vulkan, and only a `dot` in a pixel stage moves, by 1 ulp.

A comment block naming no known setup, a tolerance or a `known` line that does not parse, fails both tests, naming the file.

## Known divergences

**A driver that computes a file wrongly is a finding, and an open issue says who is wrong; it need not keep every machine with that GPU from shipping.**
A `known` line names the vendor, the backend and the issue:

```sgl
// differential: known nvidia vulkan bug-sg-diff-half-math-unrounded-on-nvidia-vulkan
```

The vendor is `amd`, `apple`, `intel` or `nvidia`, matched by the adapter's PCI vendor id, and the backend is `dx12`, `vulkan`, `metal` or `webgpu`.
A vendor may name its drivers: `amd/mesa` is RADV and any other Mesa driver of AMD's hardware, `amd/own` is AMD's own, and `amd` is both.
Mesa's are those whose driver info starts `Mesa`, so no driver's name need be known.
On a matching context a difference logs at info, naming the issue, where it would fail.
**A known line that stops diverging fails**, against the reference and against the interpreter: a fixed driver retires its line, and the issue gets checked.
A trailing `precise` scopes a line to [the strict-floor twin](#the-strict-floor-twin) alone.
A trailing `across` scopes it to the tests that hold one GPU to another, the reference and the backends of one machine.
Those pass two values that both lie in the interpreter's range, but a file held `exact` commits only a hash of each output.
So there a conversion rounded toward zero against one rounded to nearest still differs, though both APIs allow it.
A matching context is never what the others are held to, and a refresh writes no reference from it.
Two routes of one matching context are not held to each other, since one driver folding both alike proves nothing.

## Tolerances

A tolerance is per output element:
- `exact` compares bytes.
- `ulps <n>` compares 32-bit floats by units in the last place.
- `relative <x>` compares them against the larger magnitude, or against `x` itself below a magnitude of 1.

Any NaN matches any NaN, and an infinity matches only itself.

**Two routes of one context are held to the setup's tight bound.**
One driver compiles both routes' IR, so a difference there is a difference in what SGL and DXC wrote.
**An override of that bound is a known divergence, and names its issue in the comment block.**
Two APIs get the looser `across` bound, since their drivers round transcendentals and fold expressions as they like.

**Two GPU results also agree where both lie within the bound of [the interpreter's range](#the-interpreter) for the element.**
At `.fast` DXC reassociates and contracts what SGL's own DXIL writes in source order, as the level allows, and one driver then rounds the two apart.
The range holds both orders, so neither needs a looser bound; an element the interpreter bounds by nothing is held to the bound alone.
The reference test does the same for the elements a reference commits.

## The interpreter

SGL's interpreter runs an entry point on the abstract machine: the answer the language means, deterministic and with no device.
Agreeing with each other, two routes might share a bug; agreeing with the interpreter too, they compute what the source says.

A setup's interpreter half binds the same inputs by the names its module gives them, the way an `sgl test` driver does.
The engine checks the corpus file against the setups' modules and runs the dispatch one invocation after another, each invocation's stores written back before the next.
A pixel stage runs as 2 by 2 quads, each pixel a lane with its own interpolated varyings (EVAL-148), so its derivatives and the level a sample picks are a quad's.
The raster setup's half gives the varyings at each pixel's center, as its vertex stage writes them; the vertex stage itself is not interpreted.
The scene's half builds the world with `accel_build_flag::software_trace` too and binds the context's software pool, so the interpreter traces it as sg's software walk does.

### What a GPU may compute

The interpreter runs bounding (`run_limits::bounds_floats`), so each float it stores carries the range of values a GPU may give for it, besides the exact one.
The range is what the entry point's `@float` level licenses, operation by operation (EVAL-6):
- a half operation kept at a float's precision at `.fast`;
- a product left unrounded as a fused multiply-add keeps it, at `.fast` and `.specials`;
- a sum of `+` and `-` added in any order, at `.fast` and `.specials`: Higham's γ(k) times the terms' magnitudes;
- a `dot` or a matrix product rounded step by step in any order, at every level, by the same bound;
- a denormal taken as zero, below `.ieee`.

A sum's chain carries through a `let`, since a compiler reassociates through one, and a chain of two terms adds in one order alone.

A license applies to each operation and a tolerance to the result, so no tolerance can stand in for one.
`half_arithmetic.sgl`'s `a * b - a` is the case: with `b` near 1 the subtraction cancels, and a product Intel's Arc keeps at a float's precision lands 3% from binary16's answer, inside its range.

Every configuration is held to that range, and the file's `across` bound applies from its nearer end, for what the range leaves out:
a builtin's own precision, and a texture read, a subgroup operation, a derivative or an atomic, each of which takes its arguments exactly.
The test says at info how much of `across` each file used against the interpreter, beyond its ranges, over every configuration it holds no known divergence for.
That number is how far a file's tolerance carries what the ranges leave out, and 0 says the ranges hold everything.
A file held `exact` passes a ranged element anywhere in its range at `fast` and `specials`, where a half may keep a float's precision and the range is a hull.
At `precise` and `ieee` it passes only where it equals one end, bit for bit.
The range there is a conversion's two roundings, and a value between them is no rounding of it: Intel's dx12, NVIDIA's vulkan on an RTX 4090 and RADV return one.
So does a construct a device runs natively, the trace above all: its emulated form is the interpreter's stand-in for the hardware, whose precision is its own.

### The strict-floor twin

`sg differential - every corpus entry point computes at a precise floor what SGL's interpreter says` is the twin.
It compiles every entry point again at a `.precise` floor (`slib::k_float_floor_option`), and runs the interpreter at the same floor.
There a target may no longer reassociate, contract or keep a half at a float's precision, so the ranges shrink to rounding, denormals and a sum of products' steps.
It checks that a driver keeps what it is asked to keep, which the file's own level, usually `.fast`, cannot.
WGSL keeps only `.fast` (EMIT-185), so a webgpu context runs no twin.

A file's `known` lines describe its own level, so in the twin they only log, whether the driver diverges there or not.
A `known` line that ends in `precise` is the twin's own, held as the other tests hold theirs.
The first is `half_math.sgl` on Intel's dx12, which folds a float's round trip through a half away at any level.
[bug-sg-diff-half-corpus-fails-on-intel](issues/bug-sg-diff-half-corpus-fails-on-intel.md) tracks it.
`sg differential - a compile at a precise floor keeps the floor through slib` holds the floor to the GPU's compile, since the first twin lost it in slib and blamed a driver.
`SC_DIFFERENTIAL_SHADER_DUMP=<dir>` writes the bytecode every route ran there, to run it in a standalone reproduction.

### Open conditions

A condition the range leaves open, one a GPU may decide either way, is run both ways and the stores joined, up to six of them in an invocation.
Past that, and at an index or a `switch` the range leaves open, what the invocation stores after it is bounded by nothing, and the test says at info how many elements are.
The corpus has none today.

It cannot run everything, and says so at info rather than failing.
**Workgroup memory** spans the invocations of a workgroup, which the interpreter runs one at a time, so `workgroup_reduction.sgl` is left out.

The interpreter has already paid for itself in the corpus.
Its first run found two lines that tested an optimizer rather than SGL: a cancellation that drivers fold to zero, and one whose `across` bound was too tight for any second vendor.
The scene found a bug: the software walk, which webgpu's ray queries run, reported every triangle's facing the other way round from dx12 and vulkan hardware.

## References across machines

One machine sees only its own GPUs, so a divergence only another vendor has would never show here.
So every corpus file has a committed reference in [tests/differential/references/](../tests/differential/references/), `integer_bits.ref` for `integer_bits.sgl`.
It is plain text, one line per thing it holds, so a diff says exactly what moved:

- An output the file holds `exact` across APIs is its size and a 64-bit FNV-1a hash.
- Any other output is 64 of its elements, spread through it, each as its bits and, for a reader, its value.

Each entry point's block names the configuration that wrote it.
Every configuration of every machine is held to it at the file's `across` bound, or through the interpreter's range as two routes are, so a reference written on one GPU checks every other.
A corpus file without a reference fails, naming how to write one.

To write or rewrite them, run the test with `SC_DIFFERENTIAL_REFRESH=1`:

```bash
SC_DIFFERENTIAL_REFRESH=1 uv run dev.py test "sg differential - every corpus entry point computes what its committed reference says"
```

Each entry point is written from the first configuration of this machine that runs it, then every configuration is held to it.
**A refresh is a change like any other, and its diff is reviewed.**
A reference that moves is either a fix, said in its commit, or a regression the test was there to catch.
A machine whose GPU disagrees beyond `across` is a finding: who is right is a question for an issue, not for a refresh.

To run the references on another GPU of the same machine, `SC_GPU` picks it, as `SC_GPU=amd` does an AMD iGPU beside a discrete card.
Two switches help chase what it finds:
- `SC_DIFFERENTIAL_ONLY=<text>` narrows the corpus to the files whose name holds it, `SC_DIFFERENTIAL_ONLY=packing.sgl`.
- `SC_DIFFERENTIAL_DUMP=<dir>` writes every configuration's outputs there whole, one file each, to compare two GPUs byte by byte.

The first run on another vendor's GPU, an AMD RDNA 2 iGPU on 2026-11-13, matched the Arc's references on 20 of 23 files.
It found that AMD converts a float to a half toward zero, that an inline trace crashes its dx12 compiler, and a negative zero the corpus packed without asking to keep its sign.

## Not yet

- **A resolve:** sg has no multisample resolve yet, so `samples` reads each sample instead; a resolve setup waits on that API.
- **Workgroups on the interpreter:** a workgroup's invocations run together up to each barrier, a change to the interpreter itself.

[feat-sg-differential-references-and-setups](issues/feat-sg-differential-references-and-setups.md) tracks them.
