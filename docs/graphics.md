# The graphics library family (sg / sr / sv)

`libs/graphics/` is shaped-core's graphics stack: three layered libraries on the `base` foundation, plus two shader-side utilities.
[libraries.md](libraries.md) is the catalog — what each library is, and what it depends on.
This page is the family shape: how they fit together, and where the seams are.

sg is the graphics-API wrapper; sr builds render routines, windowing and the ImGui renderer on it; sv is the visualization renderer on top of sr.
slib and ssc::dxc hang off sg to the side: shader packages with hot reload, and HLSL compilation.

This is an **early-stage family**, and the libraries are at very different depths.
Each library's own docs carry its current state; this page does not try to track it.

## The libraries

### shaped-graphics — `sg::`

The graphics-API wrapper.
It exposes a small, backend-agnostic surface — `context`, `command_list`, and the GPU resource types — over concrete graphics backends.

- **Backends are separate static libraries**, one per API, under [shaped-graphics/backends/](../libs/graphics/shaped-graphics/backends/).
  Each is built only where the platform and the build allow.
- **sg does not depend on the backends.** The dependency arrow points one way, backends → sg, so there is no `sg::create_context` in the core.
  Each backend library exposes its own `sg::create_<backend>_context(config)` instead.
  See the [context concept doc](../libs/graphics/shaped-graphics/docs/concepts/context.md).
  `backend_kind` is a coarse tag rather than a backend identity: a debug, CPU or remote backend is just as valid as dx12 or vulkan.

Which backends exist, how far along each is, and sg's design at a glance: the [shaped-graphics readme](../libs/graphics/shaped-graphics/readme.md).
The load-bearing conventions are handles, backend smurf naming and the duplication-over-abstraction stance.
They live in its [coding guidelines](../libs/graphics/shaped-graphics/docs/coding-guidelines.md).

### shaped-rendering — `sr::`

Render routines and helpers built on sg: the common building blocks of a renderer — mipmap generation, texture compression, tonemapping.
The **render-routine framework** itself lives in **shaped-graphics**, documented in its [render-routines doc](../libs/graphics/shaped-graphics/docs/render-routines.md).
sr hosts the *concrete* routines built on it, and acquires their shaders through shaped-shader-library.

sr is also home to the **Dear ImGui renderer**: `sr::imgui_context` + `sr::imgui_routine`, drawn entirely through sg over the vendored Dear ImGui + ImPlot + ImGuizmo bundle.
See [imgui.md](../libs/graphics/shaped-rendering/docs/imgui.md).

And sr is where the family meets the OS.
`sr::window_system` / `sr::window` are the **window abstraction**, backed by SDL3 and exposing none of it.
A window's `native_window_handle()` is what `sg::swapchain_description` consumes, so a windowed renderer can be written against sr alone.
Multiple windows are supported today, which is the groundwork for imgui docking and multiple viewports.

- **SDL3 is downloaded on demand**, not vendored.
  The first configure runs [`extern/sdl3/fetch-sdl3.py`](../extern/sdl3/fetch-sdl3.py), which fetches a pinned, SHA-256-verified source release into `extern/sdl3/.install/` and builds it from source.
  That is one code path on every platform, since upstream ships prebuilt dev packages only for Windows and macOS.
  It costs tens of seconds per preset cold and is cached after; `SC_SKIP_SDL3=1` skips it, and the script is where the pinned version lives.
- **On Linux it needs X11 or wayland development headers**, and with X11 every extension SDL uses.
  [CI's system packages](guides/ci.md#system-packages) has the Ubuntu list, and [SDL's own docs](https://wiki.libsdl.org/SDL3/README-linux#build-dependencies) the general one.
  Without them SDL3 is skipped rather than fatal: the rest of sr still configures and builds, just without a window backend.
- **Only the backend is optional, not the API.**
  Without SDL3, `sr::window_system::try_create` fails with a reason rather than the types disappearing, so a caller compiles either way and finds out at run time.
  `SR_HAS_WINDOW` (1/0) says whether a backend was compiled in.

### shaped-viewer — `sv::`

The professional visualization library: a modern, RTX-enabled renderer with a dev-friendly API, serving Shaped Code's visualization needs.
Built on sr, and an early-stage skeleton today — see the [shaped-viewer readme](../libs/graphics/shaped-viewer/readme.md).

### shaped-shader-compiler-dxc — `ssc::dxc::`

A side utility rather than part of the sv → sr → sg chain.
It is a lean wrapper over the DirectX Shader Compiler, turning HLSL into an `sg::compiled_shader`: bytecode, reflected bindings, compute workgroup size.
It depends only on **shaped-graphics**.
Two-step API: `preprocess` resolves `#include`s via a caller-supplied resolver, then `compile` turns already-flattened source into DXIL plus reflection.
**Windows-only** today, since it links DXC and uses the Windows SDK's `d3d12shader.h` reflection.

DXC is downloaded on demand rather than vendored.
The [shaped-shader-compiler-dxc readme](../libs/graphics/shaped-shader-compiler-dxc/readme.md) owns that story: which release is pinned, what gets extracted, and `SC_SKIP_DXC`.
The one consequence worth knowing here is that the release ships `dxil.dll`, so emitted DXIL is signed and runs on dx12 without developer mode.

### shaped-shader-library — `slib::`

The other side utility: the shader **package** and hot-reload mechanism, on top of sg and, where DXC exists, ssc::dxc.
Any target declares its own shaders in its own CMakeLists and gets typed C++ symbols for them — sg does not depend on slib, yet `shaped-graphics-test` declares a package and it works.

```cmake
sc_add_shader_package(TARGET my-renderer NAME my_shaders NAMESPACE my::shaders
                      SOURCE_DIR shaders SHADERS vignette.hlsl:compute:main)
```
```cpp
auto cs = my::shaders::vignette.compute.main->acquire(ctx);   // sg::async_compiled_shader
```

- **You pass the context, not a format.** `acquire(ctx)` picks a registered compiler that reaches a format the context accepts, so one authored shader can feed several backends.
- **Hot reload never blocks a consumer**, and dev-vs-shipping is not a mode flag.

**How it all works — [shaped-graphics/docs/shaders.md](../libs/graphics/shaped-graphics/docs/shaders.md).**
Then the [shaped-shader-library readme](../libs/graphics/shaped-shader-library/readme.md).

## Building & testing

All three build and test through the repo driver like every other library:

```bash
uv run dev.py test "sg "     # just the shaped-graphics tests (also "sr ", "sv ")
uv run dev.py build          # the whole repo, incl. platform-enabled backends
```

[guides/building-and-testing.md](guides/building-and-testing.md) has the full workflow, and [libraries.md](libraries.md) the full library catalog.
