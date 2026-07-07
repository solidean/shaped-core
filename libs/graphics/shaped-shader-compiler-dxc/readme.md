# shaped-shader-compiler-dxc

A lean wrapper over the [DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler)
(DXC, `IDxcCompiler3`) that turns HLSL into an
[`sg::compiled_shader`](../shaped-graphics/src/shaped-graphics/compiled_shader.hh) — bytecode plus the
reflected bindings and compute workgroup size sg needs to build pipelines. Namespace `ssc::dxc`. Depends on
**shaped-graphics** (and transitively typed-geometry + clean-core). Part of the
[graphics family](../../../docs/graphics.md).

**Windows-only** for now: it links DXC and uses the Windows SDK's `d3d12shader.h` reflection.

## Two-step compilation

Compilation is split so callers control include resolution and can cache the flattened source:

1. **`preprocess`** — expand macros and inline `#include`s via a caller-supplied resolver
   (`cc::function_ref<cc::optional<cc::string>(cc::string_view)>` — a virtual file system, no file I/O
   baked in). Returns the flattened HLSL.
2. **`compile`** — turn *already-preprocessed* source into an `sg::compiled_shader`. A stray `#include`
   is rejected (the source is supposed to be flat by now).

```cpp
#include <shaped-shader-compiler-dxc/all.hh>

auto comp = ssc::dxc::compiler::create();                 // cc::result<ssc::dxc::compiler>

ssc::dxc::shader_description desc{.source = hlsl, .entry_point = "main",
                            .stage = sg::shader_stage::compute};

auto pp = comp.value().preprocess(desc, resolve_include);   // step 1 (optional)
desc.source = pp.value().source;
auto shader = comp.value().compile(desc);            // cc::result<sg::compiled_shader>
```

## DXC is downloaded on demand

DXC is neither vendored nor built from source (its from-source build is LLVM-scale, far too slow for
CI). Instead [`extern/dxc/download-dxc.py`](../../../extern/dxc/download-dxc.py) fetches the pinned
official release (`v1.9.2602.24`) for the host architecture, verifies its SHA-256, and extracts the
`dxcompiler.dll` + import lib + headers (plus the `dxil.dll` signer) into a gitignored
`extern/dxc/.install/`. The first Windows configure runs it (a few-second download); every configure
after is a cheap pin-file check. Set `SC_SKIP_DXC=1` to skip it (the library is then left unbuilt).

The release ships `dxil.dll`, so emitted DXIL is **signed** and runs on dx12 without developer mode.
`arm64` binaries are in the same release, so Windows ARM works too.

## Building & testing

Build and test through the repo driver — never run the `shaped-shader-compiler-dxc-test` binary
directly:

```bash
uv run dev.py test -t shaped-shader-compiler-dxc-test   # while iterating
```

See [building-and-testing](../../../docs/guides/building-and-testing.md) for the full workflow.

## More

- [cheat-sheet.md](cheat-sheet.md) — the public API at a glance.
- [graphics.md](../../../docs/graphics.md) — the whole graphics family overview.
