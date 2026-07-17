# shaped-shader-library

The shader package + hot-reload mechanism. Namespace `slib`. Depends on **shaped-graphics** (and
transitively typed-geometry + clean-core). Part of the [graphics family](../../../docs/graphics.md).

Any target — a downstream library, an app, or a **test binary** — declares its own shader package in
its own CMakeLists, gets typed C++ symbols for its shaders, and gets hot reloading. Packages are
registered explicitly at startup; many packages share one reload mechanism.

**Start with [shaped-graphics' shaders.md](../shaped-graphics/docs/shaders.md)** — the front door for
the whole shader system, including the parts that live here.

```cmake
sc_add_shader_package(
    TARGET my-renderer  NAME my_shaders  NAMESPACE my::shaders
    SOURCE_DIR shaders  LANGUAGE hlsl
    SHADERS post/vignette.hlsl:compute:main)
```

```cpp
slib::shader_library lib;
lib.add_compiler(slib::create_dxc_compiler().value());
lib.add_package(my::shaders::package());
lib.start_hot_reload();

auto cs = my::shaders::vignette.compute.main->acquire(ctx);   // sg::async_compiled_shader
```

## Design at a glance

- **Packages, not one global list.** Each target owns its shaders. sg does not depend on slib — the
  arrow points slib → sg — yet `shaped-graphics-test` declares a package and it works.
- **You pass the context, not a format.** A shader is authored in one language and may be consumed by
  several backends, so the bytecode format is the *consumer's* property. `acquire(ctx)` picks a
  registered compiler that connects the package's language to a format the context accepts, and reports
  an error if none does. Compilation is lazy per format.
- **The compiler is a seam.** `slib::shader_compiler` is one edge, language → format; register as many
  as you like. Only HLSL→DXIL exists today, and it is compiled in only where DXC does.
- **Everything is a mounted virtual filesystem.** slib never touches a raw path. A package mounts at its
  own name, shared includes mount anywhere, and exactly one adaptor (`real_filesystem`) reaches the disk.
- **Reload never blocks a consumer.** The watcher recompiles on its own thread and only *stages* the
  result; `acquire` promotes it once ready. A broken edit keeps the last good shader and reports why.
- **Dev vs shipping is not a mode.** The generator bakes the source dir *and* embeds every source. The
  library mounts the embedded copy, then the source dir over it if it exists. Dev reads and watches the
  tree; a shipped binary is self-contained.

## File organization

Source lives in `src/shaped-shader-library/`:

| Path | What's in it |
|---|---|
| (root) | `fwd.hh`, `all.hh`, and the core: `shader_package`, `shader_asset`, `shader_library` |
| `filesystem/` | the mountable VFS: the `filesystem` interface, `mount_table`, and the `memory` / `embedded` / `real` implementations |
| `compiler/` | the `shader_compiler` seam and the concrete compilers (`dxc_compiler`) |
| `impl/` | internal: the reload watcher |
| `cmake/` | `sc_add_shader_package` + the package generator |

## Building & testing

Build and test through the repo driver — never run the `shaped-shader-library-test` binary directly:

```bash
uv run dev.py test "slib"
```

The tests run against a **fake compiler**, so the whole mechanism — packages, mounts, lazy compiles,
reload, dependency tracking — is covered on every platform rather than only where DXC exists. Only
[tests/dxc_compiler-test.cc](tests/dxc_compiler-test.cc) needs a real compiler. Reload tests use a
`memory_filesystem` and an unthreaded watcher, so they involve no disk and no sleeps.

See [building-and-testing](../../../docs/guides/building-and-testing.md) for the full workflow.

## More

- [shaders.md](../shaped-graphics/docs/shaders.md) — the shader system front door. **Read this first.**
- [cheat-sheet.md](cheat-sheet.md) — the public API at a glance.
- [docs/_index.md](docs/_index.md) — this library's documentation hub.
- [docs/structure.md](docs/structure.md) — the roadmap and status.
- [docs/coding-guidelines.md](docs/coding-guidelines.md) — the two rules the code cannot enforce itself.
- [graphics.md](../../../docs/graphics.md) — the whole graphics family overview.
