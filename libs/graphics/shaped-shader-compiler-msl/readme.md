# shaped-shader-compiler-msl

A lean wrapper over Apple's Metal shader toolchain.
It turns MSL into an [`sg::compiled_shader`](../shaped-graphics/src/shaped-graphics/binding/compiled_shader.hh): a blob plus the reflected bindings sg builds pipelines from.
Namespace `ssc::msl`.
Depends on **shaped-graphics** (and transitively typed-geometry + clean-core).
Part of the [graphics family](../../../docs/graphics.md).

Apple targets only, and **built unconditionally there** — which is the first thing that differs from the DXC wrapper.

## Two artifacts, because Metal has two

A compile produces one of two things, and `compile_options::artifact` picks:

| artifact | what it is | needs |
|---|---|---|
| `metal_lib` | AIR in a container — the bytes `xcrun metal` writes, portable to any Apple GPU | the Metal toolchain |
| `msl` | the source text, compiled by the driver when a pipeline is built | nothing |

`artifact_kind::automatic` takes the first where the toolchain is installed and the second where it is not, and the
`sg::shader_format` on the result says which happened.
`artifact_kind::metallib` refuses rather than falling back, which is what a build that ships bytecode asks for.

**Apple ships the Metal toolchain as a component installed separately from Xcode** (`xcodebuild -downloadComponent MetalToolchain`), while the driver's own compiler ships with the OS.
That is the whole reason for two arms: a machine with no toolchain still compiles shaders, it just carries source rather than bytes.
So there is no configure-time gate here — `xcrun metal` is resolved at run time by `compiler::create()`, and `toolchain()` reports what it found.

```cpp
#include <shaped-shader-compiler-msl/all.hh>

auto comp = ssc::msl::compiler::create();                    // cc::result<ssc::msl::compiler>
auto shader = comp.value().compile({.source = msl, .entry_point = "main0"});
// shader.value().format is metal_lib or msl; bindings and workgroup_size are set either way
```

## Reflection reads the text, not the blob

A metallib records no reflection a tool can read without a GPU, and Metal's own reflection is a by-product of building
a **pipeline state** — which needs a device, needs a vertex layout before a `[[stage_in]]` vertex function will build
at all, and does not exist for the `[[visible]]` functions a miss or closest-hit shader compiles to.

So the bindings come from the MSL source, which is the one source that works for every stage on every host, with or
without a GPU.
[impl/msl_reflection.hh](src/shaped-shader-compiler-msl/impl/msl_reflection.hh) states the subset understood; anything
outside it is an error naming the declaration rather than a guess.

**One rule worth knowing before writing a shader**: every `device T*` is a *structured* buffer.
MSL spells a raw byte-addressed buffer exactly the same way, so the text cannot tell them apart.

## What it does not do

- **It resolves no `#include`.**
  SGL emits text that has none, and `#include <metal_stdlib>` is a Clang module the Metal compiler resolves itself.
  A hand-written shader that needs a search path passes `-I` through `extra_args`.
- **It spawns a process.** There is no library form of Apple's compiler, so the metallib arm runs `xcrun metal` with
  the source on stdin and the metallib on stdout — no temporary files.
  [impl/metal_driver.hh](src/shaped-shader-compiler-msl/impl/metal_driver.hh) is that helper, and it is **temporary**:
  process execution belongs in clean-core, and this is the second hand-rolled copy in the tree.

## Building & testing

Build and test through the repo driver — never run the `shaped-shader-compiler-msl-test` binary directly:

```bash
uv run dev.py test -t shaped-shader-compiler-msl-test
```

The tests that need no toolchain and no GPU are the majority, and the rest return early rather than failing where the
host has neither.
See [building-and-testing](../../../docs/guides/building-and-testing.md) for the full workflow.

## More

- [cheat-sheet.md](cheat-sheet.md) — the public API at a glance.
- [slib's metal compiler](../shaped-shader-library/src/shaped-shader-library/compiler/metal_compiler.hh) — the edge that carries a package here, SGL packages included.
- [graphics.md](../../../docs/graphics.md) — the whole graphics family overview.
