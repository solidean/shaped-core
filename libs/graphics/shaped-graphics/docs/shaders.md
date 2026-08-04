# Shaders

How a shader gets from a file you edit to something a `sg::context` can build a pipeline from.

This lives in shaped-graphics because it is where you will look first, but most of the machinery is
**downstream** of sg — sg deliberately does not depend on it:

```text
shaped-shader-library (slib::)   packages, hot reload, the compiler registry
        ↓ depends on
shaped-shader-compiler-dxc (ssc::dxc::)   HLSL -> DXIL
        ↓ depends on
shaped-graphics (sg::)           compiled_shader, shader_stage, shader_format, context
```

sg owns only the **vocabulary**: what a compiled shader *is*, and what a context accepts.
Everything that reads, compiles, reloads or ships one sits above it.
That is why sg is usable with no shader library at all, and why a shader library can be replaced without touching sg.

## What sg owns

| Type | What it is |
|---|---|
| [`sg::compiled_shader`](../src/shaped-graphics/binding/compiled_shader.hh) | bytecode + stage + entry point + reflected [bindings](concepts/bindings.md) (+ compute workgroup size) |
| `sg::shader_stage` | `compute`, `vertex`, `fragment`, the six ray-tracing stages, … |
| `sg::shader_format` | the bytecode flavour: `dxil`, `spirv`, `metal_lib` |
| `sg::async_compiled_shader` | `cc::shared_async<compiled_shader>` — compilation is asynchronous and fallible |
| `ctx.accepted_shader_formats()` / `ctx.accepts_shader_format(f)` | what bytecode *this* context can consume (dx12 → DXIL, vulkan → SPIR-V) |

A `compiled_shader` is a pure value.
sg never produces one — it only consumes them, through [pipelines](concepts/raster-pipeline.md) and [caches](concepts/caches.md).

## Declaring shaders: a package

A **shader package** is one target's shaders.
Any target declares its own — a library, an app, or a test binary — in its own CMakeLists:

```cmake
sc_add_shader_package(
    TARGET     my-renderer
    NAME       my_shaders          # package id; also the header name and the mount point
    NAMESPACE  my::shaders         # where the generated symbols live
    SOURCE_DIR shaders             # relative to this CMakeLists
    LANGUAGE   hlsl
    SHADERS
        vignette.hlsl:compute:main         # path:stage:entry_point
        blit.hlsl:vertex:main_vs
        blit.hlsl:fragment:main_ps
)
```

That generates a header of typed C++ symbols, so a typo is a compile error rather than a lookup that
finds nothing at runtime:

```cpp
#include <my_shaders.hh>

auto cs = my::shaders::vignette.compute.main->acquire(ctx);
auto vs = my::shaders::blit.vertex.main_vs->acquire(ctx);
```

Stages are spelled exactly as `sg::shader_stage`, so what you write in CMake is what the enum says.

Call `sc_finalize_shader_packages()` once at the bottom of the **root** CMakeLists.
It turns "a package was declared but shaped-shader-library was never added" into a message naming the targets, instead of a missing header inside the generated code at build time.

The generated header is private to the declaring target.
A library that wants to publish a shader re-exposes it from its own public header — see [slib's coding guidelines](../../shaped-shader-library/docs/coding-guidelines.md).

## Getting a shader: `acquire(ctx)`

**You pass the context, and you get back a shader in a format it accepts.** A shader is authored once,
in one language, but may be consumed by several backends — so the format is not the shader's property,
it is the *consumer's*. `acquire` picks a registered compiler that connects the package's language to
something the context takes.

If nothing connects them (say a vulkan context with only an HLSL→DXIL compiler registered), you get an
async error saying so — rather than bytecode the context cannot use.

`acquire` returns an `sg::async_compiled_shader`, which is a `cc::async` node: install a default async
pool (`cc::install_default_async_pool`) and its workers run the compile, or drive it yourself with
`cc::try_async_blocking_get_singlethreaded`. Compilation is lazy and per format — nothing compiles
until something asks.

## Wiring it up

Once, at startup:

```cpp
slib::shader_library lib;
lib.add_compiler(slib::create_dxc_compiler().value());   // hlsl -> dxil
lib.add_package(my::shaders::package());                 // fills in the symbols above
lib.start_hot_reload();                                  // after every package
```

Nothing else touches the library; call sites go through the generated symbols.
It is not a singleton, but the generated symbols *are* process-wide globals, so only one library may exist at a time.

`create_dxc_compiler()` exists only where slib was built with DXC — Windows, with `extern/dxc` fetched.
`SLIB_HAS_DXC` (1 or 0, defined for anything linking slib) is the guard.
Packages, mounts, reload and the whole test suite build and run everywhere without it.
But HLSL→DXIL is the only compiler that exists today, so off Windows there is nothing to register and every `acquire` returns the error above.

## Hot reload

`start_hot_reload()` watches every file each shader was built from — its own source **and** every
`#include` that was resolved while preprocessing it, so editing a shared `.hlsli` reloads the shaders
that pull it in.

The contract that matters:

- **A reload never blocks a consumer.**
  The watcher recompiles on its own thread and only *stages* the result.
  `acquire` promotes it once ready; until then you keep getting the shader you already had.
- **A broken edit is survivable.**
  If the new shader does not compile, the last good one keeps running and `asset->last_error()` says why.
  Fix the file, save again, and it recovers.
- **You are told when it changed.**
  `asset->generation()` moves when a shader is replaced — cache it to know when to rebuild a pipeline.
  `lib.generation()` is the coarse "something, somewhere changed".
- **An idle watcher costs nothing.**
  The OS says when a file moved, so there is no interval and no periodic wakeup: a save reaches the watcher directly rather than being noticed on the next tick.

```cpp
if (auto const g = my::shaders::vignette.compute.main->generation(); g != known_generation)
{
    rebuild_pipeline();
    known_generation = g;
}
```

Where there are no threads (`SC_THREADS=OFF`, WebAssembly), pass `{.unthreaded = true}` and call
`lib.poll_hot_reload()` yourself — it is a no-op otherwise, so it is safe to call every frame either way.

Where the OS cannot be asked to notify, the watcher quietly falls back to rescanning every `reload_config::interval_ms`.
That covers a build without threads, a platform whose watch backend is not written yet (today, everything but Windows), and a source directory that is not there.
Nothing about the contract above changes — only the latency and the idle cost do.
`{.force_polling = true}` takes that path deliberately, which is only worth doing to test it.

## Dev vs shipping

There is **no mode flag**. The package generator does two things at build time:

1. bakes the absolute path of your `SOURCE_DIR` into the generated code, and
2. embeds every shader source — including the transitive `#include` closure — into the binary.

At startup the library mounts the embedded copy, then mounts the source directory *over* it — always, since the generator always baked a path there.
A dev machine has the sources, so that mount answers and gets watched.
A shipped binary does not, so the mount finds nothing and every read falls through to the embedded copy.
Same code, same build, and nothing ever asks "am I installed".

Shipping still compiles at startup, so DXC ships with the binary today.
On Windows with DXC, an **executable** that declares a package also gets `dxcompiler.dll` and `dxil.dll` staged beside it at build time.
The import is load-time, so a binary that reaches DXC will not start without them.
Declaring the package on a *library* target stages nothing — the executable that links it has to copy them itself.
Precompiled bytecode — baked at build time and shipped instead of source — is [planned](../../shaped-shader-library/docs/structure.md).

## Where shader sources come from

slib reaches every shader through a **mounted virtual filesystem**, never a raw path.
A package mounts at its own name, and anything else can be mounted anywhere:

```cpp
lib.mount("common", std::make_shared<slib::real_filesystem>(shared_shader_dir));
// every package can now #include "common/brdf.hlsli", wherever that folder actually lives
```

Shared shader libraries therefore get a stable include path regardless of disk layout, and `..` cannot climb out of a mount.
It is also why reload tests need neither a disk nor a sleep — they mount a `slib::memory_filesystem`, and a "file edit" is a `write()`.

An `#include "..."` is looked for in three places, most specific first: the shader's own directory, then the package root, then the mount root.
All three are fixed from the shader being compiled, at every depth — an include pulled in by another include still resolves from the shader's directory, not from its includer's.
The last is what reaches a library mounted outside any package, so `#include "common/brdf.hlsli"` resolves against the `mount` above.
Every path that resolves becomes a reload dependency of the shader that pulled it in.

## Adding a shader

1. Put the `.hlsl` under your target's `SOURCE_DIR`.
2. Add one `path:stage:entry_point` line to that target's `sc_add_shader_package`.
3. Rebuild — the symbol appears.
4. `acquire(ctx)` it.

## More

- [shaped-shader-library](../../shaped-shader-library/readme.md) — packages, mounts, reload ([cheat-sheet](../../shaped-shader-library/cheat-sheet.md)).
- [slib coding guidelines](../../shaped-shader-library/docs/coding-guidelines.md) — why reload stages rather than replaces, and why one file touches the disk.
- [shaped-shader-compiler-dxc](../../shaped-shader-compiler-dxc/readme.md) — the HLSL→DXIL compiler.
- [concepts/bindings](concepts/bindings.md) — what the reflection on a `compiled_shader` means.
- [concepts/caches](concepts/caches.md) — how a compiled shader becomes a cached pipeline.
