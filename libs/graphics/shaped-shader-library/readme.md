# shaped-shader-library

The shader package + hot-reload mechanism.
Namespace `slib`, depending on **shaped-graphics** and transitively typed-geometry + clean-core.
Part of the [graphics family](../../../docs/graphics.md).

Any target — a downstream library, an app, or a **test binary** — declares its own shader package in its own CMakeLists, gets typed C++ symbols for its shaders, and gets hot reloading.
Packages are registered explicitly at startup; many packages share one reload mechanism.

**Start with [shaped-graphics' shaders.md](../shaped-graphics/docs/shaders.md)** — the front door for the whole shader system, including the parts that live here.
This readme is what is in the box and how to build it; that one is how to use it.

```cmake
sc_add_shader_package(
    TARGET my-renderer  NAME my_shaders  NAMESPACE my::shaders
    SOURCE_DIR shaders  LANGUAGE hlsl
    SHADERS vignette.hlsl:compute:main)
```

```cpp
slib::shader_library lib;
lib.add_compiler(slib::create_dxc_compiler().value());
lib.add_package(my::shaders::package());
lib.start_hot_reload();

auto cs = my::shaders::vignette.compute.main->acquire(ctx);   // sg::async_compiled_shader
```

- **You pass the context, not a format** — `acquire(ctx)` picks a compiler that reaches a format that context accepts.
- **The compiler is a seam**; HLSL→DXIL is the only edge today, and only where DXC exists.
- **Dev vs shipping is not a mode flag**, and shader sources are reached only through a mounted virtual filesystem.

## File organization

Source lives in `src/shaped-shader-library/`, with `cmake/`, `docs/` and `tests/` at the library root:

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

The tests run against a **fake compiler**, so the whole mechanism — packages, mounts, lazy compiles, reload, dependency tracking — is covered on every platform rather than only where DXC exists.
Only [tests/dxc_compiler-test.cc](tests/dxc_compiler-test.cc) needs a real compiler.
Reload tests need no disk and no sleeps; [docs/coding-guidelines.md](docs/coding-guidelines.md) says what that rests on.

See [building-and-testing](../../../docs/guides/building-and-testing.md) for the full workflow.

## More

- [shaders.md](../shaped-graphics/docs/shaders.md) — the shader system front door.
- [cheat-sheet.md](cheat-sheet.md) — the public API at a glance.
- [docs/_index.md](docs/_index.md) — this library's documentation hub.
- [docs/structure.md](docs/structure.md) — the roadmap and status.
- [docs/coding-guidelines.md](docs/coding-guidelines.md) — the two rules the code cannot enforce itself.
- [graphics.md](../../../docs/graphics.md) — the whole graphics family overview.
