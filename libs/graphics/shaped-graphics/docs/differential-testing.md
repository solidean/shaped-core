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

Two tests run the corpus:

- `sg differential - every corpus entry point computes alike on each route this context takes` is an invocable, so every driver runs it.
  The validation layers and the never-block mode see both routes there.
- `sg differential - every corpus entry point computes alike on every backend and route of this machine` creates one context per backend this binary has.
  It holds every (backend, route) pair to the first one that ran.
  A software adapter is left out: WARP is no device a shader ships to.

Both run in the default suite; the whole corpus takes about a second.

## A corpus file and its setup

A corpus file is an SGL file in [tests/differential/corpus/](../tests/differential/corpus/), and every compute entry point in it is run.
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
A setup registers itself with `sg_diff::register_setup`, naming its pipeline layout, its run, and its default tolerances.

Today there are two, both a buffer in and a buffer out over 256 threads:

| setup | input | output | routes | across |
|---|---|---|---|---|
| `words` | 32-bit words, the edges first, then hashed bits | four words a thread | exact | exact |
| `floats` | ordinary floats, -64 to 64, no zero | four floats a thread | 2 ulps | 16 ulps |

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

## Not yet

- **References across machines.** Exact outputs would commit a hash per (file, entry, output), and float ones a small reference set held to `across`.
- **Setups beyond buffers:** textures and samplers, a raster pass, a trace over a fixed scene.
- **The interpreter as a third opinion.** For compute work SGL's interpreter gives the answer the language means, with no device at all.

[feat-sg-differential-references-and-setups](issues/feat-sg-differential-references-and-setups.md) tracks them.
