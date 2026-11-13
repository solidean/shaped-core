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
One that needs a feature names it, as `scene` names `ray_query`, and a context without it skips the setup's files rather than failing them.
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

The `across` bounds of the two texture setups allow for a filter weight of 8 fractional bits, which some hardware has.
The `targets` bound allows a step of the coarsest format, since an implementation may round a float into one either way.
An output a setup marks `is_exact`, as `targets` marks its integer target, is compared byte for byte and committed as a hash, whatever its file's bound.
On the Arc B580 sampling and derivatives are bit-exact across dx12 and vulkan, and only a `dot` in a pixel stage moves, by 1 ulp.

A comment block naming no known setup, or a tolerance that does not parse, fails both tests, naming the file.

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

The first run found one such divergence, [bug-sgl-dxil-division-and-normalize-round-unlike-dxc](../../shaped-graphics-language/docs/issues/bug-sgl-dxil-division-and-normalize-round-unlike-dxc.md).

## The interpreter

SGL's interpreter runs an entry point on the abstract machine: the answer the language means, deterministic and with no device.
Agreeing with each other, two routes might share a bug; agreeing with the interpreter too, they compute what the source says.

A setup's interpreter half binds the same inputs by the names its module gives them, the way an `sgl test` driver does.
The engine checks the corpus file against the setups' modules and runs the dispatch one invocation after another, each invocation's stores written back before the next.
A pixel stage runs as 2 by 2 quads, each pixel a lane with its own interpolated varyings (EVAL-148), so its derivatives and the level a sample picks are a quad's.
The raster setup's half gives the varyings at each pixel's center, as its vertex stage writes them; the vertex stage itself is not interpreted.
The scene's half builds the world with `accel_build_flag::software_trace` too and binds the context's software pool, so the interpreter traces it as sg's software walk does.
Every configuration is held to the result at the file's `across` bound, since the interpreter rounds as plain `f32` and models no target.

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
Every configuration of every machine is held to it at the file's `across` bound, so a reference written on one GPU checks every other.
A corpus file without a reference fails, naming how to write one.

To write or rewrite them, run the test with `SC_DIFFERENTIAL_REFRESH=1`:

```bash
SC_DIFFERENTIAL_REFRESH=1 uv run dev.py test "sg differential - every corpus entry point computes what its committed reference says"
```

Each entry point is written from the first configuration of this machine that runs it, then every configuration is held to it.
**A refresh is a change like any other, and its diff is reviewed.**
A reference that moves is either a fix, said in its commit, or a regression the test was there to catch.
A machine whose GPU disagrees beyond `across` is a finding: who is right is a question for an issue, not for a refresh.

## Not yet

- **More setups:** blending into those formats, and multisampled targets resolved.
- **Workgroups on the interpreter:** a workgroup's invocations run together up to each barrier, a change to the interpreter itself.

[feat-sg-differential-references-and-setups](issues/feat-sg-differential-references-and-setups.md) tracks them.
